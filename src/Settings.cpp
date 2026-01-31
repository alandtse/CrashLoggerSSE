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

	get_value(a_ini, logLevel, section, "Log Level", ";Log level of messages to buffer for printing: trace = 0, debug = 1, info = 2, warn = 3, err = 4, critical = 5, off = 6. Default: 0");
	get_value(a_ini, flushLevel, section, "Flush Level", ";Log level to force messages to print from buffer. Default: 0");
	get_value(a_ini, waitForDebugger, section, "Wait for Debugger for Crash", ";Enable if using VisualStudio to debug CrashLogger itself. Set false otherwise because Crashlogger will not produce a crash until the debugger is detected. Default: false");
	get_value(a_ini, autoOpenCrashLog, section, "Auto Open Crash Log", ";Automatically open the crash log with the default text viewer after a crash. Default: true");
	get_value(a_ini, autoUploadCrashLog, section, "Auto Upload Crash Log", ";Automatically upload crash log to pastebin.com and open URL in browser. Requires Pastebin API Key. Great for sharing crash logs with mod authors! Default: false");
	get_value(a_ini, pastebinApiKey, section, "Pastebin API Key", ";Get your free API key from https://pastebin.com/doc_api#1 (required for auto upload). 1. Create free account at pastebin.com  2. Get API key from link  3. Paste it here. Default: empty");
	get_value(a_ini, symcache, section, "Symcache Directory", ";Local symbol cache directory for PDB symbols (speeds up crash log generation). Default: c:\\symcache");
	std::string crashDirectoryComment = std::format("; Crashlog output directory. If blank, defaults to \"Documents\\my games\\{}\\SKSE\\\"", !REL::Module::IsVR() ? "Skyrim Special Edition" : "Skyrim VR");
	get_value(a_ini, crashDirectory, section, "Crashlog Directory", crashDirectoryComment.c_str());
	get_value(a_ini, crashLogWriteMinidump, section, "Crash Log Write Minidump", ";WARNING: Minidumps are VERY LARGE (500MB-2GB+) and only useful for advanced debugging with WinDbg. Only enable if a mod author specifically requests a minidump. Default: false");

	// Thread dump hotkey settings
	get_value(a_ini, enableThreadDumpHotkey, section, "Enable Thread Dump Hotkey", ";Enable thread dump hotkey for diagnosing hangs/deadlocks. When enabled, press Ctrl+Shift+F12 while game is frozen to generate dump. Set to 0 to disable (no monitoring thread will be created). Default: true");

	// Parse hotkey combination (comma-separated list of VK codes)
	std::string hotkeyStr;
	get_value(a_ini, hotkeyStr, section, "Thread Dump Hotkey", ";Hotkey combination (VK codes): Ctrl=17, Shift=16, F12=123. Leave empty to disable monitoring thread. Default: 17, 16, 123");
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

	get_value(a_ini, threadDumpWriteMinidump, section, "Thread Dump Write Minidump", ";WARNING: Minidumps are VERY LARGE (500MB-2GB+) and only useful for advanced debugging with WinDbg. Only enable if a mod author specifically requests a minidump. Default: false");
}

const Settings::Debug& Settings::GetDebug() const
{
	return debug;
}
