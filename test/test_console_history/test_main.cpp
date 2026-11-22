#include <cstdio>
#include <cstring>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/console_transport_memory.hpp"
#include "foxie_embedded/fixed_string.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

void Feed(fxe::Console& console,
          fxe::MemoryConsoleTransport& transport,
          const char* text) {
    fxe::test_support::FeedConsole(console, transport, text);
}

} // namespace

int main() {
    std::puts("[CONSOLE_HISTORY] start");
    fxe::MemoryConsoleTransport transport;
    fxe::Console::Config config;
    config.installBuiltins = false;
    fxe::Console console(transport, config);
    fxe::FixedString<256> lastValue;
    uint8_t executionCount = 0U;
    Check(console.RegisterCommand(
              "mark",
              "record a value",
              [&](const fxe::Console::Args& args) {
                  ++executionCount;
                  return args.Has(1U) && lastValue.Assign(args.Get(1U));
              }),
          "register mark command");

    const char* commands[] = {
        "mark one\r", "mark two\r", "mark three\r",
        "mark four\r", "mark five\r", "mark six\r",
    };
    for (const char* command : commands) {
        Feed(console, transport, command);
    }

    for (uint8_t i = 0U; i < 6U; ++i) {
        Feed(console, transport, "\x1b[A");
    }
    Feed(console, transport, "\r");
    Check(lastValue == "two",
          "history retains exactly the five newest commands");

    Feed(console, transport, "mark draft");
    Feed(console, transport, "\x1b[A");
    Feed(console, transport, "\x1b[B");
    Feed(console, transport, "\r");
    Check(lastValue == "draft",
          "down from newest history restores the scratch line");

    Feed(console, transport, "\x1b[A\r");
    for (uint8_t i = 0U; i < 5U; ++i) {
        Feed(console, transport, "\x1b[A");
    }
    Feed(console, transport, "\r");
    Check(lastValue == "four",
          "consecutive duplicate execution does not evict older history");

    char maximumLine[fxe::Console::INPUT_CAPACITY] = {};
    std::memcpy(maximumLine, "mark ", 5U);
    std::memset(maximumLine + 5U, 'x', 250U);
    maximumLine[255] = '\0';
    Feed(console, transport, maximumLine);
    Feed(console, transport, "\r\x1b[A\r");
    Check(lastValue.Size() == 250U,
          "history preserves a maximum-length command line");
    Check(executionCount == 12U,
          "history navigation executes only submitted lines");

    if (g_failures == 0) {
        std::puts("[CONSOLE_HISTORY] ok");
        return 0;
    }
    std::printf("[CONSOLE_HISTORY] failures=%d\n", g_failures);
    return 1;
}
