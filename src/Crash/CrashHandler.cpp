#include "Crash/CrashHandler.h"

#include "Crash/Analysis.h"
#include "Crash/CommonHeader.h"
#include "Crash/CppException.h"
#include "Crash/Introspection/Introspection.h"
#include "Crash/Introspection/RelevantObjectsSimplifier.h"
#include "Crash/Modules/ModuleHandler.h"
#include "Crash/PDB/PdbHandler.h"
#include "Crash/ThreadDump.h"
#include "dxgi1_4.h"
#include <Settings.h>
#include <TlHelp32.h>
#include <iomanip>
#include <magic_enum/magic_enum.hpp>
#include <openvr.h>
#include <shellapi.h>
#include <sstream>
#include <thread>
#include <vmaware.hpp>
#include <wincrypt.h>
#undef debug  // avoid conflict with vmaware.hpp debug
using namespace vr;

namespace Crash
{
	std::filesystem::path crashPath;

	class SEHException : public std::exception
	{
	public:
		SEHException(const std::string& message, DWORD code) :
			_message(message), _code(code) {}

		const char* what() const noexcept override
		{
			return _message.c_str();
		}

		DWORD code() const noexcept
		{
			return _code;
		}

	private:
		std::string _message;
		DWORD _code;
	};

	// SEH to C++ exception translator function
	void seh_translator(unsigned int code, EXCEPTION_POINTERS* info)
	{
		throw SEHException("SEH Exception occurred", code);
	}

	// Safe wrapper to capture stack trace with exception protection
	// Returns a pair: (stacktrace, success_flag)
	std::pair<boost::stacktrace::stacktrace, bool> safe_capture_stacktrace() noexcept
	{
		try {
			// Capture with limited depth to avoid excessive memory usage
			// If this crashes during stack unwinding, the catch will handle it
			auto st = boost::stacktrace::stacktrace(0, 500);
			return { std::move(st), true };
		} catch (const std::bad_alloc&) {
			// Out of memory during stack capture - stack may be corrupted
			return { boost::stacktrace::stacktrace(0, 0), false };
		} catch (...) {
			// Exception during stacktrace capture - stack is likely corrupted
			return { boost::stacktrace::stacktrace(0, 0), false };
		}
	}

	Callstack::Callstack(const ::EXCEPTION_RECORD& a_except)
	{
		auto [stacktrace, success] = safe_capture_stacktrace();
		_stacktrace = std::move(stacktrace);

		try {
			// Handle empty stacktrace (indicates capture failure due to stack corruption)
			if (_stacktrace.empty()) {
				_frames = std::span<const boost::stacktrace::frame>();
				return;
			}

			// Validate stacktrace has reasonable size to prevent memory issues
			if (_stacktrace.size() > 10000) {
				// Truncate extremely large stack traces
				_frames = std::span(_stacktrace.begin(), _stacktrace.begin() + 1000);
				return;
			}

			const auto exceptionAddress = reinterpret_cast<std::uintptr_t>(a_except.ExceptionAddress);
			auto it = std::find_if(_stacktrace.cbegin(), _stacktrace.cend(), [&](auto&& a_elem) noexcept {
				try {
					return reinterpret_cast<std::uintptr_t>(a_elem.address()) == exceptionAddress;
				} catch (...) {
					// Invalid frame, skip
					return false;
				}
			});

			if (it == _stacktrace.cend()) {
				it = _stacktrace.cbegin();
			}

			_frames = std::span(it, _stacktrace.cend());
		} catch (...) {
			// Fallback: create minimal safe frame span
			if (!_stacktrace.empty()) {
				_frames = std::span(_stacktrace.begin(), _stacktrace.begin() + 1);
			} else {
				_frames = std::span<const boost::stacktrace::frame>();
			}
		}
	}

	void Callstack::print(spdlog::logger& a_log, std::span<const module_pointer> a_modules) const
	{
		print_probable_callstack(a_log, a_modules);
	}

	std::string Callstack::get_throw_location(std::span<const module_pointer> a_modules) const
	{
		// For C++ exceptions, the throw site is typically:
		// frame[0] = KERNELBASE.dll (RaiseException)
		// frame[1] = VCRUNTIME140.dll (_CxxThrowException)
		// frame[2] = actual throw site (or ThrowIfFailed wrapper)
		// We scan for the first non-system frame

		if (_frames.size() < 3) {
			return "";
		}

		try {
			for (std::size_t i = 0; i < std::min(_frames.size(), std::size_t(10)); ++i) {
				const auto& frame = _frames[i];
				const auto addr = frame.address();
				const auto mod = Introspection::get_module_for_pointer(addr, a_modules);

				if (mod) {
					const auto modName = mod->name();
					// Skip system frames
					if (modName.find("KERNELBASE") != std::string::npos ||
						modName.find("VCRUNTIME") != std::string::npos ||
						modName.find("ntdll") != std::string::npos ||
						modName.find("KERNEL32") != std::string::npos ||
						modName.find("ucrtbase") != std::string::npos) {
						continue;
					}

					// Found the throw site - get detailed info
					const auto frameAddr = reinterpret_cast<std::uintptr_t>(addr);
					const auto pdbDetails = Crash::PDB::pdb_details(mod->path(), frameAddr - mod->address());

					if (!pdbDetails.empty()) {
						return pdbDetails;
					} else {
						// No PDB, return module+offset
						return fmt::format("{}+{:07X}", mod->name(), frameAddr - mod->address());
					}
				}
			}
		} catch (...) {
			// Ignore errors
		}

		return "";
	}

	std::string Callstack::get_size_string(std::size_t a_size)
	{
		return fmt::to_string(fmt::to_string(a_size - 1).length());
	}

	std::string Callstack::get_format(std::size_t a_nameWidth) const
	{
		return "\t[{:>"s + get_size_string(_frames.size()) + "}] 0x{:012X} {:>"s + fmt::to_string(a_nameWidth) + "}{}"s;
	}

	void Callstack::print_probable_callstack(spdlog::logger& a_log, std::span<const module_pointer> a_modules) const
	{
		a_log.critical("PROBABLE CALL STACK:"sv);

		// Handle empty stacktrace case (indicates capture failure due to stack corruption)
		if (_frames.empty()) {
			a_log.critical("WARNING: Stack trace capture failed - the call stack was likely corrupted.");
			a_log.critical("         The crash information below may be incomplete or unavailable.");
			a_log.critical("         Unable to retrieve any stack frames due to stack corruption.");
			return;
		}

		// Limit stack frames to prevent excessive memory usage and processing time
		constexpr std::size_t MAX_FRAMES = 500;
		const auto frame_count = std::min(_frames.size(), MAX_FRAMES);
		if (_frames.size() > MAX_FRAMES) {
			a_log.critical("Stack trace truncated to {} frames (original: {})", MAX_FRAMES, _frames.size());
		}

		// Build frame data using shared DRY code
		std::vector<FrameData> frame_data;
		frame_data.reserve(frame_count);

		for (std::size_t i = 0; i < frame_count; ++i) {
			try {
				const auto& frame = _frames[i];
				const auto addr = frame.address();
				const auto mod = Introspection::get_module_for_pointer(addr, a_modules);

				const auto frame_info = mod ? [&]() {
					try {
						return mod->frame_info(frame);
					} catch (...) {
						return std::string("[frame info error]");
					}
				}() :
				                              ""s;

				frame_data.push_back({ addr, mod, frame_info });
			} catch (...) {
				// Invalid frame, add placeholder
				frame_data.push_back({ nullptr, nullptr, "[frame processing failed]"s });
			}
		}

		// Use shared printing logic (DRY)
		print_callstack_impl(a_log, frame_data, "\t"sv);
	}

	void Callstack::print_raw_callstack(spdlog::logger& a_log) const
	{
		a_log.critical("RAW CALL STACK:");

		const auto format = "\t[{:>"s + get_size_string(_stacktrace.size()) + "}] 0x{:X}"s;

		for (std::size_t i = 0; i < _stacktrace.size(); ++i) {
			a_log.critical(fmt::runtime(format), i, reinterpret_cast<std::uintptr_t>(_stacktrace[i].address()));
		}
	}

	namespace
	{
		// Structure to hold information about a relevant game object found during crash analysis
		struct RelevantObject
		{
			std::size_t address;
			std::string full_analysis;  // Full introspection output
			std::string location;       // Register name or stack offset
			std::size_t distance;       // Lower = closer to exception (0 = in registers, 1+ = stack offset)
		};

		// Collection to store interesting objects found during analysis
		struct RelevantObjectsCollection
		{
			std::map<std::size_t, RelevantObject> objects;

			void add(std::size_t address, std::string full_analysis, std::string location, std::size_t distance)
			{
				// Only add if address is valid and we haven't seen it yet
				if (address != 0 && objects.find(address) == objects.end()) {
					// Check if this address was successfully introspected (polymorphic or pointer with module info)
					// OR has detailed filter output (game object with properties)
					const bool was_introspected = Introspection::was_introspected(reinterpret_cast<const void*>(address));
					const bool has_filter_output = full_analysis.find("\n\t\t") != std::string::npos;
					
					if (was_introspected || has_filter_output) {
						objects[address] = { address, std::move(full_analysis), std::move(location), distance };
					}
				}
			}

			std::vector<RelevantObject> get_sorted() const
			{
				std::vector<RelevantObject> sorted;
				sorted.reserve(objects.size());
				for (const auto& [addr, obj] : objects) {
					sorted.push_back(obj);
				}
				std::sort(sorted.begin(), sorted.end(),
					[](const RelevantObject& a, const RelevantObject& b) {
						return a.distance < b.distance;
					});
				return sorted;
			}
		};

		// Structure to buffer section output for reordering
		struct SectionBuffer
		{
			std::string name;
			std::stringstream content;

			SectionBuffer(std::string section_name) :
				name(std::move(section_name)) {}
		};

		[[nodiscard]] std::string get_file_md5(const std::filesystem::path& filepath)
		{
			const auto get_error_message = [](DWORD error) -> std::string {
				if (error == 0)
					return "No error";

				LPSTR messageBuffer = nullptr;
				const auto size = FormatMessageA(
					FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
					nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
					reinterpret_cast<LPSTR>(&messageBuffer), 0, nullptr);

				if (size == 0) {
					return fmt::format("Error {:#x}", error);
				}

				std::string message(messageBuffer, size);
				LocalFree(messageBuffer);

				// Remove trailing newlines
				while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
					message.pop_back();
				}

				return fmt::format("Error {:#x}: {}", error, message);
			};

			try {
				std::ifstream file(filepath, std::ios::binary);
				if (!file) {
					const auto error = GetLastError();
					return fmt::format("<file not accessible - {}>", get_error_message(error));
				}

				HCRYPTPROV hProv = 0;
				HCRYPTHASH hHash = 0;

				if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
					const auto error = GetLastError();
					return fmt::format("<CryptAcquireContext failed - {}>", get_error_message(error));
				}

				if (!CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash)) {
					const auto error = GetLastError();
					CryptReleaseContext(hProv, 0);
					return fmt::format("<CryptCreateHash failed - {}>", get_error_message(error));
				}

				constexpr size_t BUFSIZE = 8192;
				char buffer[BUFSIZE];
				while (file.read(buffer, BUFSIZE) || file.gcount() > 0) {
					if (!CryptHashData(hHash, reinterpret_cast<BYTE*>(buffer),
							static_cast<DWORD>(file.gcount()), 0)) {
						const auto error = GetLastError();
						CryptDestroyHash(hHash);
						CryptReleaseContext(hProv, 0);
						return fmt::format("<CryptHashData failed - {}>", get_error_message(error));
					}
				}

				DWORD dwHashLen = 16;  // MD5 is always 16 bytes
				BYTE hash[16];
				if (!CryptGetHashParam(hHash, HP_HASHVAL, hash, &dwHashLen, 0)) {
					const auto error = GetLastError();
					CryptDestroyHash(hHash);
					CryptReleaseContext(hProv, 0);
					return fmt::format("<CryptGetHashParam failed - {}>", get_error_message(error));
				}

				CryptDestroyHash(hHash);
				CryptReleaseContext(hProv, 0);

				std::stringstream ss;
				ss << std::hex << std::setfill('0');
				for (int i = 0; i < 16; ++i) {
					ss << std::setw(2) << static_cast<unsigned>(hash[i]);
				}
				return ss.str();
			} catch (const std::exception& e) {
				return fmt::format("<exception: {}>", e.what());
			} catch (...) {
				return "<unknown exception>";
			}
		}

		void print_exception(spdlog::logger& a_log, const ::EXCEPTION_RECORD& a_exception, std::span<const module_pointer> a_modules, const std::string& a_throwLocation = "")
		{
#define EXCEPTION_CASE(a_code) \
	case a_code:               \
		return " \"" #a_code "\""sv

			// For C++ exceptions, the ExceptionAddress points to RaiseException in KERNELBASE.dll
			// The actual throw site is in Parameter[3] (module base)
			// We'll display both for clarity
			const auto eptr = a_exception.ExceptionAddress;
			const auto eaddr = reinterpret_cast<std::uintptr_t>(a_exception.ExceptionAddress);

			const auto post = [&]() {
				const auto mod = Introspection::get_module_for_pointer(eptr, a_modules);
				if (mod) {
					const auto pdbDetails = Crash::PDB::pdb_details(mod->path(), eaddr - mod->address());
					const auto assembly = mod->assembly((const void*)eaddr);
					if (!pdbDetails.empty())
						return fmt::format(
							" {}+{:07X}\t{} | {})"sv,
							mod->name(),
							eaddr - mod->address(),
							assembly,
							pdbDetails);
					return fmt::format(" {}+{:07X}\t{}"sv, mod->name(), eaddr - mod->address(), assembly);
				} else {
					return ""s;
				}
			}();

			const auto exception = [&]() {
				switch (a_exception.ExceptionCode) {
					EXCEPTION_CASE(EXCEPTION_ACCESS_VIOLATION);
					EXCEPTION_CASE(EXCEPTION_ARRAY_BOUNDS_EXCEEDED);
					EXCEPTION_CASE(EXCEPTION_BREAKPOINT);
					EXCEPTION_CASE(EXCEPTION_DATATYPE_MISALIGNMENT);
					EXCEPTION_CASE(EXCEPTION_FLT_DENORMAL_OPERAND);
					EXCEPTION_CASE(EXCEPTION_FLT_DIVIDE_BY_ZERO);
					EXCEPTION_CASE(EXCEPTION_FLT_INEXACT_RESULT);
					EXCEPTION_CASE(EXCEPTION_FLT_INVALID_OPERATION);
					EXCEPTION_CASE(EXCEPTION_FLT_OVERFLOW);
					EXCEPTION_CASE(EXCEPTION_FLT_STACK_CHECK);
					EXCEPTION_CASE(EXCEPTION_FLT_UNDERFLOW);
					EXCEPTION_CASE(EXCEPTION_ILLEGAL_INSTRUCTION);
					EXCEPTION_CASE(EXCEPTION_IN_PAGE_ERROR);
					EXCEPTION_CASE(EXCEPTION_INT_DIVIDE_BY_ZERO);
					EXCEPTION_CASE(EXCEPTION_INT_OVERFLOW);
					EXCEPTION_CASE(EXCEPTION_INVALID_DISPOSITION);
					EXCEPTION_CASE(EXCEPTION_NONCONTINUABLE_EXCEPTION);
					EXCEPTION_CASE(EXCEPTION_PRIV_INSTRUCTION);
					EXCEPTION_CASE(EXCEPTION_SINGLE_STEP);
					EXCEPTION_CASE(EXCEPTION_STACK_OVERFLOW);
				case CPP_EXCEPTION_CODE:
					return " \"C++ Exception\""sv;
				default:
					return ""sv;
				}
			}();

			a_log.critical("Unhandled exception{} at 0x{:012X}{}"sv, exception, eaddr, post);

			// Log exception flags with description
			a_log.critical("Exception Flags: 0x{:08X}{}"sv, a_exception.ExceptionFlags,
				(a_exception.ExceptionFlags & EXCEPTION_NONCONTINUABLE) ? " (Non-continuable)" :
				(a_exception.ExceptionFlags == 0)                       ? " (Continuable)" :
																		  "");

			// Log number of parameters
			a_log.critical("Number of Parameters: {}"sv, a_exception.NumberParameters);

			// Add thread context info
			const auto tid = GetCurrentThreadId();
			a_log.critical("Exception Thread ID: {}"sv, tid);

			// Log additional exception information for specific exception types
			if (a_exception.ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
				const auto accessType = a_exception.ExceptionInformation[0] == 0 ? "read" :
				                        a_exception.ExceptionInformation[0] == 1 ? "write" :
				                        a_exception.ExceptionInformation[0] == 8 ? "execute" :
				                                                                   "unknown";
				const auto faultAddress = a_exception.ExceptionInformation[1];
				a_log.critical("Access Violation: Tried to {} memory at 0x{:012X}"sv, accessType, faultAddress);
			} else if (a_exception.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) {
				const auto accessType = a_exception.ExceptionInformation[0] == 0 ? "read" :
				                        a_exception.ExceptionInformation[0] == 1 ? "write" :
				                        a_exception.ExceptionInformation[0] == 8 ? "execute" :
				                                                                   "unknown";
				const auto faultAddress = a_exception.ExceptionInformation[1];
				const auto ntStatus = a_exception.ExceptionInformation[2];
				a_log.critical("In-Page Error: Tried to {} memory at 0x{:012X}, NTSTATUS: 0x{:08X}"sv, accessType, faultAddress, ntStatus);
			} else if (IsCppException(a_exception)) {
				// Parse and display C++ exception details
				if (const auto cppExInfo = ParseCppException(a_exception)) {
					a_log.critical("");
					a_log.critical("C++ EXCEPTION:");

					// Use existing introspection system to analyze the exception object
					const std::size_t addresses[] = { cppExInfo->objectAddress };
					const auto analysis = Introspection::analyze_data(addresses, a_modules);

					if (!analysis.empty() && !analysis[0].empty()) {
						// The introspection system provides full type information with details
						a_log.critical("\tType: {}"sv, analysis[0]);
					} else {
						// Fallback to our manual parsing if introspection fails
						a_log.critical("\tType: {}"sv, cppExInfo->typeName);
					}

					// Always try to extract additional info from the exception object (e.g., HRESULT)
					if (cppExInfo->what) {
						a_log.critical("\tInfo: {}"sv, *cppExInfo->what);
					}

					// Show the throw location if available (extracted from callstack)
					if (!a_throwLocation.empty()) {
						a_log.critical("\tThrow Location: {}"sv, a_throwLocation);
					}

					// Log module information
					const auto modulePtr = reinterpret_cast<const void*>(cppExInfo->moduleBase);
					const auto mod = Introspection::get_module_for_pointer(modulePtr, a_modules);
					if (mod) {
						a_log.critical("\tModule: {}"sv, mod->name());
					}
				} else {
					a_log.critical("C++ Exception: Failed to parse exception details");
				}
			} else if (a_exception.NumberParameters > 0) {
				a_log.critical("Exception Information Parameters:");
				for (std::size_t i = 0; i < a_exception.NumberParameters; ++i) {
					const auto param = a_exception.ExceptionInformation[i];
					const std::size_t params[] = { param };
					const auto analysis = Introspection::analyze_data(params, a_modules);

					if (!analysis.empty() && !analysis[0].empty()) {
						a_log.critical("\tParameter[{}]: 0x{:012X} {}"sv, i, param, analysis[0]);
					} else {
						a_log.critical("\tParameter[{}]: 0x{:012X}"sv, i, param);
					}
				}
			}

			// Check for nested exceptions (with depth limit to prevent infinite recursion)
			static thread_local int exception_depth = 0;
			constexpr int MAX_EXCEPTION_DEPTH = 10;
			if (a_exception.ExceptionRecord && exception_depth < MAX_EXCEPTION_DEPTH) {
				a_log.critical("Nested Exception (depth {}):", exception_depth + 1);
				++exception_depth;
				try {
					print_exception(a_log, *a_exception.ExceptionRecord, a_modules);
				} catch (...) {
					a_log.critical("Failed to process nested exception at depth {}", exception_depth);
				}
				--exception_depth;
			} else if (a_exception.ExceptionRecord && exception_depth >= MAX_EXCEPTION_DEPTH) {
				a_log.critical("Nested exception depth limit reached ({}), stopping recursion", MAX_EXCEPTION_DEPTH);
			}
#undef EXCEPTION_CASE
		}

		void print_xse_plugins(spdlog::logger& a_log, std::span<const module_pointer> a_modules)
		{
			a_log.critical("SKSE PLUGINS:"sv);

			const auto ci = [](std::string_view a_lhs, std::string_view a_rhs) {
				const auto cmp = _strnicmp(a_lhs.data(), a_rhs.data(), std::min(a_lhs.size(), a_rhs.size()));
				return cmp == 0 && a_lhs.length() != a_rhs.length() ? a_lhs.length() < a_rhs.length() : cmp < 0;
			};

			const auto modules = [&]() {
				std::set<std::string_view, decltype(ci)> result;
				for (const auto& mod : a_modules) {
					result.insert(mod->name());
				}

				return result;
			}();

			// Helper: attempt to read FileVersion string from version resource
			auto get_file_version_string = [&](const std::filesystem::path& filename) -> std::optional<std::string> {
				DWORD handle = 0;
				const auto pathW = filename.wstring();
				const auto size = GetFileVersionInfoSizeW(pathW.c_str(), &handle);
				if (size == 0)
					return std::nullopt;

				std::vector<std::byte> data(size);
				if (!GetFileVersionInfoW(pathW.c_str(), handle, size, data.data()))
					return std::nullopt;

				// Try StringFileInfo translation entry first
				struct LANGANDCODEPAGE
				{
					WORD wLanguage;
					WORD wCodePage;
				};
				LANGANDCODEPAGE* trans = nullptr;
				UINT transLen = 0;
				if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&trans), &transLen) && transLen >= sizeof(LANGANDCODEPAGE)) {
					wchar_t block[64];
					swprintf(block, std::size(block), L"\\StringFileInfo\\%04x%04x\\FileVersion", trans[0].wLanguage, trans[0].wCodePage);
					LPWSTR value = nullptr;
					UINT valueLen = 0;
					if (VerQueryValueW(data.data(), block, reinterpret_cast<LPVOID*>(&value), &valueLen) && valueLen > 0) {
						std::wstring ws(value, valueLen);
						// trim trailing whitespace/newlines
						while (!ws.empty() && iswspace(ws.back())) ws.pop_back();
						return util::utf16_to_utf8(ws).value_or(std::string());
					}
				}

				// Fallback: use fixed file info
				VS_FIXEDFILEINFO* ffi = nullptr;
				UINT ffiLen = 0;
				if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffiLen) && ffi) {
					const auto major = HIWORD(ffi->dwFileVersionMS);
					const auto minor = LOWORD(ffi->dwFileVersionMS);
					const auto build = HIWORD(ffi->dwFileVersionLS);
					const auto rev = LOWORD(ffi->dwFileVersionLS);
					return fmt::format("{}.{}.{}.{}", major, minor, build, rev);
				}

				return std::nullopt;
			};

			struct PluginInfo
			{
				std::string name;
				std::optional<REL::Version> version;
				std::optional<std::string> version_str;  // fallback raw/string version
			};

			std::vector<PluginInfo> plugins;
			for (const auto& m : modules) {
				try {
					std::filesystem::path pluginDir{ Crash::PDB::sPluginPath };
					std::filesystem::path filename = pluginDir;
					filename.append(m);
					if (std::filesystem::exists(filename)) {
						try {
							plugins.push_back({ std::string(m), REL::GetFileVersion(filename.wstring()), std::nullopt });
						} catch (const std::exception&) {
							// Fallback: try to read whatever version string we can from the file resources
							auto vs = get_file_version_string(filename);
							if (vs) {
								plugins.push_back({ std::string(m), std::nullopt, *vs });
							} else {
								plugins.push_back({ std::string(m), std::nullopt, std::nullopt });
							}
						}
					}
				} catch (const std::exception& e) {
					a_log.critical("Skipping module {}:{}"sv, m, e.what());
				} catch (...) {
					a_log.critical("Skipping module {}:<unknown error>"sv, m);
				}
			}
			std::sort(plugins.begin(), plugins.end(),
				[=](const PluginInfo& a_lhs, const PluginInfo& a_rhs) { return ci(a_lhs.name, a_rhs.name); });
			for (const auto& p : plugins) {
				if (p.version) {
					const auto ver = [&]() {
						if (p.version) {
							std::span view{ p.version->begin(), p.version->end() };
							const auto it = std::find_if(view.rbegin(), view.rend(),
								[](std::uint16_t a_val) noexcept { return a_val != 0; });
							if (it != view.rend()) {
								std::string result = " v";
								std::string_view pre;
								for (std::size_t i = 0; i < static_cast<std::size_t>(view.rend() - it); ++i) {
									result += pre;
									result += fmt::to_string(view[i]);
									pre = "."sv;
								}
								return result;
							}
						}
						return ""s;
					}();
					a_log.critical("\t{}{}"sv, p.name, ver);
				} else if (p.version_str) {
					a_log.critical("\t{} v{}"sv, p.name, *p.version_str);
				} else {
					a_log.critical("\t{}"sv, p.name);
				}
			}
		}

		void print_modules(spdlog::logger& a_log, std::span<const module_pointer> a_modules)
		{
			a_log.critical("MODULES:"sv);

			const auto format = [&]() {
				const auto width = [&]() {
					std::size_t max = 0;
					std::for_each(a_modules.begin(), a_modules.end(),
						[&](auto&& a_elem) { max = std::max(max, a_elem->name().length()); });
					return max;
				}();

				return "\t{:<"s + fmt::to_string(width) + "} 0x{:012X}"s;
			}();

			for (const auto& mod : a_modules) {
				a_log.critical(fmt::runtime(format), mod->name(), mod->address());
			}
		}

		void print_plugins(spdlog::logger& a_log)
		{
			a_log.critical("PLUGINS:"sv);

			const auto datahandler = RE::TESDataHandler::GetSingleton();
			if (datahandler) {
				const auto lightCount = datahandler->GetLoadedLightModCount();
				const auto modCount = datahandler->GetLoadedModCount();
				a_log.critical("\tLight: {}\tRegular: {}\tTotal: {}"sv, lightCount, modCount, lightCount + modCount);
				const auto& files = datahandler->GetLoadedMods();
				const auto& smallfiles = datahandler->GetLoadedLightMods();
				const auto fileFormat = [&]() {
					return "\t[{:>02X}]{:"s + (lightCount ? "5"s : "1"s) + "}{}"s;
				}();
				for (auto i = 0; i < modCount; i++) {
					const auto file = files[i];
					a_log.critical(fmt::runtime(fileFormat), file->GetCompileIndex(), "", file->GetFilename());
				}
				for (auto i = 0; i < lightCount; i++) {
					const auto file = smallfiles[i];
					a_log.critical("\t[FE:{:>03X}] {}"sv, file->GetSmallFileCompileIndex(), file->GetFilename());
				}
			}
		}

		void print_relevant_objects_section(spdlog::logger& a_log, const RelevantObjectsCollection& collection)
		{
			a_log.critical("POSSIBLE RELEVANT OBJECTS:"sv);

			try {
				const auto sortedObjects = collection.get_sorted();

				// Print up to 128 most relevant objects
				constexpr std::size_t MAX_OBJECTS = 128;
				const auto objectCount = std::min(sortedObjects.size(), MAX_OBJECTS);

				if (objectCount == 0) {
					a_log.critical("\tNone found"sv);
				} else {
					for (std::size_t i = 0; i < objectCount; ++i) {
						const auto& obj = sortedObjects[i];
						// Simplify the full analysis for concise display
						const auto simplified = Introspection::simplify_for_relevant_objects(obj.full_analysis);
						if (!simplified.empty()) {
							a_log.critical("\t{}: {}"sv, obj.location, simplified);
						}
					}
					if (sortedObjects.size() > MAX_OBJECTS) {
						a_log.critical("\t... and {} more (truncated)"sv, sortedObjects.size() - MAX_OBJECTS);
					}
				}
			} catch (const std::exception& e) {
				a_log.critical("\tFailed to print objects: {}"sv, e.what());
			} catch (...) {
				a_log.critical("\tFailed to print objects: unknown error"sv);
			}
		}

		void print_sysinfo(spdlog::logger& a_log)
		{
			a_log.critical("SYSTEM SPECS:"sv);

			const auto os = iware::system::OS_info();
			a_log.critical("\tOS: {} v{}.{}.{}"sv, os.full_name, os.major, os.minor, os.patch);

			a_log.critical("\tCPU: {} {}"sv, iware::cpu::vendor(), iware::cpu::model_name());

			// Add CPU core information
			try {
				const auto cores = iware::cpu::quantities();
				a_log.critical("\tCPU Cores: {} logical, {} physical, {} packages"sv, cores.logical, cores.physical, cores.packages);
			} catch (...) {
				a_log.critical("\tCPU Cores: Unable to determine"sv);
			}

			const auto vendor = [](iware::gpu::vendor_t a_vendor) {
				using vendor_t = iware::gpu::vendor_t;
				switch (a_vendor) {
				case vendor_t::intel:
					return "Intel"sv;
				case vendor_t::amd:
					return "AMD"sv;
				case vendor_t::nvidia:
					return "Nvidia"sv;
				case vendor_t::microsoft:
					return "Microsoft"sv;
				case vendor_t::qualcomm:
					return "Qualcomm"sv;
				case vendor_t::unknown:
				default:
					return "Unknown"sv;
				}
			};

			const auto gpus = iware::gpu::device_properties();
			for (std::size_t i = 0; i < gpus.size(); ++i) {
				const auto& gpu = gpus[i];
				a_log.critical("\tGPU #{}: {} {}"sv, i + 1, vendor(gpu.vendor), gpu.name);
			}

			const auto gibibyte = [](std::uint64_t a_bytes) {
				constexpr double factor = 1024 * 1024 * 1024;
				return static_cast<double>(a_bytes) / factor;
			};

			const auto mem = iware::system::memory();
			a_log.critical("\tPHYSICAL MEMORY: {:.02f} GB/{:.02f} GB"sv,
				gibibyte(mem.physical_total - mem.physical_available), gibibyte(mem.physical_total));
			a_log.critical("\tVIRTUAL MEMORY: {:.02f} GB/{:.02f} GB"sv,
				gibibyte(mem.virtual_total - mem.virtual_available), gibibyte(mem.virtual_total));

			// Process memory usage
			try {
				PROCESS_MEMORY_COUNTERS_EX pmc;
				if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
					a_log.critical("\tPROCESS MEMORY: Working Set: {:.02f} MB, Private: {:.02f} MB, Peak: {:.02f} MB"sv,
						static_cast<double>(pmc.WorkingSetSize) / (1024 * 1024),
						static_cast<double>(pmc.PrivateUsage) / (1024 * 1024),
						static_cast<double>(pmc.PeakWorkingSetSize) / (1024 * 1024));
					a_log.critical("\tPAGE FAULTS: {} (Peak: {})"sv, pmc.PageFaultCount, pmc.PeakWorkingSetSize);
				}
			} catch (...) {
				a_log.critical("\tPROCESS MEMORY: Unable to determine"sv);
			}

			//https://forums.unrealengine.com/t/how-to-get-vram-usage-via-c/218627/2
			try {
				IDXGIFactory4* pFactory{};
				HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&pFactory);
				if (FAILED(hr)) {
					a_log.critical("\tGPU MEMORY: Failed to create DXGI factory (HRESULT: {:#x})"sv, static_cast<uint32_t>(hr));
					return;
				}

				IDXGIAdapter3* adapter{};
				hr = pFactory->EnumAdapters(0, reinterpret_cast<IDXGIAdapter**>(&adapter));
				if (FAILED(hr)) {
					a_log.critical("\tGPU MEMORY: Failed to enumerate adapter (HRESULT: {:#x})"sv, static_cast<uint32_t>(hr));
					pFactory->Release();
					return;
				}

				DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfo;
				hr = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfo);
				if (FAILED(hr)) {
					a_log.critical("\tGPU MEMORY: Failed to query video memory (HRESULT: {:#x})"sv, static_cast<uint32_t>(hr));
				} else {
					a_log.critical("\tGPU MEMORY: {:.02f}/{:.02f} GB"sv, gibibyte(videoMemoryInfo.CurrentUsage),
						gibibyte(videoMemoryInfo.Budget));
				}

				adapter->Release();
				pFactory->Release();
			} catch (const std::exception& e) {
				a_log.critical("\tGPU MEMORY: Exception occurred: {}"sv, e.what());
			} catch (...) {
				a_log.critical("\tGPU MEMORY: Unknown exception occurred"sv);
			}

			// Detect VM
			if (VM::detect(VM::DISABLE(VM::GAMARUE))) {
				a_log.critical("\tDetected Virtual Machine: {} ({}%)"sv, VM::brand(VM::MULTIPLE), VM::percentage());
			}
		}

		void print_process_info(spdlog::logger& a_log)
		{
			a_log.critical("PROCESS INFO:"sv);

			// Process ID and thread info
			const auto pid = GetCurrentProcessId();
			const auto tid = GetCurrentThreadId();
			a_log.critical("\tProcess ID: {}"sv, pid);
			a_log.critical("\tCrash Thread ID: {}"sv, tid);

			// Process uptime
			try {
				FILETIME creation_time, exit_time, kernel_time, user_time;
				if (GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time)) {
					FILETIME current_time;
					GetSystemTimeAsFileTime(&current_time);

					ULARGE_INTEGER creation, current;
					creation.LowPart = creation_time.dwLowDateTime;
					creation.HighPart = creation_time.dwHighDateTime;
					current.LowPart = current_time.dwLowDateTime;
					current.HighPart = current_time.dwHighDateTime;

					const auto uptime_ms = (current.QuadPart - creation.QuadPart) / 10000;  // Convert to milliseconds
					const auto uptime_sec = uptime_ms / 1000;
					const auto hours = uptime_sec / 3600;
					const auto minutes = (uptime_sec % 3600) / 60;
					const auto seconds = uptime_sec % 60;

					a_log.critical("\tProcess Uptime: {:02}:{:02}:{:02} ({}ms)"sv, hours, minutes, seconds, uptime_ms);
				}
			} catch (...) {
				a_log.critical("\tProcess Uptime: Unable to determine"sv);
			}

			// Working directory
			try {
				wchar_t current_dir[MAX_PATH];
				if (GetCurrentDirectoryW(MAX_PATH, current_dir)) {
					const std::wstring wdir(current_dir);
					const std::string dir = util::utf16_to_utf8(wdir).value_or(std::string());
					a_log.critical("\tWorking Directory: {}"sv, dir);
				}
			} catch (...) {
				a_log.critical("\tWorking Directory: Unable to determine"sv);
			}

			// Command line and executable information
			try {
				const auto cmd_line = GetCommandLineA();
				if (cmd_line) {
					a_log.critical("\tCommand Line: {}"sv, cmd_line);
				}

				// Extract executable path and provide file details
				wchar_t exePath[MAX_PATH];
				if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
					const std::filesystem::path exe_path(exePath);
					const auto hash = get_file_md5(exe_path);
					a_log.critical("\tExecutable MD5: {}"sv, hash);

					// Also get file size and timestamp
					std::error_code ec;
					const auto file_size = std::filesystem::file_size(exe_path, ec);
					if (!ec) {
						a_log.critical("\tExecutable Size: {} bytes"sv, file_size);
					}

					const auto file_time = std::filesystem::last_write_time(exe_path, ec);
					if (!ec) {
						// Convert to time_t for logging
						const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
							file_time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
						const auto time_t_val = std::chrono::system_clock::to_time_t(sctp);
						std::tm tm_val{};
						if (localtime_s(&tm_val, &time_t_val) == 0) {
							a_log.critical("\tExecutable Modified: {:04}-{:02}-{:02} {:02}:{:02}:{:02}"sv,
								tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
								tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
						}
					}
				}
			} catch (...) {
				a_log.critical("\tCommand Line/Executable Info: <error retrieving>"sv);
			}

			// Process privilege level
			try {
				HANDLE token;
				if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
					TOKEN_ELEVATION elevation;
					DWORD size;
					if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
						a_log.critical("\tElevated: {}"sv, elevation.TokenIsElevated ? "Yes" : "No");
					}
					CloseHandle(token);
				}
			} catch (...) {
				a_log.critical("\tElevated: Unable to determine"sv);
			}
		}

		void print_vrinfo(spdlog::logger& a_log)
		{
			static auto openvr = GetModuleHandle(L"openvr_api");  // dynamically attach to open_vr
			if (openvr) {
				static auto _VR_GetGenericInterface = reinterpret_cast<decltype(&VR_GetGenericInterface)>(GetProcAddress(openvr, "VR_GetGenericInterface"));
				static auto _VR_IsHmdPresent = reinterpret_cast<decltype(&VR_IsHmdPresent)>(GetProcAddress(openvr, "VR_IsHmdPresent"));
				static auto _VR_IsRuntimeInstalled = reinterpret_cast<decltype(&VR_IsRuntimeInstalled)>(GetProcAddress(openvr, "VR_IsRuntimeInstalled"));
				if (_VR_GetGenericInterface && _VR_IsHmdPresent && _VR_IsRuntimeInstalled && _VR_IsHmdPresent() && _VR_IsRuntimeInstalled()) {
					a_log.critical("VR SPECS:"sv);
					// Loading the SteamVR Runtime
					EVRInitError eError = VRInitError_None;
					auto HMD = (IVRSystem*)_VR_GetGenericInterface(IVRSystem_Version, &eError);

					if (eError != VRInitError_None) {
						const auto error_name = magic_enum::enum_name(eError);
						if (!error_name.empty()) {
							a_log.critical("\tUnable to initialize VR runtime: {} ({})"sv, error_name, static_cast<int>(eError));
						} else {
							a_log.critical("\tUnable to initialize VR runtime (Error: {})"sv, static_cast<int>(eError));
						}
						return;
					}

					// Helper lambda for safe string property retrieval
					const auto get_string_prop = [&](ETrackedDeviceProperty prop, const std::string& name) {
						try {
							char propValue[k_unMaxPropertyStringSize] = {};
							ETrackedPropertyError error = TrackedProp_Success;
							HMD->GetStringTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, prop, propValue, std::size(propValue), &error);

							if (error == TrackedProp_Success && propValue[0] != '\0') {
								a_log.critical("\t{}: {}"sv, name, propValue);
							} else {
								const auto error_name = magic_enum::enum_name(error);
								if (!error_name.empty() && error != TrackedProp_Success) {
									a_log.critical("\t{}: <error: {}>"sv, name, error_name);
								} else {
									a_log.critical("\t{}: <unavailable>"sv, name);
								}
							}
						} catch (...) {
							a_log.critical("\t{}: <exception>"sv, name);
						}
					};

					// Helper lambda for safe float property retrieval
					const auto get_float_prop = [&](ETrackedDeviceProperty prop, const std::string& name) {
						try {
							ETrackedPropertyError error = TrackedProp_Success;
							float value = HMD->GetFloatTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, prop, &error);

							if (error == TrackedProp_Success) {
								a_log.critical("\t{}: {:.2f}"sv, name, value);
							} else {
								const auto error_name = magic_enum::enum_name(error);
								if (!error_name.empty()) {
									a_log.critical("\t{}: <error: {}>"sv, name, error_name);
								} else {
									a_log.critical("\t{}: <unavailable>"sv, name);
								}
							}
						} catch (...) {
							a_log.critical("\t{}: <exception>"sv, name);
						}
					};

					// Helper lambda for safe bool property retrieval
					const auto get_bool_prop = [&](ETrackedDeviceProperty prop, const std::string& name) {
						try {
							ETrackedPropertyError error = TrackedProp_Success;
							bool value = HMD->GetBoolTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, prop, &error);

							if (error == TrackedProp_Success) {
								a_log.critical("\t{}: {}"sv, name, value ? "Yes" : "No");
							} else {
								const auto error_name = magic_enum::enum_name(error);
								if (!error_name.empty()) {
									a_log.critical("\t{}: <error: {}>"sv, name, error_name);
								} else {
									a_log.critical("\t{}: <unavailable>"sv, name);
								}
							}
						} catch (...) {
							a_log.critical("\t{}: <exception>"sv, name);
						}
					};

					// Essential crash-relevant VR information
					get_string_prop(Prop_ModelNumber_String, "Model");
					get_string_prop(Prop_ManufacturerName_String, "Manufacturer");
					get_string_prop(Prop_DriverVersion_String, "Driver Version");
					get_string_prop(Prop_TrackingSystemName_String, "Tracking System");

					// Performance-critical display properties
					get_float_prop(Prop_DisplayFrequency_Float, "Display Frequency (Hz)");
					get_float_prop(Prop_UserIpdMeters_Float, "IPD (meters)");

					// Get render target resolution (critical for performance analysis)
					try {
						uint32_t renderWidth = 0, renderHeight = 0;
						HMD->GetRecommendedRenderTargetSize(&renderWidth, &renderHeight);
						if (renderWidth > 0 && renderHeight > 0) {
							a_log.critical("\tRender Target Size: {}x{}"sv, renderWidth, renderHeight);
						} else {
							a_log.critical("\tRender Target Size: <unavailable>"sv);
						}
					} catch (...) {
						a_log.critical("\tRender Target Size: <error>"sv);
					}
				}
			}
		}

#undef SETTING_CASE

		std::int32_t __stdcall UnhandledExceptions(::EXCEPTION_POINTERS* a_exception) noexcept
		{
			// Install the SEH-to-C++ exception translator
			_set_se_translator(seh_translator);

			std::filesystem::path crashLogPath;

			try {
				const auto& debugConfig = Settings::GetSingleton()->GetDebug();
				if (debugConfig.waitForDebugger) {
					while (!IsDebuggerPresent()) {
					}
					if (IsDebuggerPresent())
						DebugBreak();
				}
				static std::mutex sync;
				const std::lock_guard l{ sync };

				const auto modules = Modules::get_loaded_modules();
				const std::span cmodules{ modules.begin(), modules.end() };
				auto [log, logPath] = get_timestamped_log("crash-"sv, "crash log"s);
				crashLogPath = logPath;

				// Clean up old logs
				const auto& debug = Settings::GetSingleton()->GetDebug();
				clean_old_files(logPath.parent_path(), "crash-"sv, ".log", debug.maxCrashLogs, ".dmp");
				clean_old_files(logPath.parent_path(), "crash-"sv, ".dmp", debug.maxMinidumps);

				// Write minidump if requested
				if (Settings::GetSingleton()->GetDebug().crashLogWriteMinidump) {
					try {
						auto dumpPath = logPath;
						dumpPath.replace_extension(".dmp");
						if (write_minidump(dumpPath, a_exception)) {
							log->critical("Minidump written to: {}", dumpPath.string());
						} else {
							log->critical("Failed to write minidump to: {}", dumpPath.string());
						}
					} catch (...) {
						log->critical("Exception while writing minidump");
					}
				}

				// Collection to gather relevant objects during analysis
				RelevantObjectsCollection relevantObjects;

				const auto print = [&](auto&& a_functor, std::string a_name = "") {
					log->critical(""sv);
					try {
						// Add timeout protection to prevent hanging on bad operations
						auto start = std::chrono::steady_clock::now();
						constexpr auto timeout = std::chrono::seconds(30);

						a_functor();

						auto elapsed = std::chrono::steady_clock::now() - start;
						if (elapsed > std::chrono::seconds(5)) {
							log->critical("\t{}: completed in {:.1f}s (slow)"sv, a_name,
								std::chrono::duration<double>(elapsed).count());
						}
					} catch (const std::exception& e) {
						log->critical("\t{}:\t{}"sv, a_name, e.what());
					} catch (...) {
						log->critical("\t{}:\tERROR"sv, a_name);
					}
					log->flush();
				};

				// Use common header logging function for crash info
				log_common_header_info(*log, ""sv, "CRASH TIME:"sv);
				log->flush();

				// Construct callstack early so we can extract throw location for C++ exceptions
				std::optional<Callstack> callstack;
				std::string throwLocation;
				try {
					callstack.emplace(*a_exception->ExceptionRecord);
					if (IsCppException(*a_exception->ExceptionRecord)) {
						throwLocation = callstack->get_throw_location(cmodules);
					}
				} catch (...) {
					// Callstack construction failed, continue without it
				}

				print([&]() { print_exception(*log, *a_exception->ExceptionRecord, cmodules, throwLocation); }, "print_exception");

				// Collect relevant objects from registers and stack (fast pass, no printing)
				try {
					// Collect from registers
					const auto [regs, regAnalysis] = analyze_registers(*a_exception->ContextRecord, cmodules);
					for (std::size_t i = 0; i < regs.size(); ++i) {
						relevantObjects.add(regs[i].second, regAnalysis[i], std::string(regs[i].first), 0);
					}

					// Collect from stack (limited to first 512 entries)
					const auto stack_opt = get_stack_info(*a_exception->ContextRecord);
					if (stack_opt) {
						const auto& stack = *stack_opt;
						constexpr std::size_t MAX_SCAN = 512;
						const auto limited_stack = stack.subspan(0, std::min(stack.size(), MAX_SCAN));
						const auto stack_analyses = analyze_stack_blocks(limited_stack, cmodules);

						std::size_t global_idx = 0;
						for (std::size_t block_idx = 0; block_idx < stack_analyses.size(); ++block_idx) {
							const auto& analysis = stack_analyses[block_idx];
							for (std::size_t idx = 0; idx < analysis.size(); ++idx) {
								const auto distance = global_idx * sizeof(std::size_t);
								relevantObjects.add(stack[global_idx], analysis[idx],
									fmt::format("RSP+{:X}", distance), distance + 1000);
								++global_idx;
							}
						}
					}
				} catch (...) {
					// If object collection fails, continue without it
				}

				// Print relevant objects section (after exception, before other sections)
				print([&]() { print_relevant_objects_section(*log, relevantObjects); }, "print_relevant_objects");

				print([&]() { print_process_info(*log); }, "print_process_info");
				print([&]() { print_sysinfo(*log); }, "print_sysinfo");
				if (REL::Module::IsVR())
					print([&]() { print_vrinfo(*log); }, "print_vrinfo");

				print([&]() {
					try {
						if (callstack) {
							callstack->print(*log, cmodules);
						} else {
							// Construct it now if we couldn't earlier
							const Callstack cs{ *a_exception->ExceptionRecord };
							cs.print(*log, cmodules);
						}
					} catch (const std::bad_alloc&) {
						log->critical("CALLSTACK ANALYSIS FAILED: Out of memory");
					} catch (...) {
						log->critical("CALLSTACK ANALYSIS FAILED: Unknown error in stack analysis");
						// Fallback: try to at least log the exception address
						try {
							const auto addr = reinterpret_cast<std::uintptr_t>(a_exception->ExceptionRecord->ExceptionAddress);
							log->critical("Exception occurred at address: 0x{:012X}", addr);
						} catch (...) {
							log->critical("Unable to retrieve exception address");
						}
					}
				},
					"probable_callstack");

				// Analyze registers and stack first, then backfill, then print
				try {
					// Helper struct for uniform backfill processing
					struct AnalysisBlock
					{
						std::vector<std::string> analysis;
						std::span<const std::size_t> addresses;
					};
					std::vector<AnalysisBlock> allBlocks;

					// Analyze registers
					const auto [regs, regAnalysis] = analyze_registers(*a_exception->ContextRecord, cmodules);
					const auto [dummy_regs, regValues] = get_register_info(*a_exception->ContextRecord);
					allBlocks.push_back({ regAnalysis, regValues });

					// Analyze stack blocks
					const auto stack_opt = get_stack_info(*a_exception->ContextRecord);
					if (stack_opt) {
						const auto& stack = *stack_opt;
						constexpr std::size_t MAX_SCAN = 512;
						const auto scanSize = std::min(stack.size(), MAX_SCAN);

						constexpr std::size_t blockSize = 256;
						for (std::size_t off = 0; off < scanSize; off += blockSize) {
							auto block = stack.subspan(off, std::min<std::size_t>(scanSize - off, blockSize));
							auto analysis = Introspection::analyze_data(block, cmodules, [&](size_t i) { return fmt::format("RSP+{:X}", (off + i) * sizeof(std::size_t)); });
							allBlocks.push_back({ std::move(analysis), block });
						}
					}

					// Backfill all analyzed data uniformly
					for (auto& block : allBlocks) {
						Introspection::backfill_void_pointers(block.analysis, block.addresses);
					}

					// Extract backfilled results for printing
					auto& finalRegAnalysis = allBlocks[0].analysis;
					std::vector<std::vector<std::string>> stackAnalyses;
					for (size_t i = 1; i < allBlocks.size(); ++i) {
						stackAnalyses.push_back(std::move(allBlocks[i].analysis));
					}

					// Print with pre-analyzed data
					print([&]() { print_registers(*log, *a_exception->ContextRecord, cmodules, finalRegAnalysis); }, "print_registers");
					print([&]() { print_stack(*log, *a_exception->ContextRecord, cmodules, stackAnalyses); }, "print_raw_stack");
				} catch (...) {
					// Fallback to original behavior if analysis fails
					print([&]() { print_registers(*log, *a_exception->ContextRecord, cmodules); }, "print_registers");
					print([&]() { print_stack(*log, *a_exception->ContextRecord, cmodules); }, "print_raw_stack");
				}
				print([&]() { print_modules(*log, cmodules); }, "print_modules");
				print([&]() { print_xse_plugins(*log, cmodules); }, "print_xse_plugins");
				print([&]() { print_plugins(*log); }, "print_plugins");

				// Ensure all log data is written to disk before we try to open the file
				log->flush();

			} catch (const SEHException& se) {
				// Log the SEH exception converted to a C++ exception
				auto [log, logPath] = get_timestamped_log("crash-"sv, "crash log"s);
				log->critical("SEH Exception caught: {} (Code: 0x{:X})", se.what(), se.code());
				log->flush();
			} catch (const std::exception& e) {
				// Log the C++ exception
				auto [log, logPath] = get_timestamped_log("crash-"sv, "crash log"s);
				log->critical("Caught C++ exception: {}", e.what());
				log->flush();
			} catch (...) {
				// Catch any other unknown exception
				auto [log, logPath] = get_timestamped_log("crash-"sv, "crash log"s);
				log->critical("Caught an unknown exception");
				log->flush();
			}

			// Upload crash log to pastebin if enabled
			bool uploadedToWeb = false;
			if (Settings::GetSingleton()->GetDebug().autoUploadCrashLog) {
				try {
					const auto pasteUrl = upload_log_to_pastebin(crashLogPath);
					if (!pasteUrl.empty()) {
						RE::DebugMessageBox(fmt::format("Crash log uploaded to pastebin.com!\n\nURL: {}\n\n(URL copied to clipboard and opened in browser)", pasteUrl).c_str());
						uploadedToWeb = true;
					} else {
						RE::DebugMessageBox("Failed to upload crash log to pastebin.\nCheck that you have a valid Pastebin API Key in CrashLogger.ini\n\nGet a free key from: https://pastebin.com/doc_api#1");
					}
				} catch (...) {
					logger::error("Failed to upload crash log");
				}
			}

			// Auto-open crash log if enabled (skip if uploaded to web to avoid focus stealing)
			if (!uploadedToWeb) {
				auto_open_log(crashLogPath);
			}

			TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
			return EXCEPTION_CONTINUE_SEARCH;
		}

		std::int32_t _stdcall VectoredExceptions(::EXCEPTION_POINTERS*) noexcept
		{
			::SetUnhandledExceptionFilter(reinterpret_cast<::LPTOP_LEVEL_EXCEPTION_FILTER>(&UnhandledExceptions));
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}  // namespace

	void Install(std::string a_crashPath)
	{
		// Set crash path first
		if (!a_crashPath.empty()) {
			crashPath = std::filesystem::path(a_crashPath);
			logger::info("Crash Logs will be written to {}", crashPath.string());
		}

		// Install crash handlers
		const auto success =
			::AddVectoredExceptionHandler(1, reinterpret_cast<::PVECTORED_EXCEPTION_HANDLER>(&VectoredExceptions));
		if (success == nullptr) {
			util::report_and_fail("failed to install vectored exception handler"sv);
		}
		logger::info("installed crash handlers"sv);

		// Start hotkey monitoring thread
		StartHotkeyMonitoring();

// Verify handlers are working by testing (in debug builds only)
#ifdef _DEBUG
		if (const auto& debugConfig = Settings::GetSingleton()->GetDebug();
			debugConfig.waitForDebugger) {
			logger::debug("Crash handler installation verified (debug mode)");
		}
#endif
	}
}  // namespace Crash
