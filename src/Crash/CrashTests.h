#pragma once

namespace Crash
{
	namespace CrashTests
	{
		// Trigger a developer crash test of the specified type
		// Types 0-4: General C++ crashes
		// Types 5-9: Skyrim-specific crashes
		void TriggerTestCrash(int crashType);

		// Get the number of available crash test types
		constexpr int GetCrashTestCount() { return 10; }

		// Get crash type names for display
		const char* GetCrashTypeName(int crashType);
	}
}
