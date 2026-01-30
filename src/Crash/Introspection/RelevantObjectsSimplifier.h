#pragma once

#include <string>
#include <string_view>

namespace Crash::Introspection
{
	// Converts full introspection analysis output into a concise single-line summary
	// suitable for "Relevant Objects" section in crash logs.
	// 
	// Returns empty string if the analysis doesn't contain useful object info.
	// 
	// Example outputs:
	//   TESForm: "Iron Sword" [0x00012EB7] (Skyrim.esm)
	//   NativeFunctionBase: Actor.StartCombat()
	//   ObjectTypeInfo: MyCustomScript
	//   CodeTasklet: Stack: [(00012345)].MyScript.OnUpdate() - "MyScript.psc" Line 42
	[[nodiscard]] std::string simplify_for_relevant_objects(std::string_view full_analysis);
}
