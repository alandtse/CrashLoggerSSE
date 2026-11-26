#pragma once

#include <cstdint>
#include <filesystem>

namespace Crash::Breadcrumb
{
	enum class Kind : std::uint32_t
	{
		Heartbeat = 0,   // periodic liveness tick (the game exits via TerminateProcess, so no clean-exit marker)
		FatalException,  // fatal-class first-chance exception seen by the vectored handler
		HandlerEntered,  // UnhandledExceptions started
		LogOpened,       // crash log file created
		HandlerDone,     // crash log finished
	};

	// Opens the breadcrumb file for this session after reporting what the previous session left behind.
	void Install(const std::filesystem::path& a_dir);

	// Allocation-free and lock-free so it is safe inside exception handlers.
	void Mark(Kind a_kind, std::uint32_t a_code = 0, std::uintptr_t a_address = 0) noexcept;

	// Exceptions that normally terminate the game; benign first-chance noise is excluded.
	[[nodiscard]] bool IsFatalClass(std::uint32_t a_code) noexcept;
}
