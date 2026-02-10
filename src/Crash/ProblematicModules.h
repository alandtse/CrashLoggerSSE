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

	// Unified function to check for problematic modules in any collection
	// Works with loaded modules (span<module_pointer>) or string collections (set<string_view>, etc.)
	template <typename Container>
	std::optional<ProblematicModuleInfo> find_problematic_module(const Container& items)
	{
		for (const auto& item : items) {
			std::string_view name_view;

			// Extract name based on item type
			using ItemType = std::decay_t<decltype(item)>;
			if constexpr (std::is_pointer_v<ItemType>) {
				// module_pointer case: dereference and call name()
				name_view = item->name();
			} else if constexpr (std::is_convertible_v<ItemType, std::string_view>) {
				// Direct string_view or string case
				name_view = item;
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
