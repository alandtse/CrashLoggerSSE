#include "Crash/CommonHeader.h"

#include <Settings.h>
#include <Psapi.h>
#include <algorithm>
#include <ctime>
#include <fmt/format.h>
#include <fstream>
#include <iomanip>
#include <shellapi.h>
#include <sstream>
#include <winhttp.h>

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
		if (localtime_s(&localTime, &time) != 0) {
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

	// Clean up old files in the directory matching prefix and extension
	void clean_old_files(const std::filesystem::path& directory, std::string_view prefix, std::string_view extension, int max_count, std::string_view associated_extension)
	{
		if (max_count <= 0)
			return;

		try {
			if (!std::filesystem::exists(directory))
				return;

			struct FileEntry
			{
				std::filesystem::path path;
				std::filesystem::file_time_type time;
			};

			std::vector<FileEntry> files;

			// Collect all files matching the prefix and extension
			for (const auto& entry : std::filesystem::directory_iterator(directory)) {
				if (entry.is_regular_file()) {
					const auto filename = entry.path().filename().string();
					if (filename.starts_with(prefix) && filename.ends_with(extension)) {
						std::error_code ec;
						const auto time = std::filesystem::last_write_time(entry.path(), ec);
						if (!ec) {
							files.push_back({ entry.path(), time });
						}
					}
				}
			}

			if (static_cast<int>(files.size()) <= max_count) {
				return;
			}

			// Sort by time descending (newest first)
			std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) {
				return a.time > b.time;
			});

			// Identify files to delete (from index max_count onwards)
			for (size_t i = max_count; i < files.size(); ++i) {
				const auto& file = files[i];

				std::filesystem::remove(file.path);
				logger::info("Cleaned up old file: {}", file.path.filename().string());

				// Try to delete associated file if requested
				if (!associated_extension.empty()) {
					auto assocPath = file.path;
					assocPath.replace_extension(associated_extension);
					if (std::filesystem::exists(assocPath)) {
						std::filesystem::remove(assocPath);
						logger::info("Cleaned up associated file: {}", assocPath.filename().string());
					}
				}
			}

		} catch (const std::exception& e) {
			logger::error("Failed to clean old files: {}", e.what());
		}
	}

	// Copy text to Windows clipboard
	bool copy_to_clipboard(const std::string& text)
	{
		try {
			if (!OpenClipboard(nullptr)) {
				return false;
			}

			EmptyClipboard();

			const size_t len = text.length() + 1;
			HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
			if (!hMem) {
				CloseClipboard();
				return false;
			}

			void* locked = GlobalLock(hMem);
			if (!locked) {
				GlobalFree(hMem);
				CloseClipboard();
				return false;
			}
			memcpy(locked, text.c_str(), len);
			GlobalUnlock(hMem);

			SetClipboardData(CF_TEXT, hMem);
			CloseClipboard();

			return true;
		} catch (...) {
			CloseClipboard();
			return false;
		}
	}

	// URL encode a string for HTTP POST
	std::string url_encode(const std::string& value)
	{
		std::ostringstream escaped;
		escaped.fill('0');
		escaped << std::hex;

		for (char c : value) {
			// Keep alphanumeric and other safe characters
			if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
				escaped << c;
			} else {
				// Encode everything else
				escaped << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
			}
		}

		return escaped.str();
	}

	// Upload log file to pastebin.com and copy URL to clipboard
	std::string upload_log_to_pastebin(const std::filesystem::path& logPath)
	{
		try {
			// Get API key from settings
			const auto& settings = Settings::GetSingleton()->GetDebug();
			if (settings.pastebinApiKey.empty()) {
				logger::error("Pastebin API key not configured. Get one from https://pastebin.com/doc_api#1");
				return ""s;
			}

			// Read log file
			std::ifstream logFile(logPath);
			if (!logFile.is_open()) {
				logger::error("Failed to open log file for upload: {}", logPath.string());
				return ""s;
			}

			std::stringstream buffer;
			buffer << logFile.rdbuf();
			std::string logContent = buffer.str();

			// Check size limit (512KB for pastebin.com)
			if (logContent.size() > 512 * 1024) {
				logger::warn("Log file too large for pastebin.com ({}bytes), truncating", logContent.size());
				logContent = logContent.substr(0, 512 * 1024);
				logContent += "\n\n[LOG TRUNCATED - File too large for pastebin.com]";
			}

			// Build POST data for pastebin.com API
			std::string postData = fmt::format(
				"api_dev_key={}&api_option=paste&api_paste_code={}&api_paste_private=1&api_paste_name={}&api_paste_expire_date=1W",
				url_encode(settings.pastebinApiKey),
				url_encode(logContent),
				url_encode(fmt::format("CrashLogger - {}", logPath.filename().string())));

			// Initialize WinHTTP
			HINTERNET hSession = WinHttpOpen(
				L"CrashLoggerSSE/1.0",
				WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
				WINHTTP_NO_PROXY_NAME,
				WINHTTP_NO_PROXY_BYPASS,
				0);

			if (!hSession) {
				logger::error("WinHttpOpen failed");
				return ""s;
			}

			// Connect to pastebin.com
			HINTERNET hConnect = WinHttpConnect(
				hSession,
				L"pastebin.com",
				INTERNET_DEFAULT_HTTPS_PORT,
				0);

			if (!hConnect) {
				logger::error("WinHttpConnect failed");
				WinHttpCloseHandle(hSession);
				return ""s;
			}

			// Open request
			HINTERNET hRequest = WinHttpOpenRequest(
				hConnect,
				L"POST",
				L"/api/api_post.php",
				nullptr,
				WINHTTP_NO_REFERER,
				WINHTTP_DEFAULT_ACCEPT_TYPES,
				WINHTTP_FLAG_SECURE);

			if (!hRequest) {
				logger::error("WinHttpOpenRequest failed");
				WinHttpCloseHandle(hConnect);
				WinHttpCloseHandle(hSession);
				return ""s;
			}

			// Set headers for form data
			std::wstring headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
			WinHttpAddRequestHeaders(
				hRequest,
				headers.c_str(),
				static_cast<DWORD>(-1),
				WINHTTP_ADDREQ_FLAG_ADD);

			// Send request
			BOOL result = WinHttpSendRequest(
				hRequest,
				WINHTTP_NO_ADDITIONAL_HEADERS,
				0,
				(LPVOID)postData.c_str(),
				static_cast<DWORD>(postData.length()),
				static_cast<DWORD>(postData.length()),
				0);

			if (!result) {
				logger::error("WinHttpSendRequest failed: {}", GetLastError());
				WinHttpCloseHandle(hRequest);
				WinHttpCloseHandle(hConnect);
				WinHttpCloseHandle(hSession);
				return ""s;
			}

			// Receive response
			result = WinHttpReceiveResponse(hRequest, nullptr);
			if (!result) {
				logger::error("WinHttpReceiveResponse failed");
				WinHttpCloseHandle(hRequest);
				WinHttpCloseHandle(hConnect);
				WinHttpCloseHandle(hSession);
				return ""s;
			}

			// Read response data
			std::string response;
			DWORD bytesAvailable = 0;
			DWORD bytesRead = 0;
			do {
				bytesAvailable = 0;
				if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
					break;
				}

				if (bytesAvailable == 0) {
					break;
				}

				std::vector<char> buffer(bytesAvailable + 1);
				if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
					buffer[bytesRead] = '\0';
					response += buffer.data();
				}
			} while (bytesAvailable > 0);

			// Cleanup
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);

			// Parse response - pastebin.com returns the URL directly on success
			// or an error message starting with "Bad API request"
			if (response.find("Bad API request") != std::string::npos || response.find("error") != std::string::npos) {
				logger::error("Pastebin API error: {}", response);
				return ""s;
			}

			// Response is the paste URL itself
			std::string pasteUrl = response;

			// Remove any whitespace/newlines
			pasteUrl.erase(std::remove_if(pasteUrl.begin(), pasteUrl.end(), ::isspace), pasteUrl.end());

			if (pasteUrl.empty() || pasteUrl.find("http") != 0) {
				logger::error("Invalid response from pastebin: {}", response);
				return ""s;
			}

			logger::info("Crash log uploaded to: {}", pasteUrl);

			// Copy to clipboard
			if (copy_to_clipboard(pasteUrl)) {
				logger::info("Paste URL copied to clipboard");
			} else {
				logger::warn("Failed to copy URL to clipboard");
			}

			// Auto-open in browser
			const std::wstring pasteUrlW = std::wstring(pasteUrl.begin(), pasteUrl.end());
			const auto shellResult = ShellExecuteW(nullptr, L"open", pasteUrlW.c_str(), nullptr, nullptr, SW_SHOW);
			if (reinterpret_cast<INT_PTR>(shellResult) > 32) {
				logger::info("Opened paste URL in browser");
			} else {
				logger::warn("Failed to open URL in browser (error: {})", static_cast<int>(reinterpret_cast<INT_PTR>(shellResult)));
			}

			return pasteUrl;

		} catch (const std::exception& e) {
			logger::error("Exception during log upload: {}", e.what());
			return ""s;
		} catch (...) {
			logger::error("Unknown exception during log upload");
			return ""s;
		}
	}

	// Detect problematic crash recovery DLLs
	bool detect_and_log_problematic_dlls(spdlog::logger& a_log)
	{
		// Extensible list of problematic DLL patterns
		// Each entry contains: {dll_name_pattern, warning_message, help_url}
		struct ProblematicDLL
		{
			std::string_view pattern;
			std::wstring pattern_lower;  // Pre-computed lowercase for comparison
			std::string_view name;
			std::string_view warning;
			std::string_view help_url;
		};

		// Pre-compute lowercase patterns for efficiency
		auto make_dll_entry = [](std::string_view pattern, std::string_view name, 
								  std::string_view warning, std::string_view help_url) {
			std::wstring pattern_lower;
			pattern_lower.reserve(pattern.length());
			for (char c : pattern) {
				pattern_lower += static_cast<wchar_t>(::tolower(static_cast<unsigned char>(c)));
			}
			return ProblematicDLL{ pattern, std::move(pattern_lower), name, warning, help_url };
		};

		const std::vector<ProblematicDLL> problematic_dlls = {
			make_dll_entry("SkyrimCrashGuard.dll",
				"SkyrimCrashGuard",
				"SkyrimCrashGuard attempts to recover from crashes by performing unsafe operations.\n"
				"This can corrupt game state and make crash logs unreliable or misleading.\n"
				"The crash information below may NOT be accurate due to SkyrimCrashGuard interference.",
				"https://www.nexusmods.com/skyrimspecialedition/mods/172082")
		};

		// Get list of loaded modules
		const auto proc = ::GetCurrentProcess();
		std::vector<::HMODULE> modules;
		std::uint32_t needed = 256 * sizeof(::HMODULE);  // Start with reasonable estimate
		bool success = false;
		
		// Retry loop with overflow protection
		for (int attempts = 0; attempts < 3 && !success; ++attempts) {
			modules.resize(needed / sizeof(::HMODULE));
			if (::K32EnumProcessModules(
				proc,
				modules.data(),
				static_cast<::DWORD>(modules.size() * sizeof(::HMODULE)),
				reinterpret_cast<::DWORD*>(&needed))) {
				success = true;
				modules.resize(needed / sizeof(::HMODULE));
			} else {
				// API call failed - check if it's due to buffer size
				const auto error = ::GetLastError();
				if (error != ERROR_INSUFFICIENT_BUFFER && error != ERROR_MORE_DATA) {
					// Real error, not just buffer size issue
					logger::error("Failed to enumerate process modules: {}", error);
					return false;
				}
			}
		}

		if (!success) {
			logger::error("Failed to enumerate process modules after multiple attempts");
			return false;
		}

		bool found_problematic = false;

		// Check each loaded module against our list
		for (const auto& module : modules) {
			wchar_t module_path[MAX_PATH];
			const auto len = ::GetModuleFileNameW(module, module_path, MAX_PATH);
			if (len > 0 && len < MAX_PATH) {  // Check for success and no truncation
				// Extract just the filename from the path
				std::wstring path_str(module_path);
				const auto last_slash = path_str.find_last_of(L"\\/");
				std::wstring filename = (last_slash != std::wstring::npos) ? path_str.substr(last_slash + 1) : path_str;

				// Convert to lowercase for case-insensitive comparison
				std::transform(filename.begin(), filename.end(), filename.begin(), 
					[](wchar_t c) { return static_cast<wchar_t>(::tolower(static_cast<unsigned char>(c))); });

				// Check against each problematic DLL pattern
				for (const auto& problematic : problematic_dlls) {
					if (filename == problematic.pattern_lower) {
						found_problematic = true;

						// Log prominent warning
						a_log.critical(""sv);
						a_log.critical("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"sv);
						a_log.critical("!!! WARNING: {} DETECTED !!!"sv, problematic.name);
						a_log.critical("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"sv);
						a_log.critical(""sv);
						a_log.critical("{}"sv, problematic.warning);
						a_log.critical(""sv);
						a_log.critical("For assistance or to remove this mod, visit:"sv);
						a_log.critical("{}"sv, problematic.help_url);
						a_log.critical(""sv);
						a_log.critical("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"sv);
						a_log.critical(""sv);
					}
				}
			}
		}

		return found_problematic;
	}

}  // namespace Crash
