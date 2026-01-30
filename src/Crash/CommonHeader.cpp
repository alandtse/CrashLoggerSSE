#include "Crash/CommonHeader.h"

#include <ctime>
#include <fmt/format.h>

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
}  // namespace Crash
