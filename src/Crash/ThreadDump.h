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
	// Raw state of a thread captured while it was suspended (plain data, no allocation).
	struct ThreadSnapshot
	{
		static constexpr std::size_t MAX_STACK_QWORDS = 512;  // 4KB

		CONTEXT ctx{};
		std::size_t stack[MAX_STACK_QWORDS]{};
		std::size_t stackCount{ 0 };
	};

	struct ThreadData
	{
		DWORD id;
		size_t index;
		std::vector<std::string> callstackModules;
		int priority;  // 2: RIP in game module, 1: any in stack, 0: none
		std::shared_ptr<ThreadSnapshot> snapshot;
	};

	std::optional<ThreadData> CollectThreadData(DWORD threadId, size_t index, std::span<const module_pointer> a_modules, const std::string& processName, const std::filesystem::path& pluginDir);
	void DumpSingleThread(spdlog::logger& a_log, const ThreadData& data, std::span<const module_pointer> a_modules);
	void WriteAllThreadsDump(std::string_view a_trigger = "Manual Trigger", std::string_view a_context = {}, bool a_interactive = true);
	void HotkeyMonitorThreadFunction();
	void StartHotkeyMonitoring();
	void StopHotkeyMonitoring();

}  // namespace Crash
