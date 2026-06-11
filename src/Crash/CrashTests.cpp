#include "Crash/CrashTests.h"

#include <stdexcept>

namespace Crash
{
	namespace CrashTests
	{
		namespace
		{
			// ============================================================================
			// General C++ Crash Helpers
			// ============================================================================

			// Helper to cause divide by zero
			__declspec(noinline) int CauseDivideByZero()
			{
				volatile int divisor = 0;
				volatile int dividend = 42;
				return dividend / divisor;  // Integer divide by zero
			}

			// Helper class for testing virtual function call on corrupted object
			class VirtualFunctionTester
			{
			public:
				virtual ~VirtualFunctionTester() = default;
				virtual void DoSomething()
				{
					// This will never execute in our test
				}
			};

			// ============================================================================
			// Skyrim-Specific Crash Helpers
			// ============================================================================

			// Helper to cause invalid form access (NULL form pointer)
			__declspec(noinline) void CauseInvalidFormAccess()
			{
				// Simulate accessing a NULL TESForm pointer - very common crash
				RE::TESForm* form = nullptr;
				volatile auto formID = form->GetFormID();  // Crash: null pointer dereference
				(void)formID;
			}

			// Helper to cause invalid 3D access (NULL NiAVObject)
			__declspec(noinline) void CauseInvalid3DAccess()
			{
				// Simulate accessing NULL 3D object - common when actors are loaded but 3D isn't ready
				RE::NiAVObject* node = nullptr;
				volatile auto pos = node->world.translate;  // Crash: null pointer dereference
				(void)pos;
			}

			// Helper to cause invalid ExtraDataList access
			__declspec(noinline) void CauseInvalidExtraDataAccess()
			{
				// Simulate accessing NULL ExtraDataList
				RE::ExtraDataList* extraList = nullptr;
				volatile bool hasData = extraList->HasType(RE::ExtraDataType::kCount);  // Crash
				(void)hasData;
			}

			// Helper to corrupt PlayerCharacter singleton
			__declspec(noinline) void CauseCorruptedPlayerSingleton()
			{
				// Get the real PlayerCharacter singleton
				auto player = RE::PlayerCharacter::GetSingleton();
				if (!player) {
					// If we can't get player, just cause a null dereference anyway
					RE::PlayerCharacter* nullPlayer = nullptr;
					volatile auto formID = nullPlayer->GetFormID();
					(void)formID;
					return;
				}

				// Corrupt the vtable pointer (first 8 bytes of the object)
				// This simulates memory corruption that can happen with bad pointers
				auto corruptedVTable = reinterpret_cast<void***>(player);
				*corruptedVTable = reinterpret_cast<void**>(0xDEADBEEF);

				// Now try to call a virtual function - this will crash
				volatile auto formID = player->GetFormID();  // Crashes trying to use corrupted vtable
				(void)formID;
			}

			// Helper to call with wrong offset (simulate version mismatch)
			__declspec(noinline) void CauseWrongOffsetAccess()
			{
				// Get PlayerCharacter singleton
				auto player = RE::PlayerCharacter::GetSingleton();
				if (!player) {
					// If we can't get player, cause null dereference
					RE::PlayerCharacter* nullPlayer = nullptr;
					volatile auto formID = nullPlayer->GetFormID();
					(void)formID;
					return;
				}

				// Simulate accessing wrong offset (like using SE offsets in AE or vice versa)
				// We'll try to read a NiNode* from a bogus offset
				// PlayerCharacter is at least 0x800 bytes, so offset 0x1000 is definitely wrong
				auto bogusOffset = reinterpret_cast<uintptr_t>(player) + 0x1000;  // Way past actual object size
				auto fakeNodePtr = reinterpret_cast<RE::NiNode**>(bogusOffset);

				// Try to dereference it - will read garbage or unmapped memory
				volatile auto node = *fakeNodePtr;  // May crash here
				if (node) {
					// If it didn't crash yet, try to use the garbage pointer
					volatile auto pos = node->world.translate;  // Definitely crashes here
					(void)pos;
				}
			}
		}

		// Crash type names (no emojis for Skyrim compatibility)
		const char* GetCrashTypeName(int crashType)
		{
			static const char* crashTypeNames[] = {
				"[0] Access Violation (invalid write)",
				"[1] Null Pointer Dereference",
				"[2] C++ Exception (std::runtime_error)",
				"[3] Divide by Zero",
				"[4] Invalid Virtual Call (corrupted object)",
				"[5] Invalid Form Access (NULL TESForm)",
				"[6] Invalid 3D Access (NULL NiAVObject)",
				"[7] Invalid ExtraData (NULL ExtraDataList)",
				"[8] Corrupted Player Singleton (vtable corruption)",
				"[9] Wrong Offset Access (version mismatch)"
			};

			if (crashType >= 0 && crashType < GetCrashTestCount()) {
				return crashTypeNames[crashType];
			}
			return "[?] Unknown";
		}

		void TriggerTestCrash(int crashType)
		{
			logger::info("Developer crash test triggered: type {}", crashType);

			switch (crashType) {
			case 0:  // Access Violation (invalid write)
				{
					logger::info("Triggering Access Violation (write to invalid address)");
					volatile int* ptr = reinterpret_cast<volatile int*>(0xDEADBEEF);
					*ptr = 42;  // Write to invalid memory
					break;
				}

			case 1:  // Null Pointer Dereference
				{
					logger::info("Triggering Null Pointer Dereference");
					volatile int* ptr = nullptr;
					volatile int value = *ptr;  // Read from null pointer
					(void)value;
					break;
				}

			case 2:  // C++ Exception
				{
					logger::info("Triggering C++ Exception (std::runtime_error)");
					throw std::runtime_error("CrashLogger Test Exception: This is an intentional crash for testing!");
				}

			case 3:  // Divide by Zero
				{
					logger::info("Triggering Divide by Zero");
					volatile int result = CauseDivideByZero();
					(void)result;
					break;
				}

			case 4:  // Invalid Virtual Call (Corrupted Object)
				{
					logger::info("Triggering Invalid Virtual Call");
					// Create a pointer to an object with corrupted vtable
					// This simulates calling a virtual function on a corrupted object
					VirtualFunctionTester* ptr = reinterpret_cast<VirtualFunctionTester*>(0x1000);
					// This will crash trying to access the vtable
					ptr->DoSomething();
					break;
				}

			case 5:  // Invalid Form Access (Skyrim-specific)
				{
					logger::info("Triggering Invalid Form Access (NULL TESForm)");
					CauseInvalidFormAccess();
					break;
				}

			case 6:  // Invalid 3D Access (Skyrim-specific)
				{
					logger::info("Triggering Invalid 3D Access (NULL NiAVObject)");
					CauseInvalid3DAccess();
					break;
				}

			case 7:  // Invalid ExtraData Access (Skyrim-specific)
				{
					logger::info("Triggering Invalid ExtraData Access (NULL ExtraDataList)");
					CauseInvalidExtraDataAccess();
					break;
				}

			case 8:  // Corrupted Player Singleton
				{
					logger::info("Triggering Corrupted Player Singleton (vtable corruption)");
					logger::warn("WARNING: This will corrupt the player object! Game may be unstable if crash log completes.");
					CauseCorruptedPlayerSingleton();
					break;
				}

			case 9:  // Wrong Offset Access (version mismatch simulation)
				{
					logger::info("Triggering Wrong Offset Access (simulates version mismatch)");
					CauseWrongOffsetAccess();
					break;
				}

			default:
				logger::warn("Invalid crash test type: {}", crashType);
				break;
			}

			// Should never reach here
			logger::error("Crash test did not trigger a crash!");
		}
	}
}
