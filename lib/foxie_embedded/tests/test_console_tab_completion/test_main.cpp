#include <cstdio>
#include <cstring>
#include <string>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/logger.hpp"
#include "foxie_embedded/console_transport_memory.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"

namespace {

size_t EnumKnownSettingKeys(const char* prefix,
                            const char** outMatches,
                            const size_t outMatchesCap) {
    if (!prefix || !outMatches || outMatchesCap == 0) {
        return 0;
    }

    static constexpr const char* KNOWN_KEYS[] = {
        "BRIGHTNESS",
        "SETTING_ALPHA",
        "MESSAGE",
        "TIMEOUT_MS",
        "SETTING_BETA",
    };

    const size_t prefixLen = std::strlen(prefix);
    size_t count = 0;
    for (const char* key : KNOWN_KEYS) {
        if (!key) {
            continue;
        }
        if (prefixLen == 0 || std::strncmp(key, prefix, prefixLen) == 0) {
            if (count < outMatchesCap) {
                outMatches[count] = key;
                ++count;
            }
        }
    }

    return count;
}

} // namespace

int main() {
    fxe::test_support::TestResults results("CONSOLE_TAB_COMPLETION");
    results.Start();

    fxe::SettingsStoreConfig cfg;
    cfg.path = "build/test_console_tab_completion.json";
    cfg.deferredSaveDelayMs = 100000;

    (void)std::remove(cfg.path);

    fxe::SettingsStore settings(cfg);
    results.Check(settings.Load(), "settings load");

    // Seed a custom key so we can verify it appears in completion.
    settings.Doc()["FOO"] = "bar";

    fxe::MemoryConsoleTransport transport;
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);

    fxe::Console console(transport);
    console.SetSettingsStore(&settings);
    console.SetSettingsKeyEnumCallback(&EnumKnownSettingKeys);

    bool ranAlpine = false;
    bool ranPaintFireworks = false;
    bool ranSoloAgain = false;
    (void)console.RegisterCommand("alpha", "Alpha telemetry", [](const fxe::Console::Args&) { return true; });
    (void)console.RegisterCommand("alpine",
                                  "Mountain telemetry",
                                  [&ranAlpine](const fxe::Console::Args&) {
                                      ranAlpine = true;
                                      return true;
                                  });
    (void)console.RegisterCommand(
        "paint",
        "paint [style]",
        [&ranPaintFireworks](const fxe::Console::Args& args) {
            ranPaintFireworks = args.Has(1) && args.Get(1) == std::string_view("fireworks");
            return ranPaintFireworks;
        },
        [](const fxe::Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) -> size_t {
            if (argIndex != 1 || !prefix || !outMatches || outMatchesCap == 0) {
                return 0;
            }
            static constexpr const char* VALUES[] = {
                "fire-lob",
                "fireworks",
            };
            const size_t prefixLen = std::strlen(prefix);
            size_t count = 0;
            for (const char* value : VALUES) {
                if (!value) {
                    continue;
                }
                if (prefixLen == 0 || std::strncmp(value, prefix, prefixLen) == 0) {
                    if (count < outMatchesCap) {
                        outMatches[count] = value;
                        ++count;
                    }
                }
            }
            return count;
        });
    (void)console.RegisterCommand(
        "solo",
        "solo run again",
        [&ranSoloAgain](const fxe::Console::Args& args) {
            ranSoloAgain = args.Has(1) && args.Get(1) == std::string_view("run") && args.Has(2) &&
                           args.Get(2) == std::string_view("again");
            return ranSoloAgain;
        },
        [](const fxe::Console::Args&,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) -> size_t {
            if (!prefix || !outMatches || outMatchesCap == 0) {
                return 0;
            }

            const char* candidate = nullptr;
            if (argIndex == 1) {
                candidate = "run";
            } else if (argIndex == 2) {
                candidate = "again";
            } else {
                return 0;
            }

            const size_t prefixLen = std::strlen(prefix);
            if (prefixLen != 0 && std::strncmp(candidate, prefix, prefixLen) != 0) {
                return 0;
            }

            outMatches[0] = candidate;
            return 1;
        });

    unsigned diagnosticRuns = 0U;
    unsigned completionRuns = 0U;
    (void)console.RegisterCommand(
        "diagnostic", "Hardware check",
        [&diagnosticRuns](const fxe::Console::Args&) {
            ++diagnosticRuns;
            return true;
        },
        [&completionRuns](const fxe::Console::Args&, size_t, const char*,
                          const char**, size_t) -> size_t {
            ++completionRuns;
            return 0U;
        },
        "Usage: diagnostic calibrate [samples] | diagnostic recover (keeps settings)");
    for (const char* query : {"help CALIBRATE", "help diagnostic samples", "help keeps settings",
                              "help diagnostic"}) {
        output.Clear();
        fxe::test_support::RunCommand(console, transport, query);
        results.Check(output.Str().find("diagnostic - Hardware check") != std::string::npos,
                      "help searches registered subcommands and every usage word");
        results.Check(output.Str().find("diagnostic recover") != std::string::npos,
                      "help displays the matching usage without executing it");
    }
    results.Check(diagnosticRuns == 0U && completionRuns == 0U,
                  "help does not execute command or completion handlers");
    (void)console.RegisterCommand("diagnostic", "Replacement help",
                                 [](const fxe::Console::Args&) { return true; });
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "help calibrate");
    results.Check(output.Str().find("No commands matching") != std::string::npos,
                  "replacing command metadata removes obsolete usage");

    // Exact help should still show the command details.
    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "help paint");
    fxe::test_support::PressEnter(console, transport);
    results.Check(output.Str().find("paint - paint [style]\r\n") != std::string::npos,
                  "help exact match shows command details");

    // Non-exact help should search command names.
    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "help alp");
    fxe::test_support::PressEnter(console, transport);
    results.Check(output.Str().find("alpha - Alpha telemetry\r\n") != std::string::npos,
                  "help search finds command name matches");
    results.Check(output.Str().find("alpine - Mountain telemetry\r\n") != std::string::npos,
                  "help search includes multiple command name matches");

    // Search should also match descriptive help text, not just command names.
    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "help telemetry");
    fxe::test_support::PressEnter(console, transport);
    results.Check(output.Str().find("alpha - Alpha telemetry\r\n") != std::string::npos,
                  "help search finds help text matches");
    results.Check(output.Str().find("alpine - Mountain telemetry\r\n") != std::string::npos,
                  "help search returns all help text matches");

    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "help mountain telemetry");
    fxe::test_support::PressEnter(console, transport);
    results.Check(output.Str().find("alpine - Mountain telemetry\r\n") != std::string::npos,
                  "help search supports multi-word idea queries");

    // Multiple command matches should complete to their common prefix.
    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "al");
    transport.PushChar('\t');
    console.Update();
    fxe::test_support::FeedConsole(console, transport, "ine");
    fxe::test_support::PressEnter(console, transport);
    results.Check(ranAlpine, "tab expands command to common prefix for multi-match");

    // Multiple argument matches should complete to their common prefix.
    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "paint fi");
    transport.PushChar('\t');
    console.Update();
    fxe::test_support::FeedConsole(console, transport, "works");
    fxe::test_support::PressEnter(console, transport);
    results.Check(ranPaintFireworks, "tab expands argument to common prefix for multi-match");

    // A unique match should append a trailing space, even when the current token
    // already exactly matches the only candidate.
    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "solo");
    transport.PushChar('\t');
    console.Update();
    fxe::test_support::FeedConsole(console, transport, "run");
    transport.PushChar('\t');
    console.Update();
    fxe::test_support::FeedConsole(console, transport, "again");
    fxe::test_support::PressEnter(console, transport);
    results.Check(ranSoloAgain, "tab appends spaces for exact unique command and argument matches");

    // Tab-complete settings subcommands.
    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "settings ");
    transport.PushChar('\t');
    console.Update();

    results.Check(output.Str().find("status\r\n") != std::string::npos,
                  "settings completion includes status");
    results.Check(output.Str().find("set\r\n") != std::string::npos,
                  "settings completion includes set");
    results.Check(output.Str().find("format\r\n") != std::string::npos,
                  "settings completion includes format");

    // Clear line.
    fxe::test_support::PressEnter(console, transport);

    // Tab-complete set keys (includes known keys + doc keys).
    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "set ");
    transport.PushChar('\t');
    console.Update();

    results.Check(output.Str().find("FOO\r\n") != std::string::npos, "set completion includes doc key");
    results.Check(output.Str().find("BRIGHTNESS\r\n") != std::string::npos,
                  "set completion includes canonical uppercase known key");

    // Clear line.
    fxe::test_support::PressEnter(console, transport);

    // Ensure top-level set preserves spaces.
    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "set MESSAGE hello world");
    fxe::test_support::PressEnter(console, transport);

    const char* message = settings.ReadValue("MESSAGE", "");
    results.Check(std::strcmp(message, "hello world") == 0, "set preserves spaces");

    fxe::SetMinLevel(fxe::Level::INFO);
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "debug trace extra");
    results.Check(fxe::GetMinLevel() == fxe::Level::INFO &&
                      output.Str().find("Usage: debug <trace|debug|info|warn|error>") != std::string::npos,
                  "extra debug arguments cannot change the log level");
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "debug trace");
    results.Check(fxe::GetMinLevel() == fxe::Level::TRACE &&
                      output.Str().find("Log level: trace") != std::string::npos,
                  "debug reply identifies the selected level");
    fxe::test_support::RunCommand(console, transport, "debug info");

    return results.Finish();
}
