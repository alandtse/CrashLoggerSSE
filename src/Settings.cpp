#include "Settings.h"

Settings* Settings::GetSingleton()
{
	static Settings singleton;
	return std::addressof(singleton);
}

void Settings::Load()
{
	constexpr auto path = L"Data/SKSE/Plugins/CrashLogger.ini";

	CSimpleIniA ini;
	ini.SetUnicode();

	ini.LoadFile(path);

	//Debug
	debug.Load(ini);

	ini.SaveFile(path);
}

void Settings::Debug::Load(CSimpleIniA& a_ini)
{
	static const char* section = "Debug";

	// Write section headers as comments for better organization
	a_ini.SetValue(section, nullptr, nullptr,
		"; ============================================================================\n"
		"; Crash Log Settings\n"
		"; ============================================================================\n"
		"; These settings control how crash logs are generated and shared when the\n"
		"; game crashes unexpectedly.\n"
		"; ============================================================================",
		false);

	get_value(a_ini, symcache, section, "Symcache Directory", ";Local symbol cache directory for PDB symbols (speeds up crash log generation). Default: c:\\symcache");
	std::string crashDirectoryComment = std::format("; Crashlog output directory. If blank, defaults to \"Documents\\my games\\{}\\SKSE\\\"", !REL::Module::IsVR() ? "Skyrim Special Edition" : "Skyrim VR");
	get_value(a_ini, crashDirectory, section, "Crashlog Directory", crashDirectoryComment.c_str());
	get_value(a_ini, maxCrashLogs, section, "Max Crash Logs", ";Maximum number of crash logs to keep. Oldest logs will be deleted when this limit is exceeded. Default: 20\n;Set to 0 to disable log cleanup (keep all logs).");
	get_value(a_ini, maxMinidumps, section, "Max Minidumps", ";Maximum number of minidump files to keep. Minidumps are very large, so a low limit is recommended. Default: 1\n;Set to 0 to disable minidump cleanup.");
	get_value(a_ini, autoOpenCrashLog, section, "Auto Open Crash Log", ";Automatically open the crash log with the default text viewer after a crash. Default: true");
	get_value(a_ini, autoUploadCrashLog, section, "Auto Upload Crash Log", ";Automatically upload crash log to pastebin.com and open URL in browser. Requires Pastebin API Key. Great for sharing crash logs with mod authors! Default: false");
	get_value(a_ini, pastebinApiKey, section, "Pastebin API Key", ";Get your free API key from https://pastebin.com/doc_api#1 (required for auto upload).\n;1. Create free account at pastebin.com  2. Get API key from link  3. Paste it here. Default: empty");

	// Thread dump section header
	a_ini.SetValue(section, nullptr, nullptr,
		"\n; ============================================================================\n"
		"; Thread Dump Settings (Hang/Deadlock Diagnosis)\n"
		"; ============================================================================\n"
		"; Thread dumps help diagnose game HANGS/FREEZES (not crashes). When the game\n"
		"; freezes, press the hotkey to generate a thread dump showing what all threads\n"
		"; are doing, which helps identify deadlocks or infinite loops.\n"
		"; ============================================================================",
		false);

	get_value(a_ini, enableThreadDumpHotkey, section, "Enable Thread Dump Hotkey", ";Enable thread dump hotkey for diagnosing hangs/deadlocks. Default: true\n;When enabled, press Ctrl+Shift+F12 while game is frozen to generate dump.\n;Set to 0 to disable (no monitoring thread will be created).");

	// Parse hotkey combination (comma-separated list of VK codes)
	std::string hotkeyStr = "17, 16, 123";  // Default: Ctrl+Shift+F12
	get_value(a_ini, hotkeyStr, section, "Thread Dump Hotkey", ";Hotkey combination (VK codes): Ctrl=17, Shift=16, F12=123. Default: 17, 16, 123\n;VK code reference: https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes");

	// Heap analysis section header
	a_ini.SetValue(section, nullptr, nullptr,
		"\n; ============================================================================\n"
		"; Heap Analysis Settings (Memory Allocation Detection)\n"
		"; ============================================================================\n"
		"; WARNING: Heap analysis can be VERY SLOW (10+ seconds) in large modded games.\n"
		"; It attempts to identify if crash-related pointers are heap allocations by\n"
		"; walking through all memory allocations. Disabled by default due to performance.\n"
		"; Only enable if specifically needed for debugging memory-related crashes.\n"
		"; ============================================================================",
		false);

	get_value(a_ini, enableHeapAnalysis, section, "Enable Heap Analysis", ";Enable heap allocation analysis for crash pointers. Default: false\n;WARNING: Can cause 10+ second delays in crash log generation. Only enable if needed.");
	get_value(a_ini, maxHeapsToCheck, section, "Max Heaps To Check", ";Maximum number of heaps to check (process has many heaps). Default: 1\n;1 = only check process heap (fastest, limited coverage)\n;2-5 = check multiple heaps (slower, better coverage)\n;0 = check all heaps (VERY SLOW, not recommended)");
	get_value(a_ini, maxHeapIterationsPerHeap, section, "Max Heap Iterations Per Heap", ";Maximum allocations to check per heap before giving up. Default: 1000\n;Lower = faster but may miss allocations. Higher = slower but more thorough.\n;0 = unlimited (VERY SLOW, not recommended)");

	// Thread context heuristics
	// Parse all keys starting with "Thread Context "
	threadContextHeuristics.clear();
	CSimpleIniA::TNamesDepend keys;
	a_ini.GetAllKeys(section, keys);

	// If no heuristics configured, add defaults
	bool hasHeuristics = false;
	for (const auto& key : keys) {
		if (std::string_view(key.pItem).starts_with("Thread Context ")) {
			hasHeuristics = true;
			break;
		}
	}

	if (!hasHeuristics) {
		a_ini.SetValue(section, "Thread Context Papyrus VM", "BSScript, Papyrus, VirtualMachine", "; Thread context detection: each line defines label and its trigger keywords (comma-separated)", false);
		a_ini.SetValue(section, "Thread Context Havok/Physics", "hkp, Havok, bhk, hkb", nullptr, false);
		a_ini.SetValue(section, "Thread Context Rendering", "Render, BSRender, BSShader, NiCamera", nullptr, false);
		a_ini.SetValue(section, "Thread Context Audio", "Audio, XAudio, BSAudio, SoundHandle", nullptr, false);
		a_ini.SetValue(section, "Thread Context Job/Task", "Job, Task, JobList, ServingThread", nullptr, false);

		// Refresh keys
		a_ini.GetAllKeys(section, keys);
	}

	for (const auto& key : keys) {
		std::string_view keyStr(key.pItem);
		if (keyStr.starts_with("Thread Context ")) {
			std::string label(keyStr.substr(15));  // Skip "Thread Context "
			std::string value = a_ini.GetValue(section, key.pItem, "");

			// Parse comma-separated keywords
			std::vector<std::string> keywords;
			std::istringstream iss(value);
			std::string token;
			while (std::getline(iss, token, ',')) {
				// Trim whitespace
				token.erase(0, token.find_first_not_of(" \t"));
				token.erase(token.find_last_not_of(" \t") + 1);
				if (!token.empty()) {
					keywords.push_back(token);
				}
			}

			if (!keywords.empty()) {
				threadContextHeuristics.emplace_back(std::move(label), std::move(keywords));
			}
		}
	}

	// Developer crash testing section header
	a_ini.SetValue(section, nullptr, nullptr,
		"\n; ============================================================================\n"
		"; Developer Crash Testing (FOR TESTING ONLY - KEEP DISABLED!)\n"
		"; ============================================================================\n"
		"; WARNING: These features intentionally CRASH the game for testing!\n"
		"; Only enable these if you're testing CrashLogger functionality.\n"
		"; DO NOT enable these during normal gameplay!\n"
		"; ============================================================================",
		false);

	get_value(a_ini, enableCrashTestHotkey, section, "Enable Crash Test Hotkey", ";Enable developer crash testing hotkey. Default: false\n;WARNING: This will display a prominent warning on screen and intentionally crash when pressed!\n;Only enable for testing CrashLogger. DO NOT enable during normal gameplay!\n;Set to false or 0 to disable completely.");

	// Parse crash test hotkey combination
	std::string crashTestHotkeyStr = "17, 16, 122";  // Default: Ctrl+Shift+F11
	get_value(a_ini, crashTestHotkeyStr, section, "Crash Test Hotkey", ";Crash test hotkey combination (VK codes): Ctrl=17, Shift=16, F11=122. Default: 17, 16, 122\n;Press this combination to trigger a test crash (only if enabled above).\n;Use Ctrl+Shift+PgUp/PgDn to cycle between crash types in-game!");

	get_value(a_ini, crashTestType, section, "Crash Test Type", ";Initial crash type on game start (0-9). Can be changed in-game with Ctrl+Shift+PgUp/PgDn. Default: 0\n;General C++ Crashes:\n;  0 = Access Violation (invalid memory write)\n;  1 = Null Pointer Dereference (read from address 0)\n;  2 = C++ Exception (std::runtime_error with message)\n;  3 = Divide by Zero (integer division)\n;  4 = Invalid Virtual Call (corrupted object vtable)\n;Skyrim-Specific Crashes:\n;  5 = Invalid Form Access (NULL TESForm pointer)\n;  6 = Invalid 3D Access (NULL NiAVObject pointer)\n;  7 = Invalid ExtraData (NULL ExtraDataList pointer)\n;  8 = Corrupted Player Singleton (vtable corruption)\n;  9 = Wrong Offset Access (simulates version mismatch)\n;TIP: Don't edit this while testing - use PgUp/PgDn hotkeys instead!");

	// Advanced section header
	a_ini.SetValue(section, nullptr, nullptr,
		"\n; ============================================================================\n"
		"; Advanced Debugging (Most users don't need these)\n"
		"; ============================================================================",
		false);

	get_value(a_ini, crashLogWriteMinidump, section, "Crash Log Write Minidump", ";Also create minidump file (.dmp) for crash log WinDbg analysis. Default: false\n;WARNING: Minidumps are VERY LARGE (500MB-2GB+) and only useful for advanced debugging with WinDbg.\n;Only enable if a mod author specifically requests a minidump.");
	get_value(a_ini, threadDumpWriteMinidump, section, "Thread Dump Write Minidump", ";Also create minidump file (.dmp) for thread dump WinDbg analysis. Default: false\n;WARNING: Minidumps are VERY LARGE (500MB-2GB+) and only useful for advanced debugging with WinDbg.\n;Only enable if a mod author specifically requests a minidump.");
	get_value(a_ini, logLevel, section, "Log Level", ";Log level of messages to buffer for printing: trace = 0, debug = 1, info = 2, warn = 3, err = 4, critical = 5, off = 6. Default: 0");
	get_value(a_ini, flushLevel, section, "Flush Level", ";Log level to force messages to print from buffer. Default: 0");
	get_value(a_ini, waitForDebugger, section, "Wait for Debugger for Crash", ";Enable if using VisualStudio to debug CrashLogger itself. Default: false\n;Set false otherwise because Crashlogger will not produce a crash until the debugger is detected.");

	threadDumpHotkey.clear();
	if (!hotkeyStr.empty()) {
		std::istringstream iss(hotkeyStr);
		std::string token;
		while (std::getline(iss, token, ',')) {
			// Trim whitespace
			token.erase(0, token.find_first_not_of(" \t"));
			token.erase(token.find_last_not_of(" \t") + 1);
			if (!token.empty()) {
				try {
					threadDumpHotkey.push_back(std::stoi(token));
				} catch (...) {
					// Skip invalid values
				}
			}
		}
	}

	crashTestHotkey.clear();
	if (!crashTestHotkeyStr.empty()) {
		std::istringstream iss(crashTestHotkeyStr);
		std::string token;
		while (std::getline(iss, token, ',')) {
			// Trim whitespace
			token.erase(0, token.find_first_not_of(" \t"));
			token.erase(token.find_last_not_of(" \t") + 1);
			if (!token.empty()) {
				try {
					crashTestHotkey.push_back(std::stoi(token));
				} catch (...) {
					// Skip invalid values
				}
			}
		}
	}
}

const Settings::Debug& Settings::GetDebug() const
{
	return debug;
}
