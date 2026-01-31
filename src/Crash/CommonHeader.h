#pragma once

#include <spdlog/spdlog.h>
#include <string_view>

namespace Crash
{
	// Logs shared header info used by crash logs and thread dumps.
	// Keeps the exact same output format (timestamp, Skyrim runtime version, plugin build info).
	void log_common_header_info(spdlog::logger& a_log, std::string_view title, std::string_view time_prefix);

	// Auto-open log file with default text viewer (shared by crash logs and thread dumps)
	void auto_open_log(const std::filesystem::path& logPath);

	// Create timestamped log file (shared by crash logs and thread dumps)
	[[nodiscard]] std::pair<std::shared_ptr<spdlog::logger>, std::filesystem::path> get_timestamped_log(
		std::string_view a_prefix,
		std::string_view a_logger_name);

}  // namespace Crash
