#pragma once

#include <spdlog/spdlog.h>
#include <string_view>
#include <vector>

namespace Crash
{
	namespace Modules
	{
		class Module;
	}

	// Logs shared header info used by crash logs and thread dumps.
	// Keeps the exact same output format (timestamp, Skyrim runtime version, plugin build info).
	void log_common_header_info(spdlog::logger& a_log, std::string_view title, std::string_view time_prefix);

	// Auto-open log file with default text viewer (shared by crash logs and thread dumps)
	void auto_open_log(const std::filesystem::path& logPath);

	// Create timestamped log file (shared by crash logs and thread dumps)
	[[nodiscard]] std::pair<std::shared_ptr<spdlog::logger>, std::filesystem::path> get_timestamped_log(
		std::string_view a_prefix,
		std::string_view a_logger_name);

	// Upload log file to pastebin.com, copy URL to clipboard, and open in browser
	// Returns the paste URL if successful, empty string otherwise
	[[nodiscard]] std::string upload_log_to_pastebin(const std::filesystem::path& logPath);

	// Clean up old files in the directory matching prefix and extension
	// keeps the N most recent files
	// If associated_extension is provided, deletes that too (e.g. delete .dmp when deleting .log)
	void clean_old_files(const std::filesystem::path& directory, std::string_view prefix, std::string_view extension, int max_count, std::string_view associated_extension = "");

	// Copy text to Windows clipboard
	bool copy_to_clipboard(const std::string& text);

	// Detect problematic crash recovery DLLs and log warnings
	// Returns true if any problematic DLLs were detected
	[[nodiscard]] bool detect_and_log_problematic_dlls(spdlog::logger& a_log, std::span<const std::unique_ptr<Modules::Module>> a_modules);

}  // namespace Crash
