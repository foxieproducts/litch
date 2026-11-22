#include "app_console.hpp"

#include "foxie_embedded/logger.hpp"

#include "app.hpp"

namespace {
bool ParseU32(const std::string_view s, uint32_t& out) {
    if (s.empty()) {
        return false;
    }

    uint32_t value = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10U + static_cast<uint32_t>(c - '0');
    }

    out = value;
    return true;
}
} // namespace

void RegisterAppConsoleCommands(App& app, fxe::Console& console) {
    console.RegisterCommand("ping", "Respond with pong", [&console](const fxe::Console::Args&) {
        console.PrintLn("pong");
        return true;
    });

    console.RegisterCommand("status", "Print app status", [&app, &console](const fxe::Console::Args&) {
        const RuntimeSettings& s = app.GetRuntimeSettings();
        console.PrintLn("brightness=%u diagnostics=%u buttonPressCount=%u", static_cast<unsigned>(s.brightness),
                        s.diagnosticsEnabled ? 1U : 0U, static_cast<unsigned>(s.buttonPressCount));
        return true;
    });

    console.RegisterCommand("brightness", "brightness <0..10>", [&app, &console](const fxe::Console::Args& args) {
        if (!args.Has(1)) {
            console.PrintLn("usage: brightness <0..10>");
            return false;
        }

        uint32_t v = 0;
        if (!ParseU32(args.Get(1), v) || v > 10U) {
            console.PrintLn("invalid brightness (expected 0..10)");
            return false;
        }

        RuntimeSettings updated = app.GetRuntimeSettings();
        updated.brightness = static_cast<uint8_t>(v);
        app.ApplyRuntimeSettings(updated);
        return true;
    });

    console.RegisterCommand("diag", "diag <0|1>", [&app, &console](const fxe::Console::Args& args) {
        if (!args.Has(1)) {
            console.PrintLn("usage: diag <0|1>");
            return false;
        }

        const std::string_view s = args.Get(1);
        if (!(s == "0" || s == "1")) {
            console.PrintLn("invalid diag (expected 0 or 1)");
            return false;
        }

        RuntimeSettings updated = app.GetRuntimeSettings();
        updated.diagnosticsEnabled = (s == "1");
        app.ApplyRuntimeSettings(updated);
        return true;
    });

    console.RegisterCommand("save", "Force settings save now", [&app, &console](const fxe::Console::Args&) {
        const bool ok = app.GetSettings().SaveNow();
        console.PrintLn(ok ? "settings: saved" : "settings: save failed");
        return ok;
    });

    fxe::Info("console: app commands registered");
}
