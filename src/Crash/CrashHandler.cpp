#include "Crash/CrashHandler.h"

#include "Crash/Introspection/Introspection.h"
#include "Crash/Modules/ModuleHandler.h"
#include "Crash/PDB/PdbHandler.h"
#include "dxgi1_4.h"
#include <Settings.h>
#include <openvr.h>
#include <vmaware.hpp>
#include <wincrypt.h>
#include <iomanip>
#include <sstream>
#include <magic_enum.hpp>
#undef debug // avoid conflict with vmaware.hpp debug
using namespace vr;

namespace Crash
{
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

	Callstack::Callstack(const ::EXCEPTION_RECORD& a_except)
	{
		try {
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

		// Limit stack frames to prevent excessive memory usage and processing time
		constexpr std::size_t MAX_FRAMES = 500;
		const auto frame_count = std::min(_frames.size(), MAX_FRAMES);
		if (_frames.size() > MAX_FRAMES) {
			a_log.critical("Stack trace truncated to {} frames (original: {})", MAX_FRAMES, _frames.size());
		}

		std::vector<const Modules::Module*> moduleStack;
		moduleStack.reserve(frame_count);
		
		for (std::size_t i = 0; i < frame_count; ++i) {
			try {
				const auto& frame = _frames[i];
				const auto mod = Introspection::get_module_for_pointer(frame.address(), a_modules);
				if (mod && mod->in_range(frame.address())) {
					moduleStack.push_back(mod);
				} else {
					moduleStack.push_back(nullptr);
				}
			} catch (...) {
				// Invalid frame, mark as null and continue
				moduleStack.push_back(nullptr);
			}
		}

		const auto format = get_format([&]() {
			std::size_t max = 0;
			std::for_each(moduleStack.begin(), moduleStack.end(),
				[&](auto&& a_elem) { max = a_elem ? std::max(max, a_elem->name().length()) : max; });
			return max;
		}());

		for (std::size_t i = 0; i < frame_count && i < moduleStack.size(); ++i) {
			try {
				const auto mod = moduleStack[i];
				const auto& frame = _frames[i];
				const auto frame_info = mod ? [&]() {
					try {
						return mod->frame_info(frame);
					} catch (...) {
						return std::string("[frame info error]");
					}
				}() : ""s;
				a_log.critical(fmt::runtime(format), i, reinterpret_cast<std::uintptr_t>(frame.address()), 
					(mod ? mod->name() : ""sv), frame_info);
			} catch (...) {
				a_log.critical("[Frame {} processing failed]", i);
			}
		}
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
		[[nodiscard]] std::shared_ptr<spdlog::logger> get_log()
		{
			std::optional<std::filesystem::path> path = crashPath;
			const auto time = std::time(nullptr);
			std::tm localTime{};
			if (gmtime_s(&localTime, &time) != 0) {
				util::report_and_fail("failed to get current time"sv);
			}

			std::stringstream buf;
			buf << "crash-"sv << std::put_time(&localTime, "%Y-%m-%d-%H-%M-%S") << ".log"sv;
			*path /= buf.str();

			auto sink = std::make_shared<spdlog::sinks::basic_file_sink_st>(path->string(), true);
			auto log = std::make_shared<spdlog::logger>("crash log"s, std::move(sink));
			log->set_pattern("%v"s);
			log->set_level(spdlog::level::trace);
			log->flush_on(spdlog::level::off);

			return log;
		}

		[[nodiscard]] std::string get_file_md5(const std::filesystem::path& filepath)
		{
			const auto get_error_message = [](DWORD error) -> std::string {
				if (error == 0) return "No error";
				
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

				DWORD dwHashLen = 16; // MD5 is always 16 bytes
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

		void print_exception(spdlog::logger& a_log, const ::EXCEPTION_RECORD& a_exception, std::span<const module_pointer> a_modules)
		{
#define EXCEPTION_CASE(a_code) \
	case a_code:               \
		return " \"" #a_code "\""sv

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
				default:
					return ""sv;
				}
			}();

			a_log.critical("Unhandled exception{} at 0x{:012X}{}"sv, exception, eaddr, post);

			// Log exception flags with description
			a_log.critical("Exception Flags: 0x{:08X}{}"sv, a_exception.ExceptionFlags,
				(a_exception.ExceptionFlags & EXCEPTION_NONCONTINUABLE) ? " (Non-continuable)" : 
				(a_exception.ExceptionFlags == 0) ? " (Continuable)" : "");

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
			} else if (a_exception.NumberParameters > 0) {
				a_log.critical("Exception Information Parameters:");
				for (std::size_t i = 0; i < a_exception.NumberParameters; ++i) {
					a_log.critical("\tParameter[{}]: 0x{:012X}"sv, i, a_exception.ExceptionInformation[i]);
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

			using value_type = std::pair<std::string, std::optional<REL::Version>>;
			std::vector<value_type> plugins;
			for (const auto& m : modules) {
				try {
					std::filesystem::path pluginDir{ Crash::PDB::sPluginPath };
					std::filesystem::path filename = pluginDir.append(m);
					if (std::filesystem::exists(filename))
						plugins.emplace_back(*std::move(std::make_optional(m)), REL::GetFileVersion(filename.wstring()));
				} catch (const std::exception& e) {
					a_log.critical("Skipping module {}:{}"sv, m, e.what());
				}
			}
			std::sort(plugins.begin(), plugins.end(),
				[=](const value_type& a_lhs, const value_type& a_rhs) { return ci(a_lhs.first, a_rhs.first); });
			for (const auto& [plugin, version] : plugins) {
				const auto ver = [&]() {
					if (version) {
						std::span view{ version->begin(), version->end() };
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

				a_log.critical("\t{}{}"sv, plugin, ver);
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

		void print_registers(spdlog::logger& a_log, const ::CONTEXT& a_context,
			std::span<const module_pointer> a_modules)
		{
			a_log.critical("REGISTERS:"sv);

			const std::array regs{
				std::make_pair("RAX"sv, a_context.Rax),
				std::make_pair("RCX"sv, a_context.Rcx),
				std::make_pair("RDX"sv, a_context.Rdx),
				std::make_pair("RBX"sv, a_context.Rbx),
				std::make_pair("RSP"sv, a_context.Rsp),
				std::make_pair("RBP"sv, a_context.Rbp),
				std::make_pair("RSI"sv, a_context.Rsi),
				std::make_pair("RDI"sv, a_context.Rdi),
				std::make_pair("R8"sv, a_context.R8),
				std::make_pair("R9"sv, a_context.R9),
				std::make_pair("R10"sv, a_context.R10),
				std::make_pair("R11"sv, a_context.R11),
				std::make_pair("R12"sv, a_context.R12),
				std::make_pair("R13"sv, a_context.R13),
				std::make_pair("R14"sv, a_context.R14),
				std::make_pair("R15"sv, a_context.R15),
			};

			std::array<std::size_t, regs.size()> todo{};
			for (std::size_t i = 0; i < regs.size(); ++i) {
				todo[i] = regs[i].second;
			}
			const auto analysis = Introspection::analyze_data(todo, a_modules);
			for (std::size_t i = 0; i < regs.size(); ++i) {
				const auto& [name, reg] = regs[i];
				a_log.critical("\t{:<3} 0x{:<16X} {}"sv, name, reg, analysis[i]);
			}
		}

		void print_stack(spdlog::logger& a_log, const ::CONTEXT& a_context, std::span<const module_pointer> a_modules)
		{
			a_log.critical("STACK:"sv);

			const auto tib = reinterpret_cast<const ::NT_TIB*>(::NtCurrentTeb());
			const auto base = tib ? static_cast<const std::size_t*>(tib->StackBase) : nullptr;
			if (!base) {
				a_log.critical("\tFAILED TO READ TIB"sv);
			} else {
				const auto rsp = reinterpret_cast<const std::size_t*>(a_context.Rsp);
				std::span stack{ rsp, base };

				const auto format = [&]() {
					return "\t[RSP+{:<"s +
					       fmt::to_string(fmt::format("{:X}"sv, (stack.size() - 1) * sizeof(std::size_t)).length()) +
					       "X}] 0x{:<16X} {}"s;
				}();

				constexpr std::size_t blockSize = 1000;
				std::size_t idx = 0;
				for (std::size_t off = 0; off < stack.size(); off += blockSize) {
					const auto analysis = Introspection::analyze_data(
						stack.subspan(off, std::min<std::size_t>(stack.size() - off, blockSize)), a_modules);
					for (const auto& data : analysis) {
						a_log.critical(fmt::runtime(format), idx * sizeof(std::size_t), stack[idx], data);
						++idx;
					}
				}
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
					
					const auto uptime_ms = (current.QuadPart - creation.QuadPart) / 10000; // Convert to milliseconds
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
					const std::string dir(wdir.begin(), wdir.end());
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
				const auto log = get_log();

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

				// Add timestamp
				const auto now = std::time(nullptr);
				std::tm local_time{};
				if (localtime_s(&local_time, &now) == 0) {
					log->critical("CRASH TIME: {:04}-{:02}-{:02} {:02}:{:02}:{:02}"sv, 
						local_time.tm_year + 1900, local_time.tm_mon + 1, local_time.tm_mday,
						local_time.tm_hour, local_time.tm_min, local_time.tm_sec);
				}
				
				const auto runtimeVer = REL::Module::get().version();
				log->critical("Skyrim {} v{}.{}.{}"sv, REL::Module::IsVR() ? "VR" : "SSE", runtimeVer[0], runtimeVer[1], runtimeVer[2]);
				log->critical("CrashLoggerSSE v{} {} {}"sv, SKSE::PluginDeclaration::GetSingleton()->GetVersion().string(), __DATE__, __TIME__);
				log->flush();

				print([&]() { print_exception(*log, *a_exception->ExceptionRecord, cmodules); }, "print_exception");
				print([&]() { print_process_info(*log); }, "print_process_info");
				print([&]() { print_sysinfo(*log); }, "print_sysinfo");
				if (REL::Module::IsVR())
					print([&]() { print_vrinfo(*log); }, "print_vrinfo");

				print([&]() {
					try {
						const Callstack callstack{ *a_exception->ExceptionRecord };
						callstack.print(*log, cmodules);
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

				print([&]() { print_registers(*log, *a_exception->ContextRecord, cmodules); }, "print_registers");
				print([&]() { print_stack(*log, *a_exception->ContextRecord, cmodules); }, "print_raw_stack");
				print([&]() { print_modules(*log, cmodules); }, "print_modules");
				print([&]() { print_xse_plugins(*log, cmodules); }, "print_xse_plugins");
				print([&]() { print_plugins(*log); }, "print_plugins");

			} catch (const SEHException& se) {
				// Log the SEH exception converted to a C++ exception
				const auto log = get_log();
				log->critical("SEH Exception caught: {} (Code: 0x{:X})", se.what(), se.code());
			} catch (const std::exception& e) {
				// Log the C++ exception
				const auto log = get_log();
				log->critical("Caught C++ exception: {}", e.what());
			} catch (...) {
				// Catch any other unknown exception
				const auto log = get_log();
				log->critical("Caught an unknown exception");
			}

			TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
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
			crashPath = a_crashPath;
			logger::info("Crash Logs will be written to {}", crashPath);
		}
		
		// Install crash handlers
		const auto success =
			::AddVectoredExceptionHandler(1, reinterpret_cast<::PVECTORED_EXCEPTION_HANDLER>(&VectoredExceptions));
		if (success == nullptr) {
			util::report_and_fail("failed to install vectored exception handler"sv);
		}
		logger::info("installed crash handlers"sv);
		
		// Verify handlers are working by testing (in debug builds only)
		#ifdef _DEBUG
		if (const auto& debugConfig = Settings::GetSingleton()->GetDebug(); 
			debugConfig.waitForDebugger) {
			logger::debug("Crash handler installation verified (debug mode)");
		}
		#endif
	}
}  // namespace Crash
