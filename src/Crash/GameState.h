#pragma once

#include <string>
#include <vector>

namespace Crash::GameState
{
	// Read-only crash-time snapshot of what the game was doing: main loop flags, open
	// menus and the player's location. Each line is independent, so a corrupt singleton
	// only drops its own line.
	[[nodiscard]] std::vector<std::string> Describe();
}
