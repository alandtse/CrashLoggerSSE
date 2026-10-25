#include "Crash/ModelDemandTracker.h"

#include "Settings.h"

namespace Crash::ModelDemandTracker
{
	namespace
	{
		struct Slot
		{
			static constexpr std::size_t kCapacity = 8;
			static constexpr std::size_t kPathSize = 260;

			// 0 == unclaimed; once claimed, only the owning thread writes the other fields.
			std::atomic<std::uint32_t> ownerThreadId{ 0 };
			std::uint32_t head{ 0 };
			char paths[kCapacity][kPathSize]{};
			std::int64_t ticks[kCapacity]{};
			bool active[kCapacity]{};
		};

		// Static storage, not thread_local: TLS in a LoadLibrary'd DLL is unsafe on raw worker threads.
		constexpr std::size_t kNumSlots = 64;
		Slot g_slots[kNumSlots];

		std::int64_t g_ticksPerSecond{ 1 };

		Slot* FindOrClaimSlot(std::uint32_t a_threadId)
		{
			for (auto& slot : g_slots) {
				if (slot.ownerThreadId.load(std::memory_order_relaxed) == a_threadId)
					return &slot;
			}
			for (auto& slot : g_slots) {
				std::uint32_t expected = 0;
				if (slot.ownerThreadId.compare_exchange_strong(expected, a_threadId, std::memory_order_relaxed))
					return &slot;
			}
			return nullptr;  // pool exhausted: drop rather than block or allocate
		}

		// Unwinds even when the original throws, so a handled exception leaves no false in-flight marker.
		struct InFlightGuard
		{
			explicit InFlightGuard(bool& a_active) :
				active(a_active)
			{
				active = true;
			}

			~InFlightGuard() { active = false; }

			bool& active;
		};

		using Demand_t = RE::BSResource::ErrorCode (*)(const char*, RE::NiPointer<RE::NiNode>&, const RE::BSModelDB::DBTraits::ArgsType&);

		// Call-through target; write_branch's return value is only valid for call/jmp sites, not this prologue.
		Demand_t g_original = nullptr;

		RE::BSResource::ErrorCode HookedDemand(
			const char* a_modelPath,
			RE::NiPointer<RE::NiNode>& a_modelOut,
			const RE::BSModelDB::DBTraits::ArgsType& a_args)
		{
			if (a_modelPath && *a_modelPath) {
				if (auto* slot = FindOrClaimSlot(::GetCurrentThreadId())) {
					const auto index = slot->head % Slot::kCapacity;
					auto& path = slot->paths[index];
					std::strncpy(path, a_modelPath, Slot::kPathSize - 1);
					path[Slot::kPathSize - 1] = '\0';

					LARGE_INTEGER now{};
					::QueryPerformanceCounter(&now);
					slot->ticks[index] = now.QuadPart;
					++slot->head;

					const InFlightGuard guard{ slot->active[index] };
					return g_original(a_modelPath, a_modelOut, a_args);
				}
			}

			return g_original(a_modelPath, a_modelOut, a_args);
		}

		// Re-emits the displaced prologue (push rdi; sub rsp, 0x30) and resumes after it.
		struct EntryTrampoline final : Xbyak::CodeGenerator
		{
			explicit EntryTrampoline(std::uintptr_t a_resume)
			{
				push(rdi);
				sub(rsp, 0x30);
				jmp(ptr[rip]);
				dq(a_resume);
			}
		};
	}

	void Install()
	{
		if (!Settings::GetSingleton()->GetDebug().enableModelPathTracking)
			return;

		LARGE_INTEGER frequency{};
		if (::QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
			g_ticksPerSecond = frequency.QuadPart;

		REL::Relocation<Demand_t> target{ RELOCATION_ID(74040, 75782) };

		static constexpr std::array<std::uint8_t, 6> kExpected{ 0x40, 0x57, 0x48, 0x83, 0xEC, 0x30 };
		if (!std::equal(kExpected.begin(), kExpected.end(), reinterpret_cast<const std::uint8_t*>(target.address()))) {
			logger::warn("skipping BSModelDB::Demand model-path tracker: patch-site signature mismatch"sv);
			return;
		}

		auto& trampoline = SKSE::GetTrampoline();

		EntryTrampoline stub(target.address() + kExpected.size());
		stub.ready();
		g_original = reinterpret_cast<Demand_t>(trampoline.allocate(stub));

		// The 6 bytes patched are exactly the two displaced instructions.
		trampoline.write_branch<6>(target.address(), HookedDemand);

		logger::info("installed BSModelDB::Demand model-path tracker"sv);
	}

	std::vector<Request> select_relevant(
		std::span<const Entry> a_newestFirst,
		std::int64_t a_nowTicks,
		std::int64_t a_ticksPerSecond)
	{
		const auto ticksPerSecond = std::max<std::int64_t>(a_ticksPerSecond, 1);

		std::vector<Request> out;
		out.reserve(a_newestFirst.size());
		for (const auto& entry : a_newestFirst) {
			const auto ageMs = std::max(0.0, static_cast<double>(a_nowTicks - entry.ticks) * 1000.0 / static_cast<double>(ticksPerSecond));
			if (entry.active || ageMs <= kRecentWindowMs)
				out.push_back({ entry.path, ageMs, entry.active });
		}
		return out;
	}

	std::int64_t now_ticks()
	{
		LARGE_INTEGER now{};
		::QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	Report get_report(std::int64_t a_faultTicks)
	{
		Report report;

		const auto threadId = ::GetCurrentThreadId();
		Slot* slot = nullptr;
		for (auto& candidate : g_slots) {
			if (candidate.ownerThreadId.load(std::memory_order_relaxed) == threadId) {
				slot = &candidate;
				break;
			}
		}
		if (!slot)
			return report;

		try {
			const auto count = std::min<std::uint32_t>(slot->head, static_cast<std::uint32_t>(Slot::kCapacity));
			std::vector<Entry> entries;
			entries.reserve(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto idx = (slot->head - 1 - i) % Slot::kCapacity;
				entries.push_back({ slot->paths[idx], slot->ticks[idx], slot->active[idx] });
				report.inFlight = report.inFlight || slot->active[idx];
			}

			report.requests = select_relevant(entries, a_faultTicks, g_ticksPerSecond);
		} catch (...) {
			// Allocation failure must not propagate out of the crash handler.
		}

		return report;
	}
}
