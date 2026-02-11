#include "Crash/ThreadDump.h"
#include "Crash/CommonHeader.h"

#include "Crash/Analysis.h"
#include "Crash/CrashHandler.h"
#include "Crash/CrashTests.h"
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
	// Runtime crash test type (can be changed via hotkeys)
	static std::atomic<int> g_currentCrashTestType{ 0 };

	std::optional<ThreadData> CollectThreadData(DWORD threadId, size_t index,
		std::span<const module_pointer> a_modules, const std::string& processName, const std::filesystem::path& pluginDir)
	{
		ThreadData data{ threadId, index, {}, 0 };

		HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
			FALSE, threadId);
		if (!thread) {
			return std::nullopt;
		}

		// Suspend for stable capture
		DWORD suspendCount = SuspendThread(thread);
		if (suspendCount == static_cast<DWORD>(-1)) {
			CloseHandle(thread);
			return std::nullopt;
		}

		try {
			// Get thread context
			CONTEXT ctx{};
			ctx.ContextFlags = CONTEXT_FULL;

			if (GetThreadContext(thread, &ctx)) {
				// Collect callstack modules and determine priority
				try {
					// Check RIP first
					const auto rip_mod = Introspection::get_module_for_pointer(reinterpret_cast<void*>(ctx.Rip), a_modules);
					if (rip_mod && rip_mod->in_range(reinterpret_cast<void*>(ctx.Rip))) {
						std::string ripModName(rip_mod->name().data(), rip_mod->name().size());
						data.callstackModules.push_back(ripModName);
						if (ripModName == processName ||
							(ripModName.ends_with(".dll") && std::filesystem::exists(pluginDir / ripModName))) {
							data.priority = 2;  // RIP in game module
						}
					}

					// Walk stack from captured CONTEXT's RSP
					const auto rsp = reinterpret_cast<const std::size_t*>(ctx.Rsp);
					constexpr size_t MAX_STACK_SCAN = 512;  // 4KB of stack

					for (size_t i = 0; i < MAX_STACK_SCAN; ++i) {
						const auto addr = rsp[i];
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
					// Failed to walk stack, but continue
				}
			}
		} catch (...) {
			// Exception during collection
		}

		// Resume thread
		ResumeThread(thread);
		CloseHandle(thread);

		return data;
	}

	void DumpSingleThread(spdlog::logger& a_log, const ThreadData& data,
		std::span<const module_pointer> a_modules)
	{
		a_log.critical("===== THREAD {} (ID: {}) ====="sv, data.index, data.id);

		HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
			FALSE, data.id);
		if (!thread) {
			a_log.critical("\tFailed to open thread (Error: {})"sv, GetLastError());
			a_log.critical(""sv);
			return;
		}

		// Suspend for stable capture
		DWORD suspendCount = SuspendThread(thread);
		if (suspendCount == static_cast<DWORD>(-1)) {
			a_log.critical("\tFailed to suspend thread"sv);
			CloseHandle(thread);
			a_log.critical(""sv);
			return;
		}

		try {
			// Get thread context
			CONTEXT ctx{};
			ctx.ContextFlags = CONTEXT_FULL;

			if (GetThreadContext(thread, &ctx)) {
				// Print registers WITH introspection (shared DRY code)
				// This will show what objects/locks each register points to
				print_registers_safe(a_log, ctx, a_modules);
				a_log.critical(""sv);

				a_log.critical("\tCALLSTACK:"sv);
				try {
					// Collect stack frames: RIP first, then scan stack for return addresses
					std::vector<const void*> frames;
					frames.reserve(65);  // RIP + up to 64 stack frames

					// Add RIP (current instruction pointer)
					frames.push_back(reinterpret_cast<const void*>(ctx.Rip));

					// Walk stack from captured CONTEXT's RSP
					// Scan stack memory for return addresses (limit scan to 4KB / 512 qwords)
					try {
						const auto rsp = reinterpret_cast<const std::size_t*>(ctx.Rsp);
						constexpr size_t MAX_FRAMES = 64;
						constexpr size_t MAX_STACK_SCAN = 512;  // 4KB of stack

						for (size_t i = 0; i < MAX_STACK_SCAN && frames.size() < MAX_FRAMES + 1; ++i) {
							const auto addr = rsp[i];
							const auto mod = Introspection::get_module_for_pointer(reinterpret_cast<void*>(addr), a_modules);
							if (mod && mod->in_range(reinterpret_cast<void*>(addr))) {
								frames.push_back(reinterpret_cast<const void*>(addr));
							}
						}
					} catch (...) {
						// Stack scan failed, continue with just RIP
					}

					// Print detailed callstack with PDB symbols and assembly (shared DRY code)
					print_callstack(a_log, frames, a_modules);

				} catch (const std::exception& e) {
					a_log.critical("\t\tCallstack error: {}"sv, e.what());
				} catch (...) {
					a_log.critical("\t\tStack walk may be incomplete due to access violations"sv);
				}
			} else {
				a_log.critical("\tFailed to get thread context (Error: {})"sv, GetLastError());
			}
		} catch (...) {
			a_log.critical("\tException during thread dump"sv);
		}

		// Resume thread
		ResumeThread(thread);
		CloseHandle(thread);
		a_log.critical(""sv);
	}

	void WriteAllThreadsDump()
	{
		try {
			// Create log file
			auto [log, logPath] = get_timestamped_log("threaddump-"sv, "thread dump"s);

			// Clean up old thread dumps
			const auto& debug = Settings::GetSingleton()->GetDebug();
			clean_old_files(logPath.parent_path(), "threaddump-"sv, ".log", debug.maxCrashLogs, ".dmp");
			clean_old_files(logPath.parent_path(), "threaddump-"sv, ".dmp", debug.maxMinidumps);

			log_common_header_info(*log, "THREAD DUMP (Manual Trigger)", "TIME:"sv);

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
			RE::DebugMessageBox(message.c_str());
			RE::ConsoleLog::GetSingleton()->Print(message.c_str());
			logger::info("{}", message);

			// Auto-open thread dump log if enabled
			auto_open_log(logPath);

		} catch (const std::exception& e) {
			logger::error("Failed to write thread dump: {}"sv, e.what());
		} catch (...) {
			logger::error("Failed to write thread dump: unknown error"sv);
		}
	}

	void HotkeyMonitorThreadFunction()
	{
		const auto& config = Settings::GetSingleton()->GetDebug();

		bool wasThreadDumpPressed = false;
		bool wasCrashTestPressed = false;
		bool wasCrashTypePrevPressed = false;
		bool wasCrashTypeNextPressed = false;
		bool warningShown = false;

		// Initialize runtime crash type from config
		g_currentCrashTestType = config.crashTestType;

		constexpr int numCrashTypes = CrashTests::GetCrashTestCount();

		while (!g_stopHotkeyThread) {
			// Check thread dump hotkey
			if (!config.threadDumpHotkey.empty()) {
				bool allPressed = true;
				for (int vk : config.threadDumpHotkey) {
					if (!(GetAsyncKeyState(vk) & 0x8000)) {
						allPressed = false;
						break;
					}
				}

				if (allPressed && !wasThreadDumpPressed) {
					// Rising edge - keys just pressed
					wasThreadDumpPressed = true;

					try {
						WriteAllThreadsDump();
					} catch (...) {
						logger::error("Failed to write thread dump"sv);
					}
				} else if (!allPressed) {
					wasThreadDumpPressed = false;
				}
			}

			// Check crash test type cycling hotkeys (if crash test enabled)
			if (config.enableCrashTestHotkey) {
				// Ctrl+Shift+PgUp = previous crash type
				const bool ctrlShiftPgUp =
					(GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
					(GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
					(GetAsyncKeyState(VK_PRIOR) & 0x8000);  // VK_PRIOR = Page Up

				if (ctrlShiftPgUp && !wasCrashTypePrevPressed) {
					wasCrashTypePrevPressed = true;
					int newType = (g_currentCrashTestType - 1 + numCrashTypes) % numCrashTypes;
					g_currentCrashTestType = newType;

					const auto message = fmt::format(
						"<< Crash Test Type: {}",
						CrashTests::GetCrashTypeName(newType));

					RE::SendHUDMessage::ShowHUDMessage(message.c_str(), nullptr, false);
					if (auto console = RE::ConsoleLog::GetSingleton()) {
						console->Print(message.c_str());
					}
					logger::info("Crash test type changed to: {}", CrashTests::GetCrashTypeName(newType));
				} else if (!ctrlShiftPgUp) {
					wasCrashTypePrevPressed = false;
				}

				// Ctrl+Shift+PgDn = next crash type
				const bool ctrlShiftPgDn =
					(GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
					(GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
					(GetAsyncKeyState(VK_NEXT) & 0x8000);  // VK_NEXT = Page Down

				if (ctrlShiftPgDn && !wasCrashTypeNextPressed) {
					wasCrashTypeNextPressed = true;
					int newType = (g_currentCrashTestType + 1) % numCrashTypes;
					g_currentCrashTestType = newType;

					const auto message = fmt::format(
						">> Crash Test Type: {}",
						CrashTests::GetCrashTypeName(newType));

					RE::SendHUDMessage::ShowHUDMessage(message.c_str(), nullptr, false);
					if (auto console = RE::ConsoleLog::GetSingleton()) {
						console->Print(message.c_str());
					}
					logger::info("Crash test type changed to: {}", CrashTests::GetCrashTypeName(newType));
				} else if (!ctrlShiftPgDn) {
					wasCrashTypeNextPressed = false;
				}
			}

			// Check crash test hotkey (if enabled)
			if (config.enableCrashTestHotkey && !config.crashTestHotkey.empty()) {
				bool allPressed = true;
				for (int vk : config.crashTestHotkey) {
					if (!(GetAsyncKeyState(vk) & 0x8000)) {
						allPressed = false;
						break;
					}
				}

				if (allPressed && !wasCrashTestPressed) {
					// Rising edge - keys just pressed
					wasCrashTestPressed = true;

					// Show prominent warning ONCE per game session
					if (!warningShown) {
						const int currentType = g_currentCrashTestType.load();
						const auto message = fmt::format(
							"WARNING: CRASH TEST HOTKEY PRESSED!\n\n"
							"This will intentionally CRASH the game for testing.\n"
							"Current Type: {}\n\n"
							"Press the hotkey AGAIN to trigger the crash.\n"
							"Use Ctrl+Shift+PgUp/PgDn to change crash type.\n"
							"(This warning will only show once)",
							CrashTests::GetCrashTypeName(currentType));

						RE::DebugMessageBox(message.c_str());
						warningShown = true;
					} else {
						// Second press - actually trigger the crash
						const int currentType = g_currentCrashTestType.load();
						logger::warn("Crash test hotkey pressed - triggering test crash type: {}",
							CrashTests::GetCrashTypeName(currentType));
						CrashTests::TriggerTestCrash(currentType);
					}
				} else if (!allPressed) {
					wasCrashTestPressed = false;
				}
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	void StartHotkeyMonitoring()
	{
		const auto& config = Settings::GetSingleton()->GetDebug();

		// Check if either feature is enabled
		const bool threadDumpEnabled = config.enableThreadDumpHotkey && !config.threadDumpHotkey.empty();
		const bool crashTestEnabled = config.enableCrashTestHotkey && !config.crashTestHotkey.empty();

		if (!threadDumpEnabled && !crashTestEnabled) {
			logger::info("Hotkey monitoring disabled (no features enabled)"sv);
			return;
		}

		g_stopHotkeyThread = false;
		g_hotkeyThread = std::jthread(HotkeyMonitorThreadFunction);

		// Log which features are enabled
		if (threadDumpEnabled && crashTestEnabled) {
			logger::info("Hotkey monitoring started: Thread Dump (Ctrl+Shift+F12), Crash Test (Ctrl+Shift+F11), Cycle Type (Ctrl+Shift+PgUp/PgDn)"sv);
		} else if (threadDumpEnabled) {
			logger::info("Thread dump hotkey monitoring started (Ctrl+Shift+F12)"sv);
		} else if (crashTestEnabled) {
			logger::info("Crash test hotkey monitoring started (Ctrl+Shift+F11, Ctrl+Shift+PgUp/PgDn to cycle)"sv);
		}
	}

	void StopHotkeyMonitoring()
	{
		g_stopHotkeyThread = true;
		if (g_hotkeyThread.joinable()) {
			g_hotkeyThread.join();
		}
	}

}  // namespace Crash
