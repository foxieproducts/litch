#include <string_view>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/logger.hpp"

#include "console_builtins_helpers.hpp"

namespace fxe {

namespace {

bool ParseLogLevel(const std::string_view s, fxe::Level& out) {
    if (s == "trace") {
        out = fxe::Level::TRACE;
        return true;
    }
    if (s == "debug") {
        out = fxe::Level::DEBUG;
        return true;
    }
    if (s == "info") {
        out = fxe::Level::INFO;
        return true;
    }
    if (s == "warn") {
        out = fxe::Level::WARN;
        return true;
    }
    if (s == "error") {
        out = fxe::Level::ERROR;
        return true;
    }

    return false;
}

} // namespace

void Console::InstallBuiltinDebug() {
    auto debugCmd = [this](const Args& args) -> bool {
        if (args.argc != 2) {
            PrintLn("Usage: debug <trace|debug|info|warn|error>");
            return false;
        }

        fxe::Level level;
        if (!ParseLogLevel(args.Get(1), level)) {
            PrintLn("Unknown level: %s", fxe::SafeStr(args.CStr(1)));
            return false;
        }

        fxe::SetMinLevel(level);
        PrintLn("Log level: %s", args.CStr(1));
        return true;
    };

    auto debugComplete = [](const Args&, const size_t argIndex, const char* prefix, const char** outMatches, const size_t outMatchesCap) -> size_t {
        if (argIndex != 1) {
            return 0;
        }
        size_t matchCount = 0;
        static constexpr const char* LEVELS[] = {"trace", "debug", "info", "warn", "error"};
        for (const char* s : LEVELS) {
            matchCount = fxe::AddUniqueMatchIfPrefix(s, prefix, outMatches, matchCount, outMatchesCap);
        }
        return matchCount;
    };

    RegisterCommand("debug", "Set log level: trace|debug|info|warn|error", debugCmd, debugComplete);
}

} // namespace fxe
