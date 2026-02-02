#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <windows.h>

namespace Crash
{
	// C++ Exception code used by MSVC (0xE06D7363 = "msc" | 0xE0000000)
	// See: https://devblogs.microsoft.com/oldnewthing/20100730-00/?p=13273
	constexpr DWORD CPP_EXCEPTION_CODE = 0xE06D7363;
	constexpr ULONG_PTR CPP_EXCEPTION_MAGIC_X64 = 0x19930520;

	struct CppExceptionInfo
	{
		std::string typeName;
		std::uintptr_t objectAddress = 0;
		std::uintptr_t throwInfoAddress = 0;
		std::uintptr_t moduleBase = 0;
		std::optional<std::string> what;
	};

	bool IsCppException(const EXCEPTION_RECORD& exception) noexcept;
	std::optional<CppExceptionInfo> ParseCppException(const EXCEPTION_RECORD& exception) noexcept;
	std::optional<std::string> TryGetExceptionWhat(std::uintptr_t objectAddress) noexcept;
}
