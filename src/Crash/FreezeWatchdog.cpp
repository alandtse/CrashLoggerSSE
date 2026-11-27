#include "Crash/FreezeWatchdog.h"

#include "Crash/ThreadDump.h"
#include "Settings.h"

namespace Crash::FreezeWatchdog
{
	namespace
	{
		constexpr std::uint64_t POLL_MS = 1000;
		constexpr std::uint64_t STALL_START_MS = 5000;
		constexpr std::uint64_t MIN_SAMPLE_MS = 3000;
		// A healthy game already burns CPU on render and worker threads, so "busy" is measured against what
		// this session normally uses rather than against zero.
		constexpr std::uint64_t BUSY_MARGIN_MS_PER_S = 500;
		constexpr int MAX_DUMPS_PER_SESSION = 3;

		std::atomic<std::uint64_t> g_lastBeat{ 0 };
		std::atomic<bool> g_outstanding{ false };
		std::atomic<DWORD> g_mainThread{ 0 };
		std::atomic<bool> g_saveLoading{ false };
		std::atomic<bool> g_loadingMenu{ false };
		std::atomic<bool> g_started{ false };

		std::uint64_t to_ms(const FILETIME& a_ft) noexcept
		{
			ULARGE_INTEGER v{};
			v.LowPart = a_ft.dwLowDateTime;
			v.HighPart = a_ft.dwHighDateTime;
			return v.QuadPart / 10'000;
		}

		// Excludes time the PC spent asleep, which would otherwise look like a freeze.
		std::uint64_t awake_ms() noexcept
		{
			ULONGLONG ticks = 0;  // 100 ns units
			::QueryUnbiasedInterruptTime(&ticks);
			return ticks / 10'000;
		}

		std::uint64_t process_cpu_ms() noexcept
		{
			FILETIME c{}, e{}, k{}, u{};
			if (!::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u)) {
				return 0;
			}
			return to_ms(k) + to_ms(u);
		}

		std::uint64_t thread_cpu_ms(DWORD a_id) noexcept
		{
			const auto h = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, a_id);
			if (!h) {
				return 0;
			}
			FILETIME c{}, e{}, k{}, u{};
			const auto ok = ::GetThreadTimes(h, &c, &e, &k, &u);
			::CloseHandle(h);
			return ok ? to_ms(k) + to_ms(u) : 0;
		}

		bool game_is_foreground() noexcept
		{
			const auto fg = ::GetForegroundWindow();
			if (!fg) {
				return false;
			}
			DWORD pid = 0;
			::GetWindowThreadProcessId(fg, &pid);
			return pid == ::GetCurrentProcessId();
		}

		void post_beat()
		{
			g_outstanding = true;
			SKSE::GetTaskInterface()->AddTask([]() {
				if (const auto id = ::GetCurrentThreadId(); g_mainThread.exchange(id) != id) {
					logger::info("Freeze watchdog: queued tasks run on thread {}"sv, id);
				}
				const auto ui = RE::UI::GetSingleton();
				g_loadingMenu = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
				g_lastBeat = awake_ms();
				g_outstanding = false;
			});
		}

		enum class Answer
		{
			Dump,
			Ignore,
			Recovered,
		};

		BOOL CALLBACK close_box_cb(HWND a_wnd, LPARAM) noexcept
		{
			::PostMessageW(a_wnd, WM_COMMAND, IDNO, 0);
			return TRUE;
		}

		struct Prompt
		{
			std::wstring text;
			std::atomic<int> result{ 0 };
			std::atomic<bool> finished{ false };
			std::atomic<DWORD> thread{ 0 };
		};

		constexpr int CLOSE_ATTEMPTS = 100;  // 50 ms apart

		// Asks the user from a helper thread, since the game's own UI is not running. Closes the box if the
		// main thread beats again after a_beatAtStall, and treats an expired countdown (a_timeoutS > 0) as "dump".
		Answer ask_user(std::uint64_t a_beatAtStall, std::uint64_t a_stallS, int a_timeoutS)
		{
			const auto prompt = std::make_shared<Prompt>();
			prompt->text = std::format(
				L"Skyrim has not responded for {} seconds.\n\nWrite a thread dump to help diagnose the freeze?\n\n"
				L"Yes: write the dump\nNo: ignore this freeze{}",
				a_stallS,
				a_timeoutS > 0 ? std::format(L"\n\nA dump is written automatically in {} seconds if you do not answer.", a_timeoutS) : L"");
			std::thread([prompt]() {
				prompt->thread = ::GetCurrentThreadId();
				prompt->result = ::MessageBoxW(nullptr, prompt->text.c_str(), L"Crash Logger: game frozen?",
					MB_YESNO | MB_ICONWARNING | MB_SYSTEMMODAL | MB_TOPMOST | MB_SETFOREGROUND);
				prompt->finished = true;
			}).detach();

			const auto recovered = [&]() { return g_lastBeat.load() != a_beatAtStall; };
			const auto start = awake_ms();
			for (;;) {
				::Sleep(250);
				const bool expired = a_timeoutS > 0 && awake_ms() - start >= static_cast<std::uint64_t>(a_timeoutS) * 1000;
				if (recovered() || expired) {
					// The box may not exist yet, so keep asking it to close for a while.
					for (int i = 0; i < CLOSE_ATTEMPTS && !prompt->finished; ++i) {
						::EnumThreadWindows(prompt->thread, close_box_cb, 0);
						::Sleep(50);
					}
					if (!prompt->finished) {
						logger::warn("The freeze prompt did not close"sv);
					}
					return recovered() ? Answer::Recovered : Answer::Dump;
				}
				if (prompt->finished) {
					if (prompt->result == 0) {
						logger::warn("Could not show the freeze prompt; writing the dump"sv);
						return Answer::Dump;
					}
					return prompt->result == IDYES ? Answer::Dump : Answer::Ignore;
				}
			}
		}

		void run()
		{
			const auto& cfg = Settings::GetSingleton()->GetDebug();
			const std::uint64_t idleMs = static_cast<std::uint64_t>(std::max(cfg.freezeThresholdSeconds, 5)) * 1000;
			const std::uint64_t busyMs = std::max<std::uint64_t>(static_cast<std::uint64_t>(std::max(cfg.freezeBusyThresholdSeconds, 0)) * 1000, idleMs);

			int promptS = cfg.freezePromptSeconds;
			if (promptS < -1) {
				logger::warn("Freeze Prompt Seconds {} is below -1; using 0"sv, promptS);
				promptS = 0;
			}

			bool stalling = false;
			bool dumped = false;
			bool logged = false;
			int dumps = 0;
			std::uint64_t t0 = 0, proc0 = 0, main0 = 0;
			std::uint64_t baseline = 0, lastProc = process_cpu_ms(), lastTick = awake_ms();

			for (;;) {
				::Sleep(static_cast<DWORD>(POLL_MS));
				if (!g_outstanding) {
					post_beat();
				}

				const auto beat = g_lastBeat.load();
				const auto now = awake_ms();
				const auto stall = now - beat;
				if (stall < STALL_START_MS) {
					stalling = false;
					dumped = false;
					logged = false;
					const auto proc = process_cpu_ms();
					if (now > lastTick) {
						const auto rate = (proc - lastProc) * 1000 / (now - lastTick);
						baseline = baseline ? (baseline * 9 + rate) / 10 : rate;
					}
					lastProc = proc;
					lastTick = now;
					continue;
				}
				if (!stalling) {
					stalling = true;
					t0 = now;
					proc0 = process_cpu_ms();
					main0 = thread_cpu_ms(g_mainThread);
					continue;
				}
				const auto sample = now - t0;
				if (dumped || dumps >= MAX_DUMPS_PER_SESSION || sample < MIN_SAMPLE_MS) {
					continue;
				}

				const auto procMsPerS = (process_cpu_ms() - proc0) * 1000 / sample;
				const bool busy = procMsPerS > baseline * 2 + BUSY_MARGIN_MS_PER_S;
				const bool loading = g_saveLoading || g_loadingMenu;
				const auto threshold = (busy || loading) ? busyMs : idleMs;
				if (!logged) {
					logged = true;
					logger::info("Main thread stalled {} s: process CPU {} ms/s (baseline {}), {}, loading {}, foreground {}, threshold {} s"sv,
						stall / 1000, procMsPerS, baseline, busy ? "busy" : "idle", loading, game_is_foreground(), threshold / 1000);
				}
				if (stall < threshold || !game_is_foreground()) {
					continue;
				}

				const auto mainNow = thread_cpu_ms(g_mainThread);
				const auto mainCpu = main0 && mainNow >= main0 ? std::format("{} ms", mainNow - main0) : std::string("unavailable");
				const char* how = "automatic";
				if (promptS != 0) {
					logger::warn("Main thread unresponsive for {} s; asking whether to write a freeze dump"sv, stall / 1000);
					const auto answer = ask_user(beat, stall / 1000, promptS);
					if (answer == Answer::Recovered) {
						continue;
					}
					dumped = true;
					if (answer == Answer::Ignore) {
						logger::info("Freeze dump declined by the user"sv);
						continue;
					}
					how = promptS > 0 ? "confirmed by the user or countdown expired" : "confirmed by the user";
				}
				const auto context = std::format(
					"FREEZE DUMP ({}): the main thread (id {}) has not run a queued task for {} s\n"
					"Game window in foreground: yes\n"
					"Loading: save load {}, loading menu {}\n"
					"Process CPU while stalled: {} ms/s against a healthy baseline of {} ms/s ({})\n"
					"Main thread CPU while stalled: {} over {} s\n"
					"Threshold applied: {} s ({})",
					how, g_mainThread.load(), stall / 1000,
					g_saveLoading.load() ? "in progress" : "no", g_loadingMenu.load() ? "open" : "closed",
					procMsPerS, baseline, busy ? "busy: compiling, loading or spinning" : "idle: likely a deadlock",
					mainCpu, sample / 1000,
					threshold / 1000, (busy || loading) ? "busy/loading" : "idle");

				dumped = true;
				logger::warn("Main thread unresponsive for {} s; writing a freeze dump"sv, stall / 1000);
				if (WriteAllThreadsDump("Automatic Freeze Detection", context, false)) {
					++dumps;
				}
			}
		}
	}

	void Start()
	{
		const auto& cfg = Settings::GetSingleton()->GetDebug();
		if (!cfg.enableFreezeDetection || g_started.exchange(true)) {
			return;
		}
		g_lastBeat = awake_ms();
		try {
			std::thread([]() {
				try {
					run();
				} catch (...) {
					logger::error("Freeze watchdog stopped after an exception"sv);
				}
			}).detach();
		} catch (const std::system_error& e) {
			logger::error("Could not start the freeze watchdog: {}"sv, e.what());
			g_started = false;
			return;
		}
		logger::info("Freeze detection enabled (idle {} s, busy {} s)"sv, cfg.freezeThresholdSeconds, cfg.freezeBusyThresholdSeconds);
	}

	void SetLoading(bool a_loading) noexcept
	{
		g_saveLoading = a_loading;
	}
}
