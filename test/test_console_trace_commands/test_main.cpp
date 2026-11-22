#include <cstdio>
#include <cstring>
#include <string>

#include "console.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "foxie_embedded/test_support/manual_millis_guard.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "logger.hpp"
#include "modules/settings_store.hpp"
#include "modules/trace_console_commands.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "trace/trace_recorder.hpp"

namespace {

bool WriteTextFile(const char* path, const char* contents) {
    if (!path || !contents) {
        return false;
    }

    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        return false;
    }

    const size_t n = std::strlen(contents);
    const size_t w = std::fwrite(contents, 1, n, f);
    std::fclose(f);
    return w == n;
}

} // namespace

int main() {
    fxe::test_support::TestResults results("CONSOLE_TRACE_COMMANDS");
    results.Start();

    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_console_trace_commands_settings.json";
    cfg.deferredSaveDelayMs = 100000;
    (void)std::remove(cfg.path);
    (void)std::remove("build/test_console_trace_commands_settings.json.bak");
    (void)std::remove("build/test_console_trace_commands_settings.json.tmp");

    modules::SettingsStore settings(cfg);
    results.Check(settings.Load(), "settings load");
    settings.Doc()["GAME_FIRST_PLAYER"] = 2U;

    logger::SetMinLevel(logger::Level::TRACE);
    logger::ClearBufferedLines();

    runtime::ManualClock clock;
    fxe::test_support::ManualMillisGuard millisGuard;
    clock.SetNowMs(7);
    runtime::GameStateMachine sm;

    trace::TraceRecorder recorder;

    MemoryConsoleTransport transport;
    Console console(transport);
    console.SetSettingsStore(&settings);
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);

    modules::TraceConsoleCommandContext traceCmdCtx;
    modules::RegisterTraceConsoleCommands(console,
                                          settings,
                                          sm,
                                          nullptr,
                                          &recorder,
                                          "test",
                                          "native",
                                          0,
                                          traceCmdCtx,
                                          &clock);

    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "trace ");
    transport.PushChar('\t');
    console.Update();
    results.Check(output.Str().find("record\r\n") != std::string::npos,
                  "trace completion lists record");
    results.Check(output.Str().find("replay\r\n") != std::string::npos,
                  "trace completion lists replay");
    fxe::test_support::PressEnter(console, transport);

    output.Clear();
    fxe::test_support::FeedConsole(console, transport, "trace record ");
    transport.PushChar('\t');
    console.Update();
    results.Check(output.Str().find("on\r\n") != std::string::npos,
                  "trace record completion lists on");
    results.Check(output.Str().find("off\r\n") != std::string::npos,
                  "trace record completion lists off");
    fxe::test_support::PressEnter(console, transport);

    results.Check(!recorder.IsActive(), "trace recorder starts inactive");

    fxe::test_support::RunCommand(console, transport, "trace record on");
    results.Check(recorder.IsActive(), "trace record on enables recorder");

    fxe::test_support::FeedConsole(console, transport, "trace record of");
    transport.PushChar('\t');
    console.Update();
    fxe::test_support::PressEnter(console, transport);
    results.Check(!recorder.IsActive(), "trace record off completes and executes");

    bool foundFirstPlayerSetting = false;
    logger::DumpBufferedLines([&foundFirstPlayerSetting](const char* line) {
        foundFirstPlayerSetting = foundFirstPlayerSetting ||
            (line != nullptr &&
             std::strstr(line, "\"GAME_FIRST_PLAYER\":2") != nullptr);
    });
    results.Check(foundFirstPlayerSetting,
                  "trace record captures effective first-player orientation");

    fxe::test_support::RunCommand(console, transport, "trace record off");
    results.Check(!recorder.IsActive(), "trace record off remains disabled");

    const char* tracePath = "build/test_console_trace_commands_replay.jsonl";
    (void)std::remove(tracePath);

    const char* traceJsonl =
        "{\"type\":\"header\",\"ver\":1}\n"
        "{\"type\":\"tick\",\"t_ms\":123}\n"
        "{\"type\":\"checkpoint_state\",\"t_ms\":123,\"game_state\":\"Idle\"}\n"
        "{\"type\":\"end\",\"t_ms\":123,\"stats\":{\"events\":0,\"frames\":0,\"engine\":0}}\n";

    results.Check(WriteTextFile(tracePath, traceJsonl), "write replay trace jsonl");

    clock.SetNowMs(999);
    sm.HandleInteractionPositionAcknowledged(1ULL << 12U, 0ULL, 0ULL,
                                             1ULL << 12U);
    sm.ApplyPhysicalOccupancySnapshot(1ULL << 12U);
    sm.HandleSensorChange(12U, false);

    fxe::test_support::RunCommand(console,
                                  transport,
                                  "trace replay build/test_console_trace_commands_replay.jsonl");
    results.Check(clock.NowMs() == 123, "trace replay advances clock to last t_ms");
    results.Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "trace replay resets to Idle");

    return results.Finish();
}
