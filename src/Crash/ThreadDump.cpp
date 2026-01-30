#include "Crash/ThreadDump.h"

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

	[[nodiscard]] std::pair<std::shared_ptr<spdlog::logger>, std::filesystem::path> get_thread_dump_log()
	{
		std::optional<std::filesystem::path> path = crashPath;
		const auto time = std::time(nullptr);
		std::tm localTime{};
		if (gmtime_s(&localTime, &time) != 0) {
			util::report_and_fail("failed to get current time"sv);
		}

		std::stringstream buf;
		buf << "threaddump-"sv << std::put_time(&localTime, "%Y-%m-%d-%H-%M-%S") << ".log"sv;
		*path /= buf.str();

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_st>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("thread dump"s, std::move(sink));
		log->set_pattern("%v"s);
		log->set_level(spdlog::level::trace);
		log->flush_on(spdlog::level::off);

		return { log, *path };
	}

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
				// Print registers
				a_log.critical("\tREGISTERS:"sv);
				a_log.critical("\t\tRAX: 0x{:016X}"sv, ctx.Rax);
				a_log.critical("\t\tRCX: 0x{:016X}"sv, ctx.Rcx);
				a_log.critical("\t\tRDX: 0x{:016X}"sv, ctx.Rdx);
				a_log.critical("\t\tRBX: 0x{:016X}"sv, ctx.Rbx);
				a_log.critical("\t\tRSP: 0x{:016X}"sv, ctx.Rsp);
				a_log.critical("\t\tRBP: 0x{:016X}"sv, ctx.Rbp);
				a_log.critical("\t\tRSI: 0x{:016X}"sv, ctx.Rsi);
				a_log.critical("\t\tRDI: 0x{:016X}"sv, ctx.Rdi);
				a_log.critical("\t\tR8:  0x{:016X}"sv, ctx.R8);
				a_log.critical("\t\tR9:  0x{:016X}"sv, ctx.R9);
				a_log.critical("\t\tR10: 0x{:016X}"sv, ctx.R10);
				a_log.critical("\t\tR11: 0x{:016X}"sv, ctx.R11);
				a_log.critical("\t\tR12: 0x{:016X}"sv, ctx.R12);
				a_log.critical("\t\tR13: 0x{:016X}"sv, ctx.R13);
				a_log.critical("\t\tR14: 0x{:016X}"sv, ctx.R14);
				a_log.critical("\t\tR15: 0x{:016X}"sv, ctx.R15);
				a_log.critical("\t\tRIP: 0x{:016X}"sv, ctx.Rip);

				a_log.critical("\tCALLSTACK:"sv);
				try {
					// Print RIP first (current instruction pointer)
					const auto rip_mod = Introspection::get_module_for_pointer(reinterpret_cast<void*>(ctx.Rip), a_modules);
					if (rip_mod && rip_mod->in_range(reinterpret_cast<void*>(ctx.Rip))) {
						a_log.critical("\t\t[0] 0x{:012X} {}+{:07X}"sv,
							ctx.Rip,
							rip_mod->name(),
							static_cast<std::uintptr_t>(ctx.Rip - rip_mod->address()));
					} else {
						a_log.critical("\t\t[0] 0x{:012X}"sv, ctx.Rip);
					}

					// Walk stack from captured CONTEXT's RSP
					// Scan stack memory for return addresses (limit scan to 4KB / 512 qwords)
					const auto rsp = reinterpret_cast<const std::size_t*>(ctx.Rsp);
					size_t frameNum = 1;
					size_t stackFramesPrinted = 0;
					constexpr size_t MAX_FRAMES = 64;
					constexpr size_t MAX_STACK_SCAN = 512;  // 4KB of stack

					for (size_t i = 0; i < MAX_STACK_SCAN && frameNum < MAX_FRAMES; ++i) {
						const auto addr = rsp[i];
						const auto mod = Introspection::get_module_for_pointer(reinterpret_cast<void*>(addr), a_modules);
						if (mod && mod->in_range(reinterpret_cast<void*>(addr))) {
							a_log.critical("\t\t[{}] 0x{:012X} {}+{:07X}"sv,
								frameNum++,
								addr,
								mod->name(),
								static_cast<std::uintptr_t>(addr - mod->address()));
							++stackFramesPrinted;
						}
					}

					if (stackFramesPrinted == 0) {
						a_log.critical("\t\tNo additional stack frames found"sv);
					}
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

	// Common logging function for header information
	void log_common_header_info(spdlog::logger& a_log, std::string_view title, std::string_view time_prefix)
	{
		// Add title header if provided
		if (!title.empty()) {
			a_log.critical("========================================"sv);
			a_log.critical("{}"sv, title);
			a_log.critical("========================================"sv);
		}

		// Add timestamp
		const auto now = std::time(nullptr);
		std::tm local_time{};
		if (localtime_s(&local_time, &now) == 0) {
			a_log.critical("{} {:04}-{:02}-{:02} {:02}:{:02}:{:02}"sv, time_prefix,
				local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
				local_time.tm_hour, local_time.tm_min, local_time.tm_sec);
		}

		// Add version information
		const auto runtimeVer = REL::Module::get().version();
		a_log.critical("Skyrim {} v{}.{}.{}"sv, REL::Module::IsVR() ? "VR" : "SSE", runtimeVer[0], runtimeVer[1], runtimeVer[2]);

		// Always include build time next to version
		a_log.critical("CrashLoggerSSE v{} {} {}"sv, SKSE::PluginDeclaration::GetSingleton()->GetVersion().string(), __DATE__, __TIME__);

		a_log.critical(""sv);
	}

	void WriteAllThreadsDump()
	{
		try {
			// Create log file
			auto [log, logPath] = get_thread_dump_log();

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
			std::string message{ "Thread dump written to: " };
			message.append(logPath.string());
			RE::DebugMessageBox(message.c_str());
			RE::ConsoleLog::GetSingleton()->Print(message.c_str());
			logger::info("{}", message);

			// Auto-open thread dump log if enabled
			autoOpenLog(logPath);

		} catch (const std::exception& e) {
			logger::error("Failed to write thread dump: {}"sv, e.what());
		} catch (...) {
			logger::error("Failed to write thread dump: unknown error"sv);
		}
	}

	// Helper function to auto-open log files
	void autoOpenLog(const std::filesystem::path& logPath)
	{
		if (!logPath.empty() && Settings::GetSingleton()->GetDebug().autoOpenCrashLog) {
			// Ensure file exists before trying to open
			if (std::filesystem::exists(logPath)) {
				logger::info("Attempting to auto-open log: {}", logPath.string());
				const std::wstring logPathW = logPath.wstring();
				const auto result = ShellExecuteW(nullptr, L"open", logPathW.c_str(), nullptr, nullptr, SW_SHOW);
				// ShellExecute returns a value <= 32 if it fails
				if (reinterpret_cast<INT_PTR>(result) <= 32) {
					logger::warn("Failed to auto-open log with default handler (error: {0}), trying notepad fallback", static_cast<int>(reinterpret_cast<INT_PTR>(result)));
					// Fallback: try opening with notepad explicitly
					const auto fallbackResult = ShellExecuteW(nullptr, L"open", L"notepad.exe", logPathW.c_str(), nullptr, SW_SHOW);
					if (reinterpret_cast<INT_PTR>(fallbackResult) <= 32) {
						logger::error("Failed to auto-open log with notepad fallback (error: {0})", static_cast<int>(reinterpret_cast<INT_PTR>(fallbackResult)));
					} else {
						logger::info("Successfully auto-opened log with notepad");
					}
				} else {
					logger::info("Successfully auto-opened log with default handler");
				}
			} else {
				logger::warn("Log file does not exist, cannot auto-open: {}", logPath.string());
			}
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
		logger::info("Thread dump hotkey monitoring started (Ctrl+Shift+F12)"sv);
	}

	void StopHotkeyMonitoring()
	{
		g_stopHotkeyThread = true;
		if (g_hotkeyThread.joinable()) {
			g_hotkeyThread.join();
		}
	}

}  // namespace Crash
