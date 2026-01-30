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

	get_value(a_ini, logLevel, section, "Log Level", ";Log level of messages to buffer for printing: trace = 0, debug = 1, info = 2, warn = 3, err = 4, critical = 5, off = 6.");
	get_value(a_ini, flushLevel, section, "Flush Level", ";Log level to force messages to print from buffer.");
	get_value(a_ini, waitForDebugger, section, "Wait for Debugger for Crash", ";Enable if using VisualStudio to debug CrashLogger; Set false otherwise because Crashlogger will not produce a crash until the debugger is detected.");
	get_value(a_ini, autoOpenCrashLog, section, "Auto Open Crash Log", ";Automatically open the crash log with the default text viewer after a crash.");
	get_value(a_ini, symcache, section, "Symcache Directory", ";Local symbol cache directory.");
	std::string crashDirectoryComment = std::format("; Crashlog output directory. If blank, defaults to \"Documents\\my games\\{}\\SKSE\\\"", !REL::Module::IsVR() ? "Skyrim Special Edition" : "Skyrim VR");
	get_value(a_ini, crashDirectory, section, "Crashlog Directory", crashDirectoryComment.c_str());

	// Thread dump hotkey settings
	get_value(a_ini, enableThreadDumpHotkey, section, "Enable Thread Dump Hotkey", ";Enable thread dump hotkey for diagnosing hangs/deadlocks (0=disabled, no monitoring thread created).");

	// Parse hotkey combination (comma-separated list of VK codes)
	std::string hotkeyStr;
	get_value(a_ini, hotkeyStr, section, "Thread Dump Hotkey", ";Hotkey combination (VK codes): Ctrl=17, Shift=16, F12=123. Leave empty to disable monitoring thread.");
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
}

const Settings::Debug& Settings::GetDebug() const
{
	return debug;
}
