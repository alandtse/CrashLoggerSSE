#pragma once

#include <cassert>
#include <cctype>
#include <cerrno>
#include <cfenv>
#include <cfloat>
#include <cinttypes>
#include <climits>
#include <clocale>
#include <cmath>
#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cuchar>
#include <cwchar>
#include <cwctype>

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <bitset>
#include <charconv>
#include <chrono>
#include <compare>
#include <complex>
#include <concepts>
#include <condition_variable>
#include <deque>
#include <exception>
#include <execution>
#include <filesystem>
#include <format>
#include <forward_list>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <iomanip>
#include <iosfwd>
#include <ios>
#include <iostream>
#include <istream>
#include <iterator>
#include <latch>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <numbers>
#include <numeric>
#include <optional>
#include <ostream>
#include <queue>
#include <random>
#include <ranges>
#include <regex>
#include <ratio>
#include <scoped_allocator>
#include <semaphore>
#include <set>
#include <shared_mutex>
#include <source_location>
#include <span>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <syncstream>
#include <system_error>
#include <thread>
#include <tuple>
#include <typeindex>
#include <typeinfo>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <valarray>
#include <variant>
#include <vector>
#include <version>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <REL/Relocation.h>

#include <ShlObj_core.h>
#include <Windows.h>
#include <Psapi.h>
#include <boost/stacktrace.hpp>
#include <fmt/format.h>
#include <frozen/map.h>
#include <infoware/cpu.hpp>
#include <infoware/gpu.hpp>
#include <infoware/system.hpp>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>


// Compatible declarations with other sample projects.
#define DLLEXPORT __declspec(dllexport)

// Compatible declarations with other sample projects.
using namespace std::literals;
using namespace REL::literals;

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
