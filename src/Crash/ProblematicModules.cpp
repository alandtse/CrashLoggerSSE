#include "Crash/ProblematicModules.h"

#include "Crash/Modules/ModuleHandler.h"
#include <SKSE/Logger.h>
#include <algorithm>
#include <mutex>
#include <unordered_set>

namespace Crash
{
	std::optional<ProblematicModuleInfo> check_problematic_module(std::string_view module_name)
	{
		// Extensible list of problematic modules
		struct ProblematicModule
		{
			std::string_view pattern;
			std::string_view name;
			std::string_view warning;
		};

		static constexpr ProblematicModule problematic_modules[] = {
			{ "skyrimcrashguard.dll",
				"SkyrimCrashGuard",
				"SkyrimCrashGuard attempts to recover from crashes by performing unsafe operations.\n"
				"This can corrupt game state and hide and introduce new subtle bugs.\n"
				"It also intercepts the VEH and may break Crash Logger's ability to process crashes.\n"
				"\n"
				"RECOMMENDED ACTION: Remove SkyrimCrashGuard or seek support from the author at:\n"
				"https://www.nexusmods.com/skyrimspecialedition/mods/172082" }
		};

		// Case-insensitive comparison
		auto ci_equal = [](std::string_view a, std::string_view b) {
			return std::equal(a.begin(), a.end(), b.begin(), b.end(),
				[](char ca, char cb) {
					return ::tolower(static_cast<unsigned char>(ca)) ==
				           ::tolower(static_cast<unsigned char>(cb));
				});
		};

		for (const auto& problematic : problematic_modules) {
			if (module_name.length() == problematic.pattern.length() &&
				ci_equal(module_name, problematic.pattern)) {
				return ProblematicModuleInfo{ problematic.name, problematic.warning };
			}
		}

		return std::nullopt;
	}

	void log_problematic_module_warning(spdlog::logger& logger, const ProblematicModuleInfo& info, bool is_crash_log, bool show_popup)
	{
		using namespace std::literals;

		// Track which modules we've warned about to avoid duplicates across:
		// 1. Startup warning (main log)
		// 2. Crash log warning (if crash occurs)
		// Whichever happens first will emit the warning; subsequent calls are silent
		static std::unordered_set<std::string_view> warned_modules;
		static std::mutex warned_mutex;
		static constexpr std::string_view crash_logger_warning = "Crash Logger may not function correctly with this module loaded."sv;
		static constexpr std::string_view crash_reports_warning = "Crash reports may be incomplete, inaccurate, or missing entirely."sv;

		{
			std::lock_guard lock(warned_mutex);
			if (!warned_modules.insert(info.name).second) {
				// Already warned about this module in a previous log
				return;
			}
		}

		logger.critical(""sv);
		logger.critical("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"sv);
		logger.critical("!!! WARNING: {} DETECTED !!!"sv, info.name);
		logger.critical("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"sv);
		logger.critical(""sv);
		logger.critical("{}"sv, info.warning);
		logger.critical(""sv);
		logger.critical("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"sv);
		logger.critical(""sv);

		if (!is_crash_log) {
			logger.critical(crash_logger_warning);
			logger.critical(crash_reports_warning);
			logger.critical(""sv);
		}

		// Show popup and console log for startup warnings
		if (show_popup) {
			const auto message = fmt::format(
				"WARNING: {} DETECTED\n\n"
				"{}\n\n"
				"{}\n"
				"{}\n\n",
				info.name,
				info.warning,
				crash_logger_warning,
				crash_reports_warning);

			RE::DebugMessageBox(message.c_str());
			if (auto console = RE::ConsoleLog::GetSingleton()) {
				console->Print(message.c_str());
			}
		}
	}
}
