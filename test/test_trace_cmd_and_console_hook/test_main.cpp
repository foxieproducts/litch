#include <cstdio>
#include <cstring>

#include "console.hpp"
#include "foxie_embedded/test_support/manual_millis_guard.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "logger.hpp"
#include "runtime/clock.hpp"
#include "trace/trace_recorder.hpp"

namespace {

struct Capture {
    bool foundCmd = false;
    char cmdLine[256] = {};

    bool foundConsoleLine = false;
    char consoleLine[256] = {};
};

} // namespace

int main() {
    fxe::test_support::TestResults results("TRACE_CMD_CONSOLE_HOOK");
    results.Start();

    logger::SetMinLevel(logger::Level::TRACE);
    logger::ClearBufferedLines();

    // Verify TraceRecorder cmd JSON escaping.
    runtime::ManualClock clock;
    fxe::test_support::ManualMillisGuard millisGuard;
    clock.SetNowMs(1000);

    trace::TraceRecorder recorder;
    trace::TraceSettingsSnapshot settings;
    recorder.Start("test_build", "test_board", 0, settings);

    clock.SetNowMs(1005);
    recorder.RecordCmd("say \"hi\" \\ line\n");

    Capture cap;
    logger::DumpBufferedLines([&cap](const char* line) {
        if (!line) {
            return;
        }

        if (std::strstr(line, "\"type\":\"cmd\"") == nullptr) {
            return;
        }

        cap.foundCmd = true;
        std::strncpy(cap.cmdLine, line, sizeof(cap.cmdLine));
        cap.cmdLine[sizeof(cap.cmdLine) - 1] = '\0';
    });
    results.Check(cap.foundCmd, "cmd record present");

    const char* expected = "{\"type\":\"cmd\",\"t_ms\":5,\"cmd\":\"say \\\"hi\\\" \\\\ line\\n\"}";
    results.Check(std::strcmp(cap.cmdLine, expected) == 0, "cmd record matches expected escaping");

    // Verify Console hook sees the raw line.
    MemoryConsoleTransport transport;
    Console console(transport);
    console.SetCommandLineCallback([&cap](const char* line) {
        if (!line) {
            return;
        }

        cap.foundConsoleLine = true;
        std::strncpy(cap.consoleLine, line, sizeof(cap.consoleLine));
        cap.consoleLine[sizeof(cap.consoleLine) - 1] = '\0';
    });

    transport.PushBytes("help\n", 5);
    console.Update();

    results.Check(cap.foundConsoleLine, "console hook fired");
    results.Check(std::strcmp(cap.consoleLine, "help") == 0, "console hook line matches");

    return results.Finish();
}
