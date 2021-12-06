#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <execution>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>
#include <boost/stacktrace.hpp>
#include <fmt/format.h>
#include <frozen/map.h>
#include <infoware/cpu.hpp>
#include <infoware/gpu.hpp>
#include <infoware/system.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

#include "Plugin.h"

using namespace std::literals;

namespace logger = SKSE::log;

namespace WinAPI
{
	using namespace SKSE::WinAPI;

	inline constexpr auto UNDNAME_NO_MS_KEYWORDS = std::uint32_t{ 0x0002 };
	inline constexpr auto UNDNAME_NO_FUNCTION_RETURNS = std::uint32_t{ 0x0004 };
	inline constexpr auto UNDNAME_NO_ALLOCATION_MODEL = std::uint32_t{ 0x0008 };
	inline constexpr auto UNDNAME_NO_ALLOCATION_LANGUAGE = std::uint32_t{ 0x0010 };
	inline constexpr auto UNDNAME_NO_THISTYPE = std::uint32_t{ 0x0060 };
	inline constexpr auto UNDNAME_NO_ACCESS_SPECIFIERS = std::uint32_t{ 0x0080 };
	inline constexpr auto UNDNAME_NO_THROW_SIGNATURES = std::uint32_t{ 0x0100 };
	inline constexpr auto UNDNAME_NO_RETURN_UDT_MODEL = std::uint32_t{ 0x0400 };
	inline constexpr auto UNDNAME_NAME_ONLY = std::uint32_t{ 0x1000 };
	inline constexpr auto UNDNAME_NO_ARGUMENTS = std::uint32_t{ 0x2000 };

	[[nodiscard]] bool IsDebuggerPresent() noexcept;

	[[nodiscard]] std::uint32_t UnDecorateSymbolName(
		const char* a_name,
		char* a_outputString,
		std::uint32_t a_maxStringLength,
		std::uint32_t a_flags) noexcept;
}

namespace util
{
	using SKSE::stl::adjust_pointer;
	using SKSE::stl::report_and_fail;
	using SKSE::stl::utf16_to_utf8;

	[[nodiscard]] inline auto module_name()
		-> std::string
	{
		const auto filename = REL::Module::get().filename();
		return util::utf16_to_utf8(filename)
		    .value_or("<unknown module name>"s);
	}
}
