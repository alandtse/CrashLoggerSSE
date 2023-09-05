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
	get_value(a_ini, symcache, section, "Symcache Directory", ";Local symbol cache directory.");
}

const Settings::Debug& Settings::GetDebug() const
{
	return debug;
}
