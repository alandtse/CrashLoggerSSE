#pragma once

#include <atomic>

namespace Crash::ModelDemandTracker
{
	// Hooks RE::BSModelDB::Demand() to record each thread's most recent model paths.
	void Install();

	// Call only from the faulting thread's own exception handler; newest path first.
	[[nodiscard]] std::vector<std::string> get_recent_paths_newest_first();
}
