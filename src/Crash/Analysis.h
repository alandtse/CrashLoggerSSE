#pragma once

#include "Crash/Modules/ModuleHandler.h"

namespace spdlog
{
	class logger;
}

namespace Crash
{
	// Shared data structures for register and stack analysis

	// Register information (name + value pairs)
	using RegisterInfo = std::array<std::pair<std::string_view, std::size_t>, 16>;
	using RegisterValues = std::array<std::size_t, 16>;

	// Get register names and values from CONTEXT
	[[nodiscard]] std::pair<RegisterInfo, RegisterValues> get_register_info(const ::CONTEXT& a_context);

	// Get stack memory span from CONTEXT (using TIB of current thread)
	// NOTE: Only valid for crash logs where CONTEXT is from current thread!
	[[nodiscard]] std::optional<std::span<const std::size_t>> get_stack_info(const ::CONTEXT& a_context);

	// Get stack memory span from CONTEXT with explicit max size (safe for thread dumps)
	// Uses RSP from CONTEXT and scans up to max_bytes (default 64KB)
	[[nodiscard]] std::span<const std::size_t> get_stack_info_safe(const ::CONTEXT& a_context, std::size_t max_bytes = 65536);

	// Analyze register values with introspection
	// Returns: pair of (register info, vector of analysis strings)
	[[nodiscard]] std::pair<RegisterInfo, std::vector<std::string>> analyze_registers(
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules);

	// Analyze stack memory blocks with introspection
	// Returns: vector of blocks, each block is a vector of analysis strings
	[[nodiscard]] std::vector<std::vector<std::string>> analyze_stack_blocks(
		std::span<const std::size_t> stack,
		std::span<const module_pointer> a_modules);

	// Print registers with introspection
	void print_registers(
		spdlog::logger& a_log,
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules,
		const std::vector<std::string>& pre_analyzed);

	// Print registers with on-the-fly analysis
	void print_registers(
		spdlog::logger& a_log,
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules);

	// Print stack with introspection
	void print_stack(
		spdlog::logger& a_log,
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules,
		const std::vector<std::vector<std::string>>& pre_analyzed_blocks);

	// Print stack with on-the-fly analysis
	void print_stack(
		spdlog::logger& a_log,
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules);

	// Thread-dump-safe versions that use explicit stack bounds instead of TIB

	// Analyze and print registers (thread-safe, same as regular version)
	void print_registers_safe(
		spdlog::logger& a_log,
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules);

	// Analyze and print stack with safe bounds (for thread dumps)
	void print_stack_safe(
		spdlog::logger& a_log,
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules,
		std::size_t max_stack_bytes = 65536);

	// Callstack formatting (shared between crash logs and thread dumps)

	// Format a single stack frame with module info, assembly, and PDB symbols
	// Returns formatted string like: "+0x123456\tmov rax, rbx | FunctionName at file.cpp:42"
	[[nodiscard]] std::string format_stack_frame(
		const void* a_address,
		const Modules::Module* a_module);

	// Generic frame data for unified callstack printing
	struct FrameData
	{
		const void* address;
		const Modules::Module* module;
		std::string frame_info;
	};

	// Core callstack printing logic - shared by crash logs and thread dumps
	// Takes pre-processed frame data and prints with consistent formatting
	void print_callstack_impl(
		spdlog::logger& a_log,
		std::span<const FrameData> a_frame_data,
		std::string_view a_indent = "\t"sv);

	// Print a callstack from a list of addresses (for thread dumps)
	void print_callstack(
		spdlog::logger& a_log,
		std::span<const void* const> a_frames,
		std::span<const module_pointer> a_modules);

}  // namespace Crash
