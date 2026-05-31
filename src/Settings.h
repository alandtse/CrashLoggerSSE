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
		bool autoUploadCrashLog{ false };
		std::string pastebinApiKey{ "" };
		std::string symcache{ "" };
		std::string crashDirectory{ "" };
		bool crashLogWriteMinidump{ false };
		int maxCrashLogs{ 20 };
		int maxMinidumps{ 1 };

		// Thread dump hotkey settings
		bool enableThreadDumpHotkey{ true };
		std::vector<int> threadDumpHotkey{ VK_CONTROL, VK_SHIFT, VK_F12 };
		bool threadDumpWriteMinidump{ false };

		// Heap analysis settings
		bool enableHeapAnalysis{ false };
		int maxHeapsToCheck{ 1 };
		int maxHeapIterationsPerHeap{ 1000 };

		// Thread context heuristics (label -> list of keywords)
		std::vector<std::pair<std::string, std::vector<std::string>>> threadContextHeuristics;

		// Developer crash testing features
		bool enableCrashTestHotkey{ false };
		std::vector<int> crashTestHotkey{ VK_CONTROL, VK_SHIFT, VK_F11 };
		int crashTestType{ 0 };  // 0=access violation, 1=null deref, 2=C++ exception, 3=stack overflow, 4=divide by zero, 5=pure virtual call
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
