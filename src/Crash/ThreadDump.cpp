#include "Crash/ThreadDump.h"
#include "Crash/CommonHeader.h"

#include "Crash/Analysis.h"
#include "Crash/Introspection/Introspection.h"
#include "Crash/Modules/ModuleHandler.h"
#include "Crash/PDB/PdbHandler.h"
#include "RE/C/ConsoleLog.h"
#include "RE/S/SendHUDMessage.h"
#include <Settings.h>
#include <TlHelp32.h>
#include <filesystem>
#include <format>
#include <shellapi.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <thread>
#include <tlhelp32.h>
#include <windows.h>

namespace Crash
{
	extern std::filesystem::path crashPath;
	// Static variables for hotkey monitoring
	static std::atomic<bool> g_stopHotkeyThread{ false };
	static std::jthread g_hotkeyThread;

	namespace
	{
		// Takes the thread's registers and top of stack while it is suspended. A suspended thread may
		// hold the heap, loader or logger lock, so nothing here allocates or takes a lock: anything
		// that does (symbols, strings, logging) would deadlock the dump the moment it needs that lock.
		bool CaptureThread(DWORD a_threadId, ThreadSnapshot& a_out) noexcept
		{
			const auto thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
				FALSE, a_threadId);
			if (!thread) {
				return false;
			}
			bool ok = false;
			if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
				a_out.ctx.ContextFlags = CONTEXT_FULL;
				if (GetThreadContext(thread, &a_out.ctx)) {
					SIZE_T read = 0;
					ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a_out.ctx.Rsp),
						a_out.stack, sizeof(a_out.stack), &read);
					a_out.stackCount = read / sizeof(std::size_t);
					ok = true;
				}
				ResumeThread(thread);
			}
			CloseHandle(thread);
			return ok;
		}
	}

	std::optional<ThreadData> CollectThreadData(DWORD threadId, size_t index,
		std::span<const module_pointer> a_modules, const std::string& processName, const std::filesystem::path& pluginDir)
	{
		ThreadData data{ threadId, index, {}, 0, std::make_shared<ThreadSnapshot>() };

		if (!CaptureThread(threadId, *data.snapshot)) {
			return std::nullopt;
		}

		// The thread is running again; everything below is free to allocate.
		const auto& ctx = data.snapshot->ctx;
		try {
			const auto rip_mod = Introspection::get_module_for_pointer(reinterpret_cast<void*>(ctx.Rip), a_modules);
			if (rip_mod && rip_mod->in_range(reinterpret_cast<void*>(ctx.Rip))) {
				std::string ripModName(rip_mod->name().data(), rip_mod->name().size());
				data.callstackModules.push_back(ripModName);
				if (ripModName == processName ||
					(ripModName.ends_with(".dll") && std::filesystem::exists(pluginDir / ripModName))) {
					data.priority = 2;  // RIP in game module
				}
			}

			for (std::size_t i = 0; i < data.snapshot->stackCount; ++i) {
				const auto addr = data.snapshot->stack[i];
				const auto mod = Introspection::get_module_for_pointer(reinterpret_cast<void*>(addr), a_modules);
				if (mod && mod->in_range(reinterpret_cast<void*>(addr))) {
					std::string modName(mod->name().data(), mod->name().size());
					if (std::find(data.callstackModules.begin(), data.callstackModules.end(), modName) == data.callstackModules.end()) {
						data.callstackModules.push_back(modName);
						if (data.priority < 2 && (modName == processName ||
													 (modName.ends_with(".dll") && std::filesystem::exists(pluginDir / modName)))) {
							data.priority = data.priority == 0 ? 1 : data.priority;  // Any in stack, but not overriding RIP priority
						}
					}
				}
			}
		} catch (...) {
			// Failed to walk the stack, but continue
		}

		return data;
	}

	void DumpSingleThread(spdlog::logger& a_log, const ThreadData& data,
		std::span<const module_pointer> a_modules)
	{
		a_log.critical("===== THREAD {} (ID: {}) ====="sv, data.index, data.id);

		if (!data.snapshot) {
			a_log.critical("	No thread snapshot"sv);
			a_log.critical(""sv);
			return;
		}

		try {
			const auto& ctx = data.snapshot->ctx;

			// Print registers WITH introspection (shared DRY code)
			print_registers_safe(a_log, ctx, a_modules);
			a_log.critical(""sv);

			a_log.critical("	CALLSTACK:"sv);
			try {
				// RIP first, then return addresses found in the captured stack
				std::vector<const void*> frames;
				constexpr size_t MAX_FRAMES = 64;
				frames.reserve(MAX_FRAMES + 1);
				frames.push_back(reinterpret_cast<const void*>(ctx.Rip));

				for (std::size_t i = 0; i < data.snapshot->stackCount && frames.size() < MAX_FRAMES + 1; ++i) {
					const auto addr = data.snapshot->stack[i];
					const auto mod = Introspection::get_module_for_pointer(reinterpret_cast<void*>(addr), a_modules);
					if (mod && mod->in_range(reinterpret_cast<void*>(addr))) {
						frames.push_back(reinterpret_cast<const void*>(addr));
					}
				}

				print_callstack(a_log, frames, a_modules);
			} catch (const std::exception& e) {
				a_log.critical("		Callstack error: {}"sv, e.what());
			} catch (...) {
				a_log.critical("		Stack walk may be incomplete"sv);
			}
		} catch (...) {
			a_log.critical("	Exception during thread dump"sv);
		}

		a_log.critical(""sv);
		a_log.flush();  // keep partial output on disk if a later thread stalls
	}

	void WriteAllThreadsDump(std::string_view a_trigger, std::string_view a_context, bool a_interactive)
	{
		try {
			const auto prefix = a_interactive ? "threaddump-"sv : "freezedump-"sv;
			auto [log, logPath] = get_timestamped_log(prefix, "thread dump"s);

			// Clean up old thread dumps
			const auto& debug = Settings::GetSingleton()->GetDebug();
			clean_old_files(logPath.parent_path(), prefix, ".log", debug.maxCrashLogs, ".dmp");
			clean_old_files(logPath.parent_path(), prefix, ".dmp", debug.maxMinidumps);

			log_common_header_info(*log, std::format("THREAD DUMP ({})", a_trigger), "TIME:"sv);
			if (!a_context.empty()) {
				log->critical("{}"sv, a_context);
				log->critical(""sv);
			}

			// Get loaded modules
			const auto modules = Modules::get_loaded_modules();
			const std::span cmodules{ modules.begin(), modules.end() };

			// Get process name and plugin dir for heuristics
			std::filesystem::path exePath = REL::Module::get().filename();
			std::string processName = exePath.string();
			std::filesystem::path pluginDir{ Crash::PDB::sPluginPath };

			// Enumerate all threads
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snapshot == INVALID_HANDLE_VALUE) {
				log->critical("Failed to create thread snapshot"sv);
				log->flush();
				return;
			}

			THREADENTRY32 te{};
			te.dwSize = sizeof(te);

			DWORD currentProcessId = GetCurrentProcessId();
			DWORD currentThreadId = GetCurrentThreadId();
			std::vector<DWORD> threadIds;

			if (Thread32First(snapshot, &te)) {
				do {
					if (te.th32OwnerProcessID == currentProcessId) {
						threadIds.push_back(te.th32ThreadID);
					}
				} while (Thread32Next(snapshot, &te));
			}
			CloseHandle(snapshot);

			log->critical("Total Threads: {}"sv, threadIds.size());
			log->critical(""sv);

			// Collect thread data
			std::vector<ThreadData> threadDataList;
			for (size_t i = 0; i < threadIds.size(); ++i) {
				if (threadIds[i] == currentThreadId) {
					// Skip current thread for now, handle separately
					continue;
				}

				auto data = CollectThreadData(threadIds[i], i + 1, cmodules, processName, pluginDir);
				if (data) {
					threadDataList.push_back(*data);
				}
			}

			// Sort threads: by priority descending (2 > 1 > 0), then by index
			std::sort(threadDataList.begin(), threadDataList.end(),
				[](const ThreadData& a, const ThreadData& b) {
					if (a.priority != b.priority) {
						return a.priority > b.priority;
					}
					return a.index < b.index;
				});

			// Dump sorted threads
			for (const auto& data : threadDataList) {
				DumpSingleThread(*log, data, cmodules);
			}

			// Handle current thread last
			size_t currentIndex = 0;
			for (size_t i = 0; i < threadIds.size(); ++i) {
				if (threadIds[i] == currentThreadId) {
					currentIndex = i + 1;
					break;
				}
			}
			log->critical("===== THREAD {} (ID: {}) [CURRENT THREAD] ====="sv, currentIndex, currentThreadId);
			log->critical(""sv);

			log->flush();

			// Write minidump if requested
			bool minidumpWritten = false;
			if (Settings::GetSingleton()->GetDebug().threadDumpWriteMinidump) {
				try {
					auto dumpPath = logPath;
					dumpPath.replace_extension(".dmp");
					if (write_minidump(dumpPath)) {
						log->critical("Minidump written to: {}", dumpPath.string());
						log->flush();
						minidumpWritten = true;
					} else {
						log->critical("Failed to write minidump to: {}", dumpPath.string());
						log->flush();
					}
				} catch (...) {
					log->critical("Exception while writing minidump");
					log->flush();
				}
			}

			std::string message{ "Thread dump written to: " };
			message.append(logPath.string());
			if (minidumpWritten) {
				message.append("\nMinidump: ");
				auto dumpPath = logPath;
				dumpPath.replace_extension(".dmp");
				message.append(dumpPath.string());
			}
			logger::info("{}", message);

			// Touching UI and spawning viewers is only safe for a user-requested dump; an automatic
			// freeze dump runs while the main thread may be stuck.
			if (a_interactive) {
				RE::DebugMessageBox(message.c_str());
				RE::ConsoleLog::GetSingleton()->Print(message.c_str());
				auto_open_log(logPath);
			}

		} catch (const std::exception& e) {
			logger::error("Failed to write thread dump: {}"sv, e.what());
		} catch (...) {
			logger::error("Failed to write thread dump: unknown error"sv);
		}
	}

	void HotkeyMonitorThreadFunction()
	{
		const auto& config = Settings::GetSingleton()->GetDebug();

		bool wasPressed = false;

		while (!g_stopHotkeyThread) {
			// Check if all keys are pressed
			bool allPressed = true;
			for (int vk : config.threadDumpHotkey) {
				if (!(GetAsyncKeyState(vk) & 0x8000)) {
					allPressed = false;
					break;
				}
			}

			if (allPressed && !wasPressed) {
				// Rising edge - keys just pressed
				wasPressed = true;

				try {
					WriteAllThreadsDump();
				} catch (...) {
					logger::error("Failed to write thread dump"sv);
				}
			} else if (!allPressed) {
				wasPressed = false;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	void StartHotkeyMonitoring()
	{
		const auto& config = Settings::GetSingleton()->GetDebug();

		// Only create thread if feature is enabled AND hotkey is configured
		if (!config.enableThreadDumpHotkey) {
			logger::info("Thread dump hotkey disabled"sv);
			return;
		}

		if (config.threadDumpHotkey.empty()) {
			logger::info("Thread dump hotkey not configured, monitoring thread not created"sv);
			return;
		}

		g_stopHotkeyThread = false;
		g_hotkeyThread = std::jthread(HotkeyMonitorThreadFunction);

		std::string hotkeyNames;
		for (size_t i = 0; i < config.threadDumpHotkey.size(); ++i) {
			if (i > 0) {
				hotkeyNames += "+";
			}
			int vk = config.threadDumpHotkey[i];
			if (vk == VK_CONTROL || vk == 17) {
				hotkeyNames += "Ctrl";
			} else if (vk == VK_SHIFT || vk == 16) {
				hotkeyNames += "Shift";
			} else if (vk == VK_MENU || vk == 18) {
				hotkeyNames += "Alt";
			} else if (vk >= VK_F1 && vk <= VK_F24) {
				hotkeyNames += std::format("F{}", vk - VK_F1 + 1);
			} else if (vk >= 'A' && vk <= 'Z') {
				hotkeyNames += static_cast<char>(vk);
			} else if (vk >= '0' && vk <= '9') {
				hotkeyNames += static_cast<char>(vk);
			} else {
				hotkeyNames += std::to_string(vk);
			}
		}
		logger::info("Thread dump hotkey monitoring started ({})", hotkeyNames);
	}

	void StopHotkeyMonitoring()
	{
		g_stopHotkeyThread = true;
		if (g_hotkeyThread.joinable()) {
			g_hotkeyThread.join();
		}
	}

}  // namespace Crash
