#include "Crash/CommonHeader.h"

#include <Settings.h>
#include <ctime>
#include <fmt/format.h>
#include <shellapi.h>
#include <sstream>

namespace Crash
{
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

	// Auto-open log file with default text viewer
	void auto_open_log(const std::filesystem::path& logPath)
	{
		if (!logPath.empty() && Settings::GetSingleton()->GetDebug().autoOpenCrashLog) {
			// Ensure file exists before trying to open
			if (std::filesystem::exists(logPath)) {
				logger::info("Attempting to auto-open log: {}", logPath.string());
				const std::wstring logPathW = logPath.wstring();
				const auto result = ShellExecuteW(nullptr, L"open", logPathW.c_str(), nullptr, nullptr, SW_SHOW);
				// ShellExecute returns a value <= 32 if it fails
				if (reinterpret_cast<INT_PTR>(result) <= 32) {
					logger::warn("Failed to auto-open log with default handler (error: {}), trying notepad fallback", static_cast<int>(reinterpret_cast<INT_PTR>(result)));
					// Fallback: try opening with notepad explicitly
					const auto fallbackResult = ShellExecuteW(nullptr, L"open", L"notepad.exe", logPathW.c_str(), nullptr, SW_SHOW);
					if (reinterpret_cast<INT_PTR>(fallbackResult) <= 32) {
						logger::error("Failed to auto-open log with notepad fallback (error: {})", static_cast<int>(reinterpret_cast<INT_PTR>(fallbackResult)));
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

	// Create timestamped log file
	std::pair<std::shared_ptr<spdlog::logger>, std::filesystem::path> get_timestamped_log(
		std::string_view a_prefix,
		std::string_view a_logger_name)
	{
		extern std::filesystem::path crashPath;
		std::optional<std::filesystem::path> path = crashPath;
		const auto time = std::time(nullptr);
		std::tm localTime{};
		if (gmtime_s(&localTime, &time) != 0) {
			util::report_and_fail("failed to get current time"sv);
		}

		std::stringstream buf;
		buf << a_prefix << std::put_time(&localTime, "%Y-%m-%d-%H-%M-%S") << ".log"sv;
		*path /= buf.str();

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_st>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>(std::string(a_logger_name), std::move(sink));
		log->set_pattern("%v"s);
		log->set_level(spdlog::level::trace);
		log->flush_on(spdlog::level::off);

		return { log, *path };
	}

}  // namespace Crash
