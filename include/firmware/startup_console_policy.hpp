#pragma once

#include <string_view>

#include "foxie_embedded/console.hpp"

namespace firmware {

inline std::string_view TrimStartupConsoleLine(std::string_view line) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    return line.substr(first, line.find_last_not_of(" \t\r\n") - first + 1U);
}

inline bool IsStartupConsoleLineAllowed(std::string_view line) {
    line = TrimStartupConsoleLine(line);
    return line.empty() || line == "support" || line == "perf" || line == "heap" ||
           line == "version" || line == "ver" || line == "logs" ||
           line == "safemode status" || line == "reboot nosave";
}

// Called only before service/worker initialization. The caller combines the
// brownout latch with SettingsStore's armed guard; after queue acceptance that
// guard is not a general statement of durability.
inline bool HandleStartupConsoleLine(fxe::Console& console,
                                     std::string_view line,
                                     const bool unsavedBrownoutProtection) {
    line = TrimStartupConsoleLine(line);
    if (line == "reboot nosave" && unsavedBrownoutProtection) {
        console.PrintLn(
            "startup: restart blocked to preserve unsaved power protection; diagnostics and LED retries remain available");
        return true;
    }
    if (IsStartupConsoleLineAllowed(line)) {
        return false;
    }
    console.PrintLn(
        "startup: waiting for LED output; available: support, perf, heap, version, logs, safemode status%s",
        unsavedBrownoutProtection ? "" : ", reboot nosave");
    return true;
}

} // namespace firmware
