#pragma once

#include "Crash/Modules/ModuleHandler.h"
#include <optional>
#include <span>
#include <string_view>

namespace spdlog
{
	class logger;
}

namespace Crash
{
	struct ProblematicModuleInfo
	{
		std::string_view name;
		std::string_view warning;
	};

	// Check if a module name matches known problematic patterns
	std::optional<ProblematicModuleInfo> check_problematic_module(std::string_view module_name);

	// Check for problematic modules in loaded module list
	std::optional<ProblematicModuleInfo> find_problematic_module(std::span<const module_pointer> modules);

	// Check for problematic modules in a list of filenames/module names
	template <typename Container>
	std::optional<ProblematicModuleInfo> find_problematic_module_in_names(const Container& names)
	{
		for (const auto& name : names) {
			// Handle both std::string and std::string_view
			std::string_view name_view;
			if constexpr (std::is_same_v<std::decay_t<decltype(name)>, std::string>) {
				name_view = name;
			} else {
				name_view = name;
			}

			if (auto warning = check_problematic_module(name_view)) {
				return warning;
			}
		}
		return std::nullopt;
	}

	// Log a formatted warning banner for a problematic module
	// If show_popup is true, also displays a message box and console log (for startup warnings)
	void log_problematic_module_warning(spdlog::logger& logger, const ProblematicModuleInfo& info, bool is_crash_log = false, bool show_popup = false);
}
