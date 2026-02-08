#pragma once

#include <functional>
#include <string>

namespace Crash
{
	namespace Modules
	{
		class Module;
	}

	namespace Introspection
	{
		[[nodiscard]] const Modules::Module* get_module_for_pointer(
			const void* a_ptr,
			std::span<const std::unique_ptr<Modules::Module>> a_modules) noexcept;

		// Reset introspection state for a new crash analysis
		// Should be called once at the beginning of crash analysis, not per block
		void reset_analysis_state() noexcept;

		[[nodiscard]] std::vector<std::string> analyze_data(
			std::span<const std::size_t> a_data,
			std::span<const std::unique_ptr<Modules::Module>> a_modules,
			std::function<std::string(size_t)> a_label_generator = nullptr);

		// Backfill void* entries in analysis results with known object information
		void backfill_void_pointers(std::vector<std::string>& a_results, std::span<const std::size_t> a_addresses);

		// Check if an address is a game object (polymorphic type with introspection)
		// Returns false for void* pointers with module info (those are not game objects)
		[[nodiscard]] bool was_introspected(const void* a_ptr) noexcept;
	}
}
