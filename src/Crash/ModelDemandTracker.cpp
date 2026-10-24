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
		};

		// Static storage, not thread_local: TLS in a LoadLibrary'd DLL is unsafe on raw worker threads.
		constexpr std::size_t kNumSlots = 64;
		Slot g_slots[kNumSlots];

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

		using Demand_t = RE::BSResource::ErrorCode (*)(const char*, RE::NiPointer<RE::NiNode>&, const RE::BSModelDB::DBTraits::ArgsType&);

		// Call-through target; write_branch's return value is only valid for call/jmp sites, not this prologue.
		Demand_t g_original = nullptr;

		RE::BSResource::ErrorCode HookedDemand(
			const char* a_modelPath,
			RE::NiPointer<RE::NiNode>& a_modelOut,
			const RE::BSModelDB::DBTraits::ArgsType& a_args)
		{
			if (a_modelPath) {
				if (auto* slot = FindOrClaimSlot(::GetCurrentThreadId())) {
					auto& path = slot->paths[slot->head % Slot::kCapacity];
					std::strncpy(path, a_modelPath, Slot::kPathSize - 1);
					path[Slot::kPathSize - 1] = '\0';
					++slot->head;
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

	std::vector<std::string> get_recent_paths_newest_first()
	{
		std::vector<std::string> out;

		Slot* slot = nullptr;
		const auto threadId = ::GetCurrentThreadId();
		for (auto& candidate : g_slots) {
			if (candidate.ownerThreadId.load(std::memory_order_relaxed) == threadId) {
				slot = &candidate;
				break;
			}
		}
		if (!slot)
			return out;

		const auto count = std::min<std::uint32_t>(slot->head, static_cast<std::uint32_t>(Slot::kCapacity));
		out.reserve(count);
		for (std::uint32_t i = 0; i < count; ++i) {
			const auto idx = (slot->head - 1 - i) % Slot::kCapacity;
			out.emplace_back(slot->paths[idx]);
		}
		return out;
	}
}
