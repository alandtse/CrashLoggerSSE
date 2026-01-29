#pragma once

class Settings
{
public:
	class Debug
	{
	public:
		void Load(CSimpleIniA& a_ini);

		spdlog::level::level_enum logLevel{ spdlog::level::level_enum::info };
		spdlog::level::level_enum flushLevel{ spdlog::level::level_enum::trace };
		bool waitForDebugger{ false };
		bool autoOpenCrashLog{ true };
		std::string symcache{ "" };
		std::string crashDirectory{ "" };
	};

	[[nodiscard]] static Settings* GetSingleton();

	void Load();

	[[nodiscard]] const Debug& GetDebug() const;

private:
	template <class T>
	static void get_value(CSimpleIniA& a_ini, T& a_value, const char* a_section, const char* a_key, const char* a_comment)
	{
		ini::get_value(a_ini, a_value, a_section, a_key, a_comment);
	}
	Debug debug{};
};
