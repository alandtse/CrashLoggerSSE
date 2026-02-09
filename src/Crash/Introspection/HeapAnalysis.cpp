#include "HeapAnalysis.h"

#include "Settings.h"

#include <Windows.h>

namespace Crash::Introspection::Heap
{
	namespace
	{
		[[nodiscard]] bool is_valid_heap_handle(HANDLE a_heap) noexcept
		{
			if (!a_heap || a_heap == INVALID_HANDLE_VALUE) {
				return false;
			}

			try {
				// Try to query heap information to validate handle
				ULONG heap_info = 0;
				if (::HeapQueryInformation(a_heap, HeapCompatibilityInformation,
						&heap_info, sizeof(heap_info), nullptr)) {
					return true;
				}
			} catch (...) {
				return false;
			}

			return false;
		}

		[[nodiscard]] std::optional<HeapInfo> check_process_heaps(
			const void* a_ptr,
			int a_max_heaps,
			int a_max_iterations) noexcept
		{
			try {
				// Get all process heaps
				DWORD num_heaps = ::GetProcessHeaps(0, nullptr);
				if (num_heaps == 0 || num_heaps > 1024) {  // Sanity check
					return std::nullopt;
				}

				std::vector<HANDLE> heaps(num_heaps);
				if (::GetProcessHeaps(num_heaps, heaps.data()) == 0) {
					return std::nullopt;
				}

				// Limit heaps to check (0 = check all)
				const auto heaps_to_check = (a_max_heaps > 0) ?
				                                std::min(static_cast<DWORD>(a_max_heaps), num_heaps) :
				                                num_heaps;

				for (DWORD heap_idx = 0; heap_idx < heaps_to_check; ++heap_idx) {
					const auto heap = heaps[heap_idx];
					if (!is_valid_heap_handle(heap)) {
						continue;
					}

					// Lock heap for safe traversal
					if (!::HeapLock(heap)) {
						continue;
					}

					PROCESS_HEAP_ENTRY entry{};
					bool found = false;
					HeapInfo info{};

					// Limit iterations (0 = unlimited)
					std::size_t iterations = 0;
					const auto max_iterations = static_cast<std::size_t>(a_max_iterations);

					while (::HeapWalk(heap, &entry) != FALSE) {
						if (a_max_iterations > 0) {
							++iterations;
							if (iterations > max_iterations) {
								break;
							}
						}

						if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
							const auto block_start = reinterpret_cast<const std::byte*>(entry.lpData);
							const auto block_end = block_start + entry.cbData;
							const auto ptr_byte = reinterpret_cast<const std::byte*>(a_ptr);

							if (ptr_byte >= block_start && ptr_byte < block_end) {
								info.heap_base = entry.lpData;
								info.allocation_size = entry.cbData;
								info.heap_type = (heap == ::GetProcessHeap()) ? "Process Heap" : "Private Heap";
								info.possibly_corrupted = false;

								// Check for potential corruption indicators
								if (entry.cbData > 0x10000000) {  // > 256MB is suspicious
									info.possibly_corrupted = true;
								}

								found = true;
								break;
							}
						}
					}

					::HeapUnlock(heap);

					if (found) {
						return info;
					}
				}
			} catch (...) {
				// Heap traversal failed, possibly corrupted
			}

			return std::nullopt;
		}
	}

	std::optional<HeapInfo> analyze_heap_pointer(const void* a_ptr) noexcept
	{
		if (!a_ptr) {
			return std::nullopt;
		}

		// Check configuration
		const auto settings = Settings::GetSingleton();
		if (!settings || !settings->GetDebug().enableHeapAnalysis) {
			return std::nullopt;
		}

		const auto max_heaps = settings->GetDebug().maxHeapsToCheck;
		const auto max_iterations = settings->GetDebug().maxHeapIterationsPerHeap;

		// Check if pointer is in process heaps
		return check_process_heaps(a_ptr, max_heaps, max_iterations);
	}

	std::string format_heap_info(const HeapInfo& a_info) noexcept
	{
		std::string result = a_info.heap_type;
		result += ", size=" + std::to_string(a_info.allocation_size);

		if (a_info.possibly_corrupted) {
			result += " [POSSIBLY CORRUPTED]";
		}

		return result;
	}
}
