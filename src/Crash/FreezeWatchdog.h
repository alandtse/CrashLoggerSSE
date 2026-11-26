#pragma once

namespace Crash::FreezeWatchdog
{
	// Starts a background thread that writes a thread dump when the main thread stops running queued tasks
	// while the game is in the foreground and not loading. Does nothing if disabled in the settings.
	void Start();

	// Tells the watchdog a save is loading, which legitimately stalls the main thread.
	void SetLoading(bool a_loading) noexcept;
}
