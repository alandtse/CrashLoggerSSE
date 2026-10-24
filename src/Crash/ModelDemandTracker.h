#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Crash::ModelDemandTracker
{
	// Hooks RE::BSModelDB::Demand() to record each thread's most recent model paths.
	void Install();

	struct Request
	{
		std::string path;
		double ageMs;
		bool active{ false };  // still executing inside Demand() at crash time
	};

	struct Report
	{
		bool inFlight{ false };
		std::vector<Request> requests;  // newest first
	};

	struct Entry
	{
		std::string path;
		std::int64_t ticks;
	};

	// Conservative starting value pending calibration from real crash logs.
	inline constexpr double kRecentWindowMs = 5.0;

	// Keeps the newest a_activeCount entries plus any other entry within kRecentWindowMs.
	[[nodiscard]] std::vector<Request> select_relevant(
		std::span<const Entry> a_newestFirst,
		std::int64_t a_nowTicks,
		std::int64_t a_ticksPerSecond,
		std::size_t a_activeCount);

	// High-resolution timestamp; capture it as early as possible in the crash handler.
	[[nodiscard]] std::int64_t now_ticks();

	// Call only from the faulting thread's own exception handler; ages are measured to a_faultTicks.
	[[nodiscard]] Report get_report(std::int64_t a_faultTicks);
}
