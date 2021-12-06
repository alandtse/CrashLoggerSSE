#pragma once

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

		[[nodiscard]] std::vector<std::string> analyze_data(
			std::span<const std::size_t> a_data,
			std::span<const std::unique_ptr<Modules::Module>> a_modules);
	}
}
