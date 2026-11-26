#include "Crash/Breadcrumb.h"

namespace Crash::Breadcrumb
{
	namespace
	{
		struct Record
		{
			std::uint32_t magic;
			std::uint32_t kind;
			std::uint32_t code;
			std::uint32_t threadId;
			std::uint64_t tick;
			std::uint64_t address;
			std::uint32_t seq;
			std::uint32_t pad[7];
		};
		static_assert(sizeof(Record) == 0x40);

		constexpr std::uint32_t MAGIC = 0x4243524C;  // "LRCB"
		constexpr std::size_t RING_SLOTS = 16;
		// slots 0..5 hold one record per milestone kind; the rest form a ring of fatal exceptions
		constexpr std::size_t MILESTONE_SLOTS = 6;
		constexpr std::size_t TOTAL_SLOTS = MILESTONE_SLOTS + RING_SLOTS;

		HANDLE g_file{ INVALID_HANDLE_VALUE };
		std::atomic<std::uint32_t> g_seq{ 0 };
		std::atomic<std::uint32_t> g_ring{ 0 };

		void write_slot(std::size_t a_slot, const Record& a_record) noexcept
		{
			if (g_file == INVALID_HANDLE_VALUE) {
				return;
			}
			OVERLAPPED ov{};
			ov.Offset = static_cast<DWORD>(a_slot * sizeof(Record));
			DWORD written = 0;
			::WriteFile(g_file, &a_record, sizeof(a_record), &written, &ov);
		}

		std::vector<Record> read_previous(const std::filesystem::path& a_path)
		{
			std::vector<Record> records;
			const auto file = ::CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return records;
			}
			for (std::size_t i = 0; i < TOTAL_SLOTS; ++i) {
				Record record{};
				DWORD read = 0;
				if (!::ReadFile(file, &record, sizeof(record), &read, nullptr) || read != sizeof(record)) {
					break;
				}
				if (record.magic == MAGIC) {
					records.push_back(record);
				}
			}
			::CloseHandle(file);
			return records;
		}

		void report_previous(const std::vector<Record>& a_records)
		{
			if (a_records.empty()) {
				return;
			}
			const Record* milestones[MILESTONE_SLOTS]{};
			const Record* lastFatal = nullptr;
			for (const auto& record : a_records) {
				if (record.kind == static_cast<std::uint32_t>(Kind::FatalException)) {
					if (!lastFatal || record.seq > lastFatal->seq) {
						lastFatal = &record;
					}
				} else if (record.kind < std::size(milestones)) {
					milestones[record.kind] = &record;
				}
			}
			const auto* entered = milestones[static_cast<std::size_t>(Kind::HandlerEntered)];
			const auto* opened = milestones[static_cast<std::size_t>(Kind::LogOpened)];
			const auto* done = milestones[static_cast<std::size_t>(Kind::HandlerDone)];
			const auto* heartbeat = milestones[static_cast<std::size_t>(Kind::Heartbeat)];
			if (done) {
				logger::info("Previous session ended with a complete crash log"sv);
				return;
			}
			if (entered) {
				logger::warn("Previous session: crash handler started (exception 0x{:08X}) but {}. The crash log may be missing or incomplete",
					entered->code,
					opened ? "did not finish the log" : "never created the log file");
				return;
			}
			// Fatal-class exceptions are often handled and harmless. One that is the last thing the
			// session recorded before it stopped is the signature of a crash that never reached us.
			constexpr std::uint64_t WINDOW_MS = 10'000;
			if (lastFatal && heartbeat && heartbeat->tick >= lastFatal->tick &&
				heartbeat->tick - lastFatal->tick <= WINDOW_MS) {
				logger::warn(
					"Previous session stopped {} ms after a fatal-class exception (0x{:08X} at 0x{:012X} on thread {}) "
					"without the crash handler running. The exception was likely handled by other code or the process was "
					"terminated before the crash handler could run",
					heartbeat->tick - lastFatal->tick, lastFatal->code, lastFatal->address, lastFatal->threadId);
			}
		}

	}

	bool IsFatalClass(std::uint32_t a_code) noexcept
	{
		switch (a_code) {
		case 0xC0000005:  // access violation
		case 0xC0000006:  // in-page error
		case 0xC000001D:  // illegal instruction
		case 0xC0000025:  // noncontinuable exception
		case 0xC0000094:  // integer divide by zero
		case 0xC0000096:  // privileged instruction
		case 0xC00000FD:  // stack overflow
		case 0xC0000374:  // heap corruption
		case 0xC0000409:  // fast fail / stack buffer overrun
		case 0xC0000602:  // fail fast exception
			return true;
		default:
			return false;
		}
	}

	void Install(const std::filesystem::path& a_dir)
	{
		try {
			const auto path = a_dir / "crashlogger-breadcrumb.bin";
			report_previous(read_previous(path));
			g_file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (g_file == INVALID_HANDLE_VALUE) {
				logger::warn("Could not open crash breadcrumb file {}"sv, path.string());
			} else {
				// The game exits through TerminateProcess, so a clean-exit marker never runs; a
				// heartbeat lets the next session see how long this one stayed alive.
				std::thread([]() {
					for (;;) {
						Mark(Kind::Heartbeat);
						::Sleep(2000);
					}
				}).detach();
			}
		} catch (...) {
			logger::warn("Crash breadcrumb setup failed"sv);
		}
	}

	void Mark(Kind a_kind, std::uint32_t a_code, std::uintptr_t a_address) noexcept
	{
		Record record{};
		record.magic = MAGIC;
		record.kind = static_cast<std::uint32_t>(a_kind);
		record.code = a_code;
		record.threadId = ::GetCurrentThreadId();
		record.tick = ::GetTickCount64();
		record.address = a_address;
		record.seq = g_seq.fetch_add(1) + 1;
		const auto kind = static_cast<std::size_t>(a_kind);
		if (a_kind == Kind::FatalException) {
			write_slot(MILESTONE_SLOTS + (g_ring.fetch_add(1) % RING_SLOTS), record);
		} else if (kind < MILESTONE_SLOTS) {
			write_slot(kind, record);
		}
	}
}
