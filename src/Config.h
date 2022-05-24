#pragma once

#include <SKSE/SKSE.h>
#include <articuno/articuno.h>

namespace CrashLogger {
    class Debug {
    public:
        [[nodiscard]] inline spdlog::level::level_enum GetLogLevel() const noexcept { return _logLevel; }

        [[nodiscard]] inline spdlog::level::level_enum GetFlushLevel() const noexcept { return _flushLevel; }

		[[nodiscard]] inline bool GetWaitForDebugger() const noexcept { return _waitForDebugger; }

		[[nodiscard]] inline std::string GetSymcache() const noexcept { return _symcache; }


    private:
        articuno_serialize(ar) {
            auto logLevel = spdlog::level::to_string_view(_logLevel);
            auto flushLevel = spdlog::level::to_string_view(_flushLevel);
			auto waitForDebugger = _waitForDebugger;
			auto symcache = _symcache;
			ar <=> articuno::kv(logLevel, "logLevel");
            ar <=> articuno::kv(flushLevel, "flushLevel");
			ar <=> articuno::kv(waitForDebugger, "waitForDebugger");
			ar <=> articuno::kv(symcache, "symcache");
        }

        articuno_deserialize(ar) {
            *this = Debug();
            std::string logLevel;
			std::string flushLevel;
			bool waitForDebugger = _waitForDebugger;
			std::string symcache= _symcache;
			if (ar <=> articuno::kv(logLevel, "logLevel")) {
                _logLevel = spdlog::level::from_str(logLevel);
            }
            if (ar <=> articuno::kv(flushLevel, "flushLevel")) {
                _flushLevel = spdlog::level::from_str(flushLevel);
            }
			if (ar <=> articuno::kv(waitForDebugger, "waitForDebugger")) {
				_waitForDebugger = waitForDebugger;
			}
			if (ar <=> articuno::kv(symcache, "symcache")) {
				_symcache = symcache;
			}

		}

        spdlog::level::level_enum _logLevel{spdlog::level::level_enum::info};
        spdlog::level::level_enum _flushLevel{spdlog::level::level_enum::trace};
		bool _waitForDebugger{ false };
		std::string _symcache{ "" };
        friend class articuno::access;
    };

    class Config {
    public:
        [[nodiscard]] inline const Debug& GetDebug() const noexcept { return _debug; }

        [[nodiscard]] static const Config& GetSingleton() noexcept;

    private:
        articuno_serde(ar) { ar <=> articuno::kv(_debug, "debug"); }

        Debug _debug;

        friend class articuno::access;
    };
}  // namespace CrashLogger
