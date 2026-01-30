#pragma once

#include <spdlog/spdlog.h>
#include <string_view>

namespace Crash
{
	// Logs shared header info used by crash logs and thread dumps.
	// Keeps the exact same output format (timestamp, Skyrim runtime version, plugin build info).
	void log_common_header_info(spdlog::logger& a_log, std::string_view title, std::string_view time_prefix);
}  // namespace Crash
