#include <cstdio>
#include <string>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/console_transport_memory.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "owner_pairing_console_test_transport.hpp"
#include "modules/fox_console_commands.hpp"

namespace {
int failures = 0;
void Check(const bool accepted, const char* message) {
    if (!accepted) {
        std::printf("[FAIL] %s\n", message);
        ++failures;
    }
}
void CheckFoxCommand() {
    fxe::MemoryConsoleTransport transport;
    fxe::Console console(transport);
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);
    const bool registered = modules::RegisterFoxConsoleCommand(console);
    Check(registered, "fox registers in every profile");
    if (!registered) return;
    const auto run = [&](const char* command) {
        output.Clear();
        fxe::test_support::RunCommand(console, transport, command);
        return output.Str();
    };
    Check(run("help fox").find("fox [hello|fortune|quack]") != std::string::npos,
          "help discovers the single fox namespace");
    const auto greeting = run("fox");
    Check(greeting.find("Foxie says hello") != std::string::npos &&
              greeting.size() < 256U,
          "bare fox renders a tiny bounded greeting");
    Check(run("fox hello").find("Foxie says hello") != std::string::npos,
          "explicit hello retains the greeting");
    Check(run("fox quack").find("duck declines") != std::string::npos,
          "duck lives inside the same namespace");
    Check(run("fox unknown").find("Usage: fox") != std::string::npos &&
              run("fox fortune extra").find("Usage: fox") != std::string::npos,
          "invalid actions and excess arguments cannot advance the fortune");
    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "fox f\t");
    Check(output.Str().find("fortune") != std::string::npos &&
              output.Str().find("Foxie:") == std::string::npos,
          "terminal Tab edits the draft without executing a fortune");
    output.Clear();
    fxe::test_support::PressEnter(console, transport);
    Check(output.Str().find("Before you move") != std::string::npos,
          "only Enter executes the completed draft");
    Check(run("fox fortune").find("least useful piece") != std::string::npos,
          "fortunes advance on valid requests");
    for (unsigned i = 0U; i < 4U; ++i) (void)run("fox fortune");
    Check(run("fox fortune").find("Before you move") != std::string::npos,
          "the bounded fortune cursor wraps");
    fxe::MemoryConsoleTransport otherTransport;
    fxe::Console other(otherTransport);
    fxe::test_support::ConsoleOutputCapture otherOutput;
    otherOutput.Attach(otherTransport);
    Check(modules::RegisterFoxConsoleCommand(other), "another Console owns its fox");
    fxe::test_support::RunCommand(other, otherTransport, "fox fortune");
    Check(otherOutput.Str().find("Before you move") != std::string::npos,
          "fortune state belongs to the Console instead of shared global state");
}
void CheckEnteredLineAuthority() {
    using Source = fxe::ConsoleInputSource;
    test_support::OwnerPairingConsoleTransport transport;
    fxe::Console console(transport.hybrid, {false});
    Source observed = Source::Unknown;
    uint64_t generation = 0U;
    std::string line;
    console.SetEnteredLineCallback([&](const char* text) {
        observed = console.CommandInputSource();
        generation = console.CommandRemoteInputGeneration();
        line = text;
        return true;
    });
    transport.usb.input += "next\n"; console.Update();
    Check(observed == Source::Local && line == "next" && generation == 0U &&
              console.CommandInputSource() == Source::Unknown,
          "entered-line owner receives scoped complete USB provenance");
    transport.ble.input += "\n"; console.Update();
    Check(observed == Source::Remote && line.empty() &&
              generation == transport.ble.generation &&
              console.CommandRemoteInputGeneration() == 0U,
          "blank interactive input retains scoped remote generation");
    transport.ble.input += "ne"; console.Update();
    transport.usb.input += "xt\n"; console.Update();
    Check(observed == Source::Mixed,
          "entered-line shortcuts cannot promote mixed bytes to USB authority");
    transport.ble.input += "old"; console.Update();
    ++transport.ble.generation;
    transport.ble.input += "next\n"; console.Update();
    Check(observed == Source::Remote && line == "next" &&
              generation == transport.ble.generation,
          "retired partial input cannot survive into a fresh interactive record");
    fxe::MemoryConsoleTransport unknown;
    fxe::Console unidentified(unknown, {false});
    unidentified.SetEnteredLineCallback([&](const char*) {
        observed = unidentified.CommandInputSource(); return true;
    });
    unknown.PushBytes("next\n", 5U); unidentified.Update();
    Check(observed == Source::Unknown,
          "unidentified interactive transport remains unprivileged");
}
} // namespace

int main() {
    CheckEnteredLineAuthority();
    CheckFoxCommand();
    fxe::MemoryConsoleTransport transport;
    fxe::Console console(transport);
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);
    fxe::MemoryConsoleTransport capacityTransport;
    fxe::Console capacityConsole(capacityTransport);
    fxe::test_support::ConsoleOutputCapture capacityOutput;
    capacityOutput.Attach(capacityTransport);
    const char* lastCommand = "";
    // The firmware's 33 registrations supplement five real
    // built-ins. Hardware handlers are replaced only for this table-capacity
    // control; their actions are outside this generic parser regression.
    const char* registrations[] = {
        "support", "perf", "heap", "engine", "hwtest", "factory", "litchcraft",
        "game", "reset", "fen", "motion", "trace", "board", "ble",
        "input", "comptest", "amptest", "ledpower", "cc", "i2c", "5ven", "setled",
        "rotate", "firstplayer", "flip", "p1black", "reboot", "safemode",
        "uptime", "version", "ver", "sleep", "ambient",
    };
    for (const char* name : registrations) {
        if (std::string(name) == "hwtest") {
#if defined(LITCH_ESP32S3) && defined(LITCH_PANIC_DIAGNOSTIC)
            Check(capacityConsole.RegisterCommand("panicprobe", "private capacity control",
                      [](const fxe::Console::Args&) { return true; }),
                  "private panic command retains its slot");
#endif
        }
        if (std::string(name) == "litchcraft") {
            Check(modules::RegisterFoxConsoleCommand(capacityConsole),
                  "fox fits before the late production registrations");
#if defined(LITCH_OWNER_PAIRING_PREVIEW) && LITCH_OWNER_PAIRING_PREVIEW
            Check(capacityConsole.RegisterCommand("ownerpair", "private capacity control",
                      [](const fxe::Console::Args&) { return true; }),
                  "private owner consent retains its slot");
#endif
        }
        Check(capacityConsole.RegisterCommand(name, "capacity control",
                  [&lastCommand, name](const fxe::Console::Args&) {
                      lastCommand = name;
                      return true;
                  }),
              "full production registration set fits the existing table");
    }
    size_t registeredCount = 39U; // Five built-ins, 33 product commands and fox.
#if defined(LITCH_ESP32S3) && defined(LITCH_PANIC_DIAGNOSTIC)
    ++registeredCount;
#endif
#if defined(LITCH_OWNER_PAIRING_PREVIEW) && LITCH_OWNER_PAIRING_PREVIEW
    ++registeredCount;
#endif
    static_assert(fxe::Console::MAX_COMMANDS == 48U);
    char fillerNames[9U][16U] = {};
    for (size_t i = 0U; i < fxe::Console::MAX_COMMANDS - registeredCount; ++i) {
        std::snprintf(fillerNames[i], sizeof(fillerNames[i]), "capacity-%u",
                      static_cast<unsigned>(i));
        Check(capacityConsole.RegisterCommand(fillerNames[i], "",
                  [](const fxe::Console::Args&) { return true; }),
              "every remaining slot through forty-eight is usable");
    }
    Check(!capacityConsole.RegisterCommand("overflow", "",
              [](const fxe::Console::Args&) { return true; }),
          "the forty-ninth registration is rejected without changing existing commands");
    capacityOutput.Clear();
    fxe::test_support::RunCommand(capacityConsole, capacityTransport, "fox quack");
    Check(capacityOutput.Str().find("duck declines") != std::string::npos,
          "fox remains reachable after the full-table refusal");
    for (const char* late : {"version", "ver", "sleep", "ambient"}) {
        fxe::test_support::RunCommand(capacityConsole, capacityTransport, late);
        Check(std::string(lastCommand) == late,
              "late production command remains reachable with the exact private feature set");
    }

    unsigned dispatches = 0U;
    unsigned argc = 0U;
    std::string value;
    Check(console.RegisterCommand("mark", "parser control",
              [&](const fxe::Console::Args& args) {
                  ++dispatches;
                  argc = args.argc;
                  value = args.Get(1U);
                  return true;
              }), "register parser control");
    fxe::test_support::RunCommand(console, transport, "mark a b c d e f g");
    Check(dispatches == 1U && argc == 8U,
          "exactly eight arguments remain accepted");
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "mark a b c d e f g h");
    Check(dispatches == 1U &&
              output.Str().find("too many arguments") != std::string::npos,
          "ninth token rejects before any handler dispatch");
    fxe::test_support::RunCommand(console, transport, "mark a b c d e f g h ");
    Check(dispatches == 1U,
          "a delimiter after the ninth token cannot bypass argument rejection");
    fxe::test_support::RunCommand(console, transport, "mark \"a b c d e f g h\"");
    Check(dispatches == 2U && argc == 2U && value == "a b c d e f g h",
          "quoted fields preserve the established argument grouping");

    const unsigned beforeNul = dispatches;
    const char nulLine[] = "mark keep\0junk";
    output.Clear();
    transport.PushBytes(nulLine, sizeof(nulLine) - 1U);
    console.Update();
    fxe::test_support::PressEnter(console, transport);
    Check(dispatches == beforeNul &&
              output.Str().find("NUL input") != std::string::npos,
          "embedded NUL cannot turn malformed bytes into a valid prefix");
    fxe::test_support::RunCommand(console, transport, "mark recovered\r\n");
    Check(dispatches == beforeNul + 1U && value == "recovered",
          "next line recovers and CRLF dispatches once");

    const unsigned beforeOverflow = dispatches;
    const std::string longLine = "mark " + std::string(300U, 'x');
    transport.PushBytes(longLine.data(), longLine.size());
    console.Update();
    fxe::test_support::PressEnter(console, transport);
    Check(dispatches == beforeOverflow,
          "input-buffer overflow cannot dispatch a truncated prefix");
    fxe::test_support::RunCommand(console, transport, "mark okX\b");
    Check(dispatches == beforeOverflow + 1U && value == "ok",
          "backspace editing still changes the submitted value");

    transport.PushBytes(nulLine, sizeof(nulLine) - 1U);
    transport.PushChar('\x15'); // Ctrl+U clears the entire rejected draft.
    console.Update();
    fxe::test_support::RunCommand(console, transport, "mark cleared");
    Check(value == "cleared" && dispatches == beforeOverflow + 2U,
          "explicit whole-line editing recovers without a spurious rejection");
    fxe::test_support::RunCommand(console, transport, "mark caf\xC3\xA9");
    Check(value == "caf\xC3\xA9",
          "generic console text retains UTF-8; FEN owns its ASCII restriction");

    unsigned pasteOverflows = 0U;
    unsigned pasteLines = 0U;
    transport.PushBytes(nulLine, sizeof(nulLine) - 1U);
    console.Update();
    Check(console.StartPasteMode(
              [&](const fxe::Console::PasteEvent event, char*, size_t) {
                  if (event == fxe::Console::PasteEvent::LineOverflow) {
                      ++pasteOverflows;
                  } else if (event == fxe::Console::PasteEvent::Line) {
                      ++pasteLines;
                  }
                  return true;
              }), "paste ownership discards the prior draft");
    transport.PushBytes(nulLine, sizeof(nulLine) - 1U);
    transport.PushChar('\n');
    console.Update();
    Check(pasteOverflows == 1U && pasteLines == 0U,
          "raw paste reports NUL as malformed instead of a truncated line");
    transport.PushBytes("safe\n", 5U);
    transport.PushChar('\x04');
    console.Update();
    const unsigned beforePasteRecovery = dispatches;
    fxe::test_support::RunCommand(console, transport, "mark after-paste");
    Check(pasteLines == 1U && dispatches == beforePasteRecovery + 1U &&
              value == "after-paste",
          "paste completion restores ordinary input without a stale rejection");
    std::printf("[CONSOLE_INPUT_BOUNDS] failures=%d Console=%zu Args=%zu commands=%zu\n",
                failures, sizeof(fxe::Console), sizeof(fxe::Console::Args),
                fxe::Console::MAX_COMMANDS);
    return failures == 0 ? 0 : 1;
}
