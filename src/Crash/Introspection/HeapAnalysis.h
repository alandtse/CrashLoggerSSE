#pragma once

#include <optional>
#include <string>

namespace Crash::Introspection::Heap
{
	struct HeapInfo
	{
		const void* heap_base;
		std::size_t allocation_size;
		std::string heap_type;
		bool possibly_corrupted;
	};

	// Analyze if a pointer is within a heap and extract metadata
	// Note: Can throw std::bad_alloc if memory exhausted during crash
	[[nodiscard]] std::optional<HeapInfo> analyze_heap_pointer(const void* a_ptr);

	// Get a human-readable description of heap metadata
	// Note: Can throw std::bad_alloc if memory exhausted during crash
	[[nodiscard]] std::string format_heap_info(const HeapInfo& a_info);
}
