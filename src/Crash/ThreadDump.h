#pragma once

#include "Crash/CommonHeader.h"
#include "Crash/Modules/ModuleHandler.h"
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace spdlog
{
	class logger;
}

namespace Crash
{
	struct ThreadData
	{
		DWORD id;
		size_t index;
		std::vector<std::string> callstackModules;
		int priority;  // 2: RIP in game module, 1: any in stack, 0: none
	};

	[[nodiscard]] std::pair<std::shared_ptr<spdlog::logger>, std::filesystem::path> get_thread_dump_log();
	std::optional<ThreadData> CollectThreadData(DWORD threadId, size_t index, std::span<const module_pointer> a_modules, const std::string& processName, const std::filesystem::path& pluginDir);
	void DumpSingleThread(spdlog::logger& a_log, const ThreadData& data, std::span<const module_pointer> a_modules);
	void WriteAllThreadsDump();
	void autoOpenLog(const std::filesystem::path& logPath);
	void HotkeyMonitorThreadFunction();
	void StartHotkeyMonitoring();
	void StopHotkeyMonitoring();

}  // namespace Crash
