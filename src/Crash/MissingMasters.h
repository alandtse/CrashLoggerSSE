#pragma once

#include <string>
#include <vector>

namespace Crash::MissingMasters
{
	// Read-only crash-time diagnosis of the "enabled plugin has a master that is not in
	// Data" failure. In that case TESDataHandler::CompileFiles aborts before reading any
	// forms (leaving the compiled file collection empty), and main init later crashes on
	// the missing base form.
	//
	// Returns the report lines to print, or an empty vector when the data load did not
	// fail this way (so normal crashes are never affected).
	[[nodiscard]] std::vector<std::string> Diagnose();
}
