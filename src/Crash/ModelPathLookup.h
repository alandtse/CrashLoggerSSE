#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace Crash::ModelPathLookup
{
	// Hooks BSResource::ID::ID() to remember which path produced each 12-byte resource id.
	// Queued model loads keep only that id, so this is the only way back to a path.
	void Install();

	struct Hit
	{
		std::string path;
		std::size_t stackOffset;  // bytes above RSP
	};

	// Finds recorded ids on the faulting thread's stack, either stored in place or at the
	// address of a stack value (an entry pointer), lowest address first.
	[[nodiscard]] std::vector<Hit> FindPathsOnStack(std::span<const std::size_t> a_stack, std::size_t a_limit);
}
