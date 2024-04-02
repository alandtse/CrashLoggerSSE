#pragma once

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#include <ClibUtil/simpleINI.hpp>
#include <Psapi.h>
#include <ShlObj_core.h>
#include <boost/stacktrace.hpp>
#include <dia2.h>
#include <diacreate.h>
#include <fmt/format.h>
#include <frozen/map.h>
#include <infoware/cpu.hpp>
#include <infoware/gpu.hpp>
#include <infoware/system.hpp>

#undef cdecl  // Workaround for Clang 14 CMake configure error.

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#undef GetObject  // Have to do this because PCH pulls in spdlog->winbase.h->windows.h->wingdi.h, which redfines GetObject

// Compatible declarations with other sample projects.
#define DLLEXPORT __declspec(dllexport)

using namespace std::literals;
using namespace REL::literals;
namespace string = clib_util::string;
namespace ini = clib_util::ini;

namespace logger = SKSE::log;

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
