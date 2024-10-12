#include "Crash/CrashHandler.h"

#include "Crash/Introspection/Introspection.h"
#include "Crash/Modules/ModuleHandler.h"
#include "Crash/PDB/PdbHandler.h"
#include "dxgi1_4.h"
#include <Settings.h>
#include <openvr.h>
#include <vmaware.hpp>
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
		const auto exceptionAddress = reinterpret_cast<std::uintptr_t>(a_except.ExceptionAddress);
		auto it = std::find_if(_stacktrace.cbegin(), _stacktrace.cend(), [&](auto&& a_elem) noexcept {
			return reinterpret_cast<std::uintptr_t>(a_elem.address()) == exceptionAddress;
		});

		if (it == _stacktrace.cend()) {
			it = _stacktrace.cbegin();
		}

		_frames = std::span(it, _stacktrace.cend());
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

		std::vector<const Modules::Module*> moduleStack;
		moduleStack.reserve(_frames.size());
		for (const auto& frame : _frames) {
			const auto mod = Introspection::get_module_for_pointer(frame.address(), a_modules);
			if (mod && mod->in_range(frame.address())) {
				moduleStack.push_back(mod);
			} else {
				moduleStack.push_back(nullptr);
			}
		}

		const auto format = get_format([&]() {
			std::size_t max = 0;
			std::for_each(moduleStack.begin(), moduleStack.end(),
				[&](auto&& a_elem) { max = a_elem ? std::max(max, a_elem->name().length()) : max; });
			return max;
		}());

		for (std::size_t i = 0; i < _frames.size(); ++i) {
			const auto mod = moduleStack[i];
			const auto& frame = _frames[i];
			a_log.critical(fmt::runtime(format), i, reinterpret_cast<std::uintptr_t>(frame.address()), (mod ? mod->name() : ""sv),
				(mod ? mod->frame_info(frame) : ""s));
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

			// Log exception flags
			a_log.critical("Exception Flags: 0x{:08X}"sv, a_exception.ExceptionFlags);

			// Log number of parameters
			a_log.critical("Number of Parameters: {}"sv, a_exception.NumberParameters);

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

			// Check for nested exceptions
			if (a_exception.ExceptionRecord) {
				a_log.critical("Nested Exception:");
				print_exception(a_log, *a_exception.ExceptionRecord, a_modules);  // Recursively print nested exception
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

			//https://forums.unrealengine.com/t/how-to-get-vram-usage-via-c/218627/2
			IDXGIFactory4* pFactory{};
			CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&pFactory);
			IDXGIAdapter3* adapter{};
			pFactory->EnumAdapters(0, reinterpret_cast<IDXGIAdapter**>(&adapter));
			DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfo;
			adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfo);
			a_log.critical("\tGPU MEMORY: {:.02f}/{:.02f} GB"sv, gibibyte(videoMemoryInfo.CurrentUsage),
				gibibyte(videoMemoryInfo.Budget));

			// Detect VM
			if (VM::detect(VM::DISABLE(VM::GAMARUE))) {
				a_log.critical("\tDetected Virtual Machine: {} ({}%)"sv, VM::brand(VM::MULTIPLE), VM::percentage());
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
						a_log.critical("\tUnable to initialize VR"sv);
						return;
					}
					const std::vector<std::pair<std::string, ETrackedDeviceProperty>> propListString{
						{ "Model", Prop_ModelNumber_String },
						{ "Manufacturer", Prop_ManufacturerName_String },
						{ "Tracking System", Prop_TrackingSystemName_String },
						{ "Hardware Revision", Prop_HardwareRevision_String },
						{ "Driver Version", Prop_DriverVersion_String },
						{ "Render Model", Prop_RenderModelName_String },
						{ "Additional Data", Prop_AdditionalSystemReportData_String },
						{ "Expected Controller Type", Prop_ExpectedControllerType_String },
						{ "Controller Type", Prop_ControllerType_String }
					};
					const std::vector<std::pair<std::string, ETrackedDeviceProperty>> propListFloat{
						//{ "Battery %", Prop_DeviceBatteryPercentage_Float },
						//{ "Power Usage", Prop_DevicePowerUsage_Float }, // maybe be future value
						{ "Display Frequency", Prop_DisplayFrequency_Float }
					};
					const std::vector<std::pair<std::string, ETrackedDeviceProperty>> propListBool{
						{ "Wireless", Prop_DeviceIsWireless_Bool },
						{ "Charging", Prop_DeviceIsCharging_Bool },
						{ "Update Available", Prop_Firmware_UpdateAvailable_Bool },
					};
					char propValue[k_unMaxPropertyStringSize];
					for (const auto& entry : propListString) {
						HMD->GetStringTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, entry.second, propValue, std::size(propValue));
						a_log.critical("\t{}: {}"sv, entry.first, propValue);
					}
					for (const auto& entry : propListFloat) {
						const auto propValue = HMD->GetFloatTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, entry.second);
						a_log.critical("\t{}: {}"sv, entry.first, propValue);
					}
					for (const auto& entry : propListBool) {
						const auto propValue = HMD->GetBoolTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, entry.second);
						a_log.critical("\t{}: {}"sv, entry.first, propValue);
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
						a_functor();
					} catch (const std::exception& e) {
						log->critical("\t{}:\t{}"sv, a_name, e.what());
					} catch (...) {
						log->critical("\t{}:\tERROR"sv, a_name);
					}
					log->flush();
				};

				const auto runtimeVer = REL::Module::get().version();
				log->critical("Skyrim {} v{}.{}.{}"sv, REL::Module::IsVR() ? "VR" : "SSE", runtimeVer[0], runtimeVer[1], runtimeVer[2]);
				log->critical("CrashLoggerSSE v{} {} {}"sv, SKSE::PluginDeclaration::GetSingleton()->GetVersion().string(), __DATE__, __TIME__);
				log->flush();

				print([&]() { print_exception(*log, *a_exception->ExceptionRecord, cmodules); }, "print_exception");
				print([&]() { print_sysinfo(*log); }, "print_sysinfo");
				if (REL::Module::IsVR())
					print([&]() { print_vrinfo(*log); }, "print_vrinfo");

				print([&]() {
					const Callstack callstack{ *a_exception->ExceptionRecord };
					callstack.print(*log, cmodules);
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
		const auto success =
			::AddVectoredExceptionHandler(1, reinterpret_cast<::PVECTORED_EXCEPTION_HANDLER>(&VectoredExceptions));
		if (success == nullptr) {
			util::report_and_fail("failed to install vectored exception handler"sv);
		}
		logger::info("installed crash handlers"sv);
		if (!a_crashPath.empty()) {
			crashPath = a_crashPath;
			logger::info("Crash Logs will be written to {}", crashPath);
		}
	}
}  // namespace Crash
