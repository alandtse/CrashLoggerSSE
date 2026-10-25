#include "Crash/ModelPathLookup.h"

#include "Settings.h"

#include <map>

namespace Crash::ModelPathLookup
{
	namespace
	{
		using IdWords = std::array<std::uint32_t, 3>;

		constexpr std::size_t kCapacity = 8192;
		constexpr std::size_t kPathSize = 244;
		constexpr std::size_t kMaxScanQwords = 8192;
		constexpr std::uintptr_t kMinUserAddress = 0x10000;
		constexpr std::uintptr_t kMaxUserAddress = 0x7FFFFFFFFFFF;

		constexpr auto kMinVRAddressLibraryVersion = "0.287.0";

		struct Record
		{
			IdWords id{};
			char path[kPathSize]{};
		};

		// Static storage: the hook runs on every thread that builds a resource id.
		Record g_records[kCapacity];
		std::atomic<std::uint32_t> g_next{ 0 };

		using IdCtor_t = void* (*)(void*, const char*);
		IdCtor_t g_original = nullptr;

		void* HookedIdCtor(void* a_this, const char* a_path)
		{
			auto* result = g_original(a_this, a_path);

			if (a_path && *a_path) {
				const auto& id = *static_cast<const IdWords*>(a_this);
				if (id[0] || id[1] || id[2]) {
					auto& record = g_records[g_next.fetch_add(1, std::memory_order_relaxed) % kCapacity];
					record.id = {};
					std::strncpy(record.path, a_path, kPathSize - 1);
					record.path[kPathSize - 1] = char{};
					record.id = id;
				}
			}

			return result;
		}

		bool read_id(const void* a_address, IdWords& a_out)
		{
			__try {
				std::memcpy(a_out.data(), a_address, sizeof(IdWords));
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		struct PrologueTrampoline final : Xbyak::CodeGenerator
		{
			PrologueTrampoline(std::span<const std::uint8_t> a_displaced, std::uintptr_t a_resume)
			{
				for (const auto byte : a_displaced)
					db(byte);
				jmp(ptr[rip]);
				dq(a_resume);
			}
		};
	}

	void Install()
	{
		if (!Settings::GetSingleton()->GetDebug().enableModelPathTracking)
			return;

		// Resolving an id the VR address library lacks aborts the game, so older releases skip the hook.
		if (REL::Module::IsVR() && !REL::IDDB::get().IsVRAddressLibraryAtLeastVersion(kMinVRAddressLibraryVersion)) {
			logger::info("skipping BSResource::ID::ID path recorder: VR Address Library older than {}"sv, kMinVRAddressLibraryVersion);
			return;
		}

		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(68631, 69971) };

		static constexpr std::array<std::uint8_t, 6> kExpected{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };
		if (!std::equal(kExpected.begin(), kExpected.end(), reinterpret_cast<const std::uint8_t*>(target.address()))) {
			logger::warn("skipping BSResource::ID::ID path recorder: patch-site signature mismatch"sv);
			return;
		}

		auto& trampoline = SKSE::GetTrampoline();

		PrologueTrampoline stub(kExpected, target.address() + kExpected.size());
		stub.ready();
		g_original = reinterpret_cast<IdCtor_t>(trampoline.allocate(stub));

		trampoline.write_branch<6>(target.address(), HookedIdCtor);

		logger::info("installed BSResource::ID::ID path recorder"sv);
	}

	std::vector<Hit> FindPathsOnStack(std::span<const std::size_t> a_stack, std::size_t a_limit)
	{
		std::map<IdWords, const Record*> byId;
		for (const auto& record : g_records) {
			if (record.id[0] || record.id[1] || record.id[2])
				byId[record.id] = &record;
		}

		std::vector<Hit> hits;
		const auto remember = [&](const IdWords& a_id, std::size_t a_index) {
			const auto found = byId.find(a_id);
			if (found == byId.end() || hits.size() >= a_limit)
				return;

			std::string path{ found->second->path };
			const auto known = std::ranges::any_of(hits, [&](const Hit& a_hit) { return a_hit.path == path; });
			if (!known)
				hits.push_back({ std::move(path), a_index * sizeof(std::size_t) });
		};

		const auto count = std::min(a_stack.size(), kMaxScanQwords);
		for (std::size_t i = 0; i < count; ++i) {
			IdWords id;
			if (read_id(&a_stack[i], id))
				remember(id, i);

			const auto value = a_stack[i];
			if (value >= kMinUserAddress && value <= kMaxUserAddress && read_id(reinterpret_cast<const void*>(value), id))
				remember(id, i);
		}

		return hits;
	}
}
