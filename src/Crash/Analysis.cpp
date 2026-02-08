#include "Crash/Analysis.h"

#include "Crash/Introspection/Introspection.h"

#include <Windows.h>
#include <unordered_set>

namespace Crash
{
	// Get register names and values from CONTEXT
	std::pair<RegisterInfo, RegisterValues> get_register_info(const ::CONTEXT& a_context)
	{
		const RegisterInfo regs{ {
			{ "RAX"sv, a_context.Rax },
			{ "RCX"sv, a_context.Rcx },
			{ "RDX"sv, a_context.Rdx },
			{ "RBX"sv, a_context.Rbx },
			{ "RSP"sv, a_context.Rsp },
			{ "RBP"sv, a_context.Rbp },
			{ "RSI"sv, a_context.Rsi },
			{ "RDI"sv, a_context.Rdi },
			{ "R8"sv, a_context.R8 },
			{ "R9"sv, a_context.R9 },
			{ "R10"sv, a_context.R10 },
			{ "R11"sv, a_context.R11 },
			{ "R12"sv, a_context.R12 },
			{ "R13"sv, a_context.R13 },
			{ "R14"sv, a_context.R14 },
			{ "R15"sv, a_context.R15 },
		} };

		RegisterValues values{};
		for (std::size_t i = 0; i < regs.size(); ++i) {
			values[i] = regs[i].second;
		}

		return std::make_pair(regs, values);
	}

	// Get stack memory span from CONTEXT (using TIB of current thread)
	// NOTE: Only valid for crash logs where CONTEXT is from current thread!
	std::optional<std::span<const std::size_t>> get_stack_info(const ::CONTEXT& a_context)
	{
		const auto tib = reinterpret_cast<const ::NT_TIB*>(::NtCurrentTeb());
		const auto base = tib ? static_cast<const std::size_t*>(tib->StackBase) : nullptr;
		if (!base) {
			return std::nullopt;
		}
		const auto rsp = reinterpret_cast<const std::size_t*>(a_context.Rsp);
		return std::span{ rsp, base };
	}

	// Get stack memory span from CONTEXT with explicit max size (safe for thread dumps)
	// Uses RSP from CONTEXT and scans up to max_bytes (default 64KB)
	std::span<const std::size_t> get_stack_info_safe(const ::CONTEXT& a_context, std::size_t max_bytes)
	{
		const auto rsp = reinterpret_cast<const std::size_t*>(a_context.Rsp);
		const std::size_t max_qwords = max_bytes / sizeof(std::size_t);
		return std::span{ rsp, max_qwords };
	}

	// Analyze register values with introspection
	std::pair<RegisterInfo, std::vector<std::string>> analyze_registers(
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules)
	{
		const auto [regs, regValues] = get_register_info(a_context);
		auto analysis = Introspection::analyze_data(regValues, a_modules, [&](size_t i) {
			return std::string(regs[i].first);
		});
		Introspection::backfill_void_pointers(analysis, regValues);
		return std::make_pair(regs, analysis);
	}

	// Analyze stack memory blocks with introspection
	std::vector<std::vector<std::string>> analyze_stack_blocks(
		std::span<const std::size_t> stack,
		std::span<const module_pointer> a_modules)
	{
		constexpr std::size_t BLOCK_SIZE = 1000;
		const std::size_t scanSize = stack.size();

		std::vector<std::vector<std::string>> all_analysis_results;
		std::vector<std::span<const std::size_t>> all_address_spans;

		for (std::size_t off = 0; off < scanSize; off += BLOCK_SIZE) {
			auto block_span = stack.subspan(off, std::min<std::size_t>(scanSize - off, BLOCK_SIZE));
			auto analysis = Introspection::analyze_data(
				block_span, a_modules, [&](size_t i) {
					return fmt::format("RSP+{:X}", (off + i) * sizeof(std::size_t));
				});
			all_analysis_results.push_back(std::move(analysis));
			all_address_spans.push_back(block_span);
		}

		// Backfill all results at once
		for (std::size_t block_idx = 0; block_idx < all_analysis_results.size(); ++block_idx) {
			Introspection::backfill_void_pointers(all_analysis_results[block_idx], all_address_spans[block_idx]);
		}

		return all_analysis_results;
	}

	// Print registers with pre-analyzed results
	void print_registers(
		spdlog::logger& a_log,
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules,
		const std::vector<std::string>& pre_analyzed)
	{
		a_log.critical("REGISTERS:"sv);

		const auto [regs, regValues] = get_register_info(a_context);
		for (std::size_t i = 0; i < regs.size(); ++i) {
			const auto& [name, reg] = regs[i];
			a_log.critical("\t{:<3} 0x{:<16X} {}"sv, name, reg, pre_analyzed[i]);
		}
	}

	// Print registers with on-the-fly analysis
	void print_registers(
		spdlog::logger& a_log,
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules)
	{
		const auto [regs, analysis] = analyze_registers(a_context, a_modules);
		print_registers(a_log, a_context, a_modules, analysis);
	}

	// Print stack with pre-analyzed results
	void print_stack(
		spdlog::logger& a_log,
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules,
		const std::vector<std::vector<std::string>>& pre_analyzed_blocks)
	{
		const auto stack_opt = get_stack_info(a_context);
		if (!stack_opt) {
			a_log.critical("STACK:"sv);
			a_log.critical("\tFAILED TO READ TIB"sv);
			return;
		}
		const auto& stack = *stack_opt;

		a_log.critical("STACK:"sv);

		const auto format = [&]() {
			return "\t[RSP+{:<"s +
			       fmt::to_string(fmt::format("{:X}"sv, (stack.size() - 1) * sizeof(std::size_t)).length()) +
			       "X}] 0x{:<16X} {}"s;
		}();

		// Print the pre-analyzed backfilled results
		std::size_t global_idx = 0;
		for (std::size_t block_idx = 0; block_idx < pre_analyzed_blocks.size(); ++block_idx) {
			const auto& analysis = pre_analyzed_blocks[block_idx];
			for (std::size_t i = 0; i < analysis.size(); ++i) {
				const auto& data = analysis[i];
				a_log.critical(fmt::runtime(format), global_idx * sizeof(std::size_t), stack[global_idx], data);
				++global_idx;
			}
		}
	}

	// Print stack with on-the-fly analysis
	void print_stack(
		spdlog::logger& a_log,
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules)
	{
		const auto stack_opt = get_stack_info(a_context);
		if (!stack_opt) {
			a_log.critical("STACK:"sv);
			a_log.critical("\tFAILED TO READ TIB"sv);
			return;
		}
		const auto& stack = *stack_opt;

		const auto all_analysis_results = analyze_stack_blocks(stack, a_modules);
		print_stack(a_log, a_context, a_modules, all_analysis_results);
	}

	// Thread-dump-safe versions that use explicit stack bounds instead of TIB

	// Analyze and print registers (thread-safe, same as regular version)
	void print_registers_safe(
		spdlog::logger& a_log,
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules)
	{
		// Register analysis doesn't depend on TIB, so just call regular version
		print_registers(a_log, a_context, a_modules);
	}

	// Analyze and print stack with safe bounds (for thread dumps)
	void print_stack_safe(
		spdlog::logger& a_log,
		const ::CONTEXT& a_context,
		std::span<const module_pointer> a_modules,
		std::size_t max_stack_bytes)
	{
		a_log.critical("STACK:"sv);

		try {
			const auto stack = get_stack_info_safe(a_context, max_stack_bytes);

			const auto format = [&]() {
				return "\t[RSP+{:<"s +
				       fmt::to_string(fmt::format("{:X}"sv, (stack.size() - 1) * sizeof(std::size_t)).length()) +
				       "X}] 0x{:<16X} {}"s;
			}();

			const auto all_analysis_results = analyze_stack_blocks(stack, a_modules);

			// Print the analyzed results
			std::size_t global_idx = 0;
			for (std::size_t block_idx = 0; block_idx < all_analysis_results.size(); ++block_idx) {
				const auto& analysis = all_analysis_results[block_idx];
				for (std::size_t i = 0; i < analysis.size(); ++i) {
					const auto& data = analysis[i];
					a_log.critical(fmt::runtime(format), global_idx * sizeof(std::size_t), stack[global_idx], data);
					++global_idx;
				}
			}
		} catch (const std::exception& e) {
			a_log.critical("\tFailed to analyze stack: {}"sv, e.what());
		} catch (...) {
			a_log.critical("\tFailed to analyze stack: unknown error"sv);
		}
	}

	// Callstack formatting (shared between crash logs and thread dumps)

	// Format a single stack frame with module info, assembly, and PDB symbols
	std::string format_stack_frame(
		const void* a_address,
		const Modules::Module* a_module)
	{
		if (!a_module || !a_module->in_range(a_address)) {
			return ""s;
		}

		try {
			// Use the module's frame_info which includes assembly + PDB symbols
			// This creates a boost::stacktrace::frame temporarily just for formatting
			boost::stacktrace::frame temp_frame(a_address);
			return a_module->frame_info(temp_frame);
		} catch (...) {
			// If frame_info fails, return just the offset
			const auto offset = reinterpret_cast<std::uintptr_t>(a_address) - a_module->address();
			return fmt::format("+{:07X}", offset);
		}
	}

	// Core callstack printing logic - shared by crash logs and thread dumps
	void print_callstack_impl(
		spdlog::logger& a_log,
		std::span<const FrameData> a_frame_data,
		std::string_view a_indent)
	{
		if (a_frame_data.empty()) {
			a_log.critical("{}No stack frames available"sv, a_indent);
			return;
		}

		// Calculate max module name width for formatting
		std::size_t max_name_width = 0;
		for (const auto& frame : a_frame_data) {
			if (frame.module) {
				max_name_width = std::max(max_name_width, frame.module->name().length());
			}
		}

		// Format string for frame index
		const auto index_width = fmt::to_string(a_frame_data.size() - 1).length();
		const auto format = std::string(a_indent) + "[{:>"s + fmt::to_string(index_width) +
		                    "}] 0x{:012X} {:>"s + fmt::to_string(max_name_width) + "}{}"s;

		// Print each frame with detailed info
		for (std::size_t i = 0; i < a_frame_data.size(); ++i) {
			try {
				const auto& frame = a_frame_data[i];
				a_log.critical(
					fmt::runtime(format),
					i,
					reinterpret_cast<std::uintptr_t>(frame.address),
					(frame.module ? frame.module->name() : ""sv),
					frame.frame_info);
			} catch (...) {
				a_log.critical("[Frame {} processing failed]", i);
			}
		}
	}

	// Print a callstack from a list of addresses (for thread dumps)
	void print_callstack(
		spdlog::logger& a_log,
		std::span<const void* const> a_frames,
		std::span<const module_pointer> a_modules)
	{
		// Build frame data
		std::vector<FrameData> frame_data;
		frame_data.reserve(a_frames.size());

		for (const auto addr : a_frames) {
			try {
				const auto mod = Introspection::get_module_for_pointer(addr, a_modules);
				const auto frame_info = mod ? format_stack_frame(addr, mod) : ""s;
				frame_data.push_back({ addr, mod, frame_info });
			} catch (...) {
				frame_data.push_back({ addr, nullptr, "[frame lookup error]"s });
			}
		}

		// Use shared printing logic
		print_callstack_impl(a_log, frame_data, "\t"sv);
	}

	std::vector<const void*> scan_stack_for_frames(
		std::span<const std::size_t> a_stack,
		std::span<const module_pointer> a_modules,
		std::size_t a_max_frames)
	{
		std::vector<const void*> frames;
		frames.reserve(std::min(a_max_frames, a_stack.size()));
		std::unordered_set<const void*> seen;

		for (const auto value : a_stack) {
			if (value == 0) {
				continue;
			}
			const auto addr = reinterpret_cast<const void*>(value);
			const auto mod = Introspection::get_module_for_pointer(addr, a_modules);
			if (!mod || !mod->in_range(addr)) {
				continue;
			}

			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(addr, &mbi, sizeof(mbi))) {
				continue;
			}
			const auto protect = mbi.Protect & 0xFF;
			const bool executable = protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
			                        protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
			if (!executable) {
				continue;
			}
			if (!seen.insert(addr).second) {
				continue;
			}

			frames.push_back(addr);
			if (frames.size() >= a_max_frames) {
				break;
			}
		}

		return frames;
	}

	std::vector<HybridFrame> build_hybrid_callstack(
		std::span<const void* const> a_probable_frames,
		std::span<const std::size_t> a_stack,
		std::span<const module_pointer> a_modules,
		std::size_t a_max_total_frames,
		std::size_t a_max_inserted_frames)
	{
		std::vector<HybridFrame> frames;
		frames.reserve(a_max_total_frames);
		std::unordered_set<const void*> seen;

		auto push_frame = [&](const void* a_addr, HybridFrameSource a_source) {
			if (!a_addr) {
				return;
			}
			if (seen.insert(a_addr).second) {
				frames.push_back({ a_addr, a_source });
			}
		};

		for (const auto addr : a_probable_frames) {
			if (frames.size() >= a_max_total_frames) {
				return frames;
			}
			push_frame(addr, HybridFrameSource::Probable);
		}

		if (a_stack.empty()) {
			return frames;
		}

		const auto reconstructed = scan_stack_for_frames(a_stack, a_modules, a_max_total_frames);
		std::size_t inserted = 0;
		for (const auto addr : reconstructed) {
			if (frames.size() >= a_max_total_frames || inserted >= a_max_inserted_frames) {
				break;
			}
			if (seen.insert(addr).second) {
				frames.push_back({ addr, HybridFrameSource::StackScan });
				++inserted;
			}
		}

		return frames;
	}

	void print_reconstructed_callstack(
		spdlog::logger& a_log,
		std::span<const std::size_t> a_stack,
		std::span<const module_pointer> a_modules)
	{
		a_log.critical("RECONSTRUCTED CALL STACK (STACK SCAN):"sv);

		const auto frames = scan_stack_for_frames(a_stack, a_modules);
		if (frames.empty()) {
			a_log.critical("\tNone found"sv);
			return;
		}

		std::vector<FrameData> frame_data;
		frame_data.reserve(frames.size());
		for (const auto addr : frames) {
			try {
				const auto mod = Introspection::get_module_for_pointer(addr, a_modules);
				const auto frame_info = mod ? format_stack_frame(addr, mod) : ""s;
				frame_data.push_back({ addr, mod, frame_info });
			} catch (...) {
				frame_data.push_back({ addr, nullptr, "[frame lookup error]"s });
			}
		}

		print_callstack_impl(a_log, frame_data, "\t"sv);
	}

	void print_hybrid_callstack(
		spdlog::logger& a_log,
		std::span<const void* const> a_probable_frames,
		std::span<const std::size_t> a_stack,
		std::span<const module_pointer> a_modules,
		std::size_t a_max_total_frames,
		std::size_t a_max_inserted_frames)
	{
		a_log.critical("CALL STACK ([P]robable / [S]tack scan):"sv);

		const auto frames = build_hybrid_callstack(
			a_probable_frames,
			a_stack,
			a_modules,
			a_max_total_frames,
			a_max_inserted_frames);
		if (frames.empty()) {
			a_log.critical("\tNone found"sv);
			return;
		}

		std::vector<FrameData> frame_data;
		std::vector<char> source_tags;
		frame_data.reserve(frames.size());
		source_tags.reserve(frames.size());
		for (const auto& frame : frames) {
			try {
				const auto mod = Introspection::get_module_for_pointer(frame.address, a_modules);
				auto frame_info = mod ? format_stack_frame(frame.address, mod) : ""s;
				frame_data.push_back({ frame.address, mod, std::move(frame_info) });
				source_tags.push_back(frame.source == HybridFrameSource::Probable ? 'P' : 'S');
			} catch (...) {
				frame_data.push_back({ frame.address, nullptr, "[frame lookup error]"s });
				source_tags.push_back('?');
			}
		}

		std::size_t max_name_width = 0;
		for (const auto& frame : frame_data) {
			if (frame.module) {
				max_name_width = std::max(max_name_width, frame.module->name().length());
			}
		}

		const auto index_width = fmt::to_string(frame_data.size() - 1).length();
		const auto format = std::string("\t") + "[{:>"s + fmt::to_string(index_width) + "}][{}] 0x{:012X} {:>"s +
		                    fmt::to_string(max_name_width) + "}{}"s;

		for (std::size_t i = 0; i < frame_data.size(); ++i) {
			try {
				const auto& frame = frame_data[i];
				a_log.critical(
					fmt::runtime(format),
					i,
					source_tags[i],
					reinterpret_cast<std::uintptr_t>(frame.address),
					(frame.module ? frame.module->name() : ""sv),
					frame.frame_info);
			} catch (...) {
				a_log.critical("[Frame {} processing failed]", i);
			}
		}
	}

	// Minidump generation (shared between crash logs and thread dumps)

	bool write_minidump(
		const std::filesystem::path& a_path,
		::EXCEPTION_POINTERS* a_exception,
		::HANDLE a_thread)
	{
		try {
			// Create minidump file
			const auto file = ::CreateFileW(
				a_path.c_str(),
				GENERIC_WRITE,
				0,
				nullptr,
				CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);

			if (file == INVALID_HANDLE_VALUE) {
				return false;
			}

			// Prepare minidump info
			::MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
			::MINIDUMP_EXCEPTION_INFORMATION* exceptionPtr = nullptr;

			if (a_exception) {
				exceptionInfo.ThreadId = ::GetCurrentThreadId();
				exceptionInfo.ExceptionPointers = a_exception;
				exceptionInfo.ClientPointers = FALSE;
				exceptionPtr = &exceptionInfo;
			}

			// Write minidump with full memory
			const auto dumpType = static_cast<::MINIDUMP_TYPE>(
				MiniDumpWithFullMemory |
				MiniDumpWithHandleData |
				MiniDumpWithThreadInfo |
				MiniDumpWithUnloadedModules);

			const auto result = ::MiniDumpWriteDump(
				::GetCurrentProcess(),
				::GetCurrentProcessId(),
				file,
				dumpType,
				exceptionPtr,
				nullptr,
				nullptr);

			::CloseHandle(file);
			return result != 0;

		} catch (...) {
			return false;
		}
	}

}  // namespace Crash
