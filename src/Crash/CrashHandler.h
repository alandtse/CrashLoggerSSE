#pragma once

#include <vector>

struct _EXCEPTION_RECORD;

namespace Crash
{
	namespace Modules
	{
		class Module;
	}

	class Callstack
	{
	public:
		Callstack(const ::_EXCEPTION_RECORD& a_except);

		Callstack(boost::stacktrace::stacktrace a_stacktrace) :
			_stacktrace{ std::move(a_stacktrace) },
			_frames{ _stacktrace.begin(), _stacktrace.end() }
		{}

		void print(
			spdlog::logger& a_log,
			std::span<const std::unique_ptr<Modules::Module>> a_modules) const;

		// Get the throw location for C++ exceptions (frame after KERNELBASE/VCRUNTIME)
		// Returns empty string if not found
		[[nodiscard]] std::string get_throw_location(
			std::span<const std::unique_ptr<Modules::Module>> a_modules) const;

		[[nodiscard]] std::vector<std::string> get_frame_info_strings(
			std::span<const std::unique_ptr<Modules::Module>> a_modules,
			std::size_t a_max_frames = 50) const;

		[[nodiscard]] std::vector<const void*> get_frame_addresses(
			std::size_t a_max_frames = 500) const;

	private:
		[[nodiscard]] static std::string get_size_string(std::size_t a_size);

		[[nodiscard]] std::string get_format(std::size_t a_nameWidth) const;

		void print_probable_callstack(
			spdlog::logger& a_log,
			std::span<const std::unique_ptr<Modules::Module>> a_modules) const;

		void print_raw_callstack(spdlog::logger& a_log) const;

		boost::stacktrace::stacktrace _stacktrace;
		std::span<const boost::stacktrace::frame> _frames;
	};

	void Install(std::string a_crashPath);

	// Developer crash testing functions
	void TriggerTestCrash(int crashType);
}
