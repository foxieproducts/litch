#include <cstddef>
#include <cstdint>
#include <cstring>

#include "modules/trace_console_commands.hpp"

#include "logger.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/settings_store.hpp"
#include "modules/takeback_grace_setting.hpp"
#include "trace/trace_recorder.hpp"
#include "trace/trace_replay.hpp"

namespace modules {

namespace {

size_t AddCompletionMatch(const char* candidate,
                          const char* prefix,
                          const char** outMatches,
                          size_t matchCount,
                          const size_t outMatchesCap) {
    if (!candidate || candidate[0] == '\0') {
        return matchCount;
    }

    if (prefix && prefix[0] != '\0') {
        const size_t prefixLen = std::strlen(prefix);
        if (std::strncmp(candidate, prefix, prefixLen) != 0) {
            return matchCount;
        }
    }

    if (outMatches && matchCount < outMatchesCap) {
        outMatches[matchCount] = candidate;
    }
    return matchCount + 1;
}

template <size_t N>
size_t CompleteChoices(const size_t expectedArgIndex,
                       const size_t argIndex,
                       const char* prefix,
                       const char** outMatches,
                       const size_t outMatchesCap,
                       const char* const (&choices)[N]) {
    if (argIndex != expectedArgIndex) {
        return 0;
    }

    size_t matchCount = 0;
    for (const char* choice : choices) {
        matchCount = AddCompletionMatch(choice, prefix, outMatches, matchCount, outMatchesCap);
    }
    return matchCount;
}

size_t TraceComplete(const fxe::Console::Args& args,
                     const size_t argIndex,
                     const char* prefix,
                     const char** outMatches,
                     const size_t outMatchesCap) {
    static constexpr const char* kTraceSubcommands[] = {
        "record",
        "replay",
    };
    static constexpr const char* kRecordModes[] = {
        "on",
        "off",
    };

    if (argIndex == 1) {
        return CompleteChoices(1, argIndex, prefix, outMatches, outMatchesCap, kTraceSubcommands);
    }
    if (argIndex == 2 && args.Equals(1, "record")) {
        return CompleteChoices(2, argIndex, prefix, outMatches, outMatchesCap, kRecordModes);
    }
    return 0;
}

trace::TraceSettingsSnapshot MakeTraceSettingsSnapshot(const fxe::SettingsStore& settings) {
    trace::TraceSettingsSnapshot s;

    s.brightness = settings.ReadValue("BRIGHTNESS", s.brightness);

    const auto& doc = settings.Doc();
    if (doc.containsKey("BOARD_ROTATE")) {
        s.gameFlipSides = settings.ReadValue("BOARD_ROTATE", s.gameFlipSides);
    }

    if (doc.containsKey("GAME_FIRST_PLAYER")) {
        s.gameFirstPlayer =
            settings.ReadValue("GAME_FIRST_PLAYER", s.gameFirstPlayer);
    }

    if (doc.containsKey("VS_CPU_LEVEL")) {
        s.vsCpuLevel = settings.ReadValue("VS_CPU_LEVEL", s.vsCpuLevel);
    }

    s.vsCpuPlayer = settings.ReadValue("VS_CPU_PLAYER", s.vsCpuPlayer);

    s.timer = settings.ReadValue("TIMER", s.timer);
    s.takebackGraceSeconds =
        takeback_grace_setting::ReadSeconds(settings);

    return s;
}

} // namespace

bool TraceCommand(TraceConsoleCommandContext& ctx, const fxe::Console::Args& args) {
    if (!ctx.console || !ctx.settings || !ctx.stateMachine) {
        return false;
    }

    const std::string_view sub = args.Has(1) ? args.Get(1) : std::string_view();
    if (sub.empty()) {
        ctx.console->PrintLn("usage: trace <record|replay> ...");
        return false;
    }

    if (sub == "record") {
        if (!ctx.recorder) {
            ctx.console->PrintLn("trace: unavailable");
            return false;
        }

        const std::string_view mode = args.Has(2) ? args.Get(2) : std::string_view();
        if (mode.empty()) {
            ctx.console->PrintLn("trace: %s", ctx.recorder->IsActive() ? "on" : "off");
            return true;
        }

        if (mode == "on") {
            if (ctx.recorder->IsActive()) {
                ctx.console->PrintLn("trace: already on");
                return true;
            }

            const trace::TraceSettingsSnapshot snap = MakeTraceSettingsSnapshot(*ctx.settings);
            ctx.recorder->Start(ctx.buildId, ctx.boardId, ctx.rngSeed, snap);
            ctx.recorder->RecordSettings(snap);
            ctx.recorder->RecordCheckpointState(ctx.stateMachine->DescribeInteractionState(),
                                                ctx.stateMachine->IsPlacementModeActive(),
                                                ctx.stateMachine->IsCpuThinkingActive(),
                                                ctx.stateMachine->HasHumanIllegal(),
                                                ctx.stateMachine->HasCpuIllegal());
            ctx.recorder->RecordCheckpointInteraction(
                ctx.stateMachine->GetChessInteractionModel(),
                ctx.stateMachine->GetInteractionInvariantStatus());
            if (ctx.gamePersistence != nullptr) {
                const GameVariantStateSnapshot variantState =
                    ctx.gamePersistence->GetGameVariantStateSnapshot();
                ctx.recorder->RecordCheckpointVariant(
                    variantState,
                    runtime::ComputeChessInteractionModelHash(
                        ctx.stateMachine->GetChessInteractionModel()));
            }
            ctx.console->PrintLn("OK");
            if (logger::GetMinLevel() != logger::Level::TRACE) {
                ctx.console->PrintLn("trace: note: set 'debug trace' to stream JSONL");
            }
            return true;
        }

        if (mode == "off") {
            if (!ctx.recorder->IsActive()) {
                ctx.console->PrintLn("trace: already off");
                return true;
            }

            ctx.recorder->Stop();
            ctx.console->PrintLn("OK");
            return true;
        }

        ctx.console->PrintLn("usage: trace record <on|off>");
        return false;
    }

    if (sub == "replay") {
#ifndef ARDUINO
        if (!ctx.replayClock) {
            ctx.console->PrintLn("trace: replay unavailable");
            return false;
        }

        if (!args.Has(2)) {
            ctx.console->PrintLn("usage: trace replay <path>");
            return false;
        }

        trace::ReplayResult r;
        ctx.replayClock->SetNowMs(0);
        ctx.stateMachine->Reset();

        const bool ok = trace::ReplayJsonlFile(args.CStr(2), *ctx.replayClock, *ctx.stateMachine, r);
        if (!ok) {
            ctx.console->PrintLn("FAIL line=%lu msg=%s", static_cast<unsigned long>(r.failureLine), r.message);
            return false;
        }

        ctx.console->PrintLn("OK");
        return true;
#else
        ctx.console->PrintLn("trace: replay unsupported");
        return false;
#endif
    }

    ctx.console->PrintLn("usage: trace <record|replay> ...");
    return false;
}

void RegisterTraceConsoleCommands(fxe::Console& console,
                                  fxe::SettingsStore& settings,
                                  runtime::GameStateMachine& stateMachine,
                                  GamePersistenceCoordinator* gamePersistence,
                                  trace::TraceRecorder* recorder,
                                  const char* buildId,
                                  const char* boardId,
                                  const uint32_t rngSeed,
                                  TraceConsoleCommandContext& ctx
#ifndef ARDUINO
                                  ,
                                  runtime::ManualClock* replayClock
#endif
) {
    ctx.console = &console;
    ctx.settings = &settings;
    ctx.stateMachine = &stateMachine;
    ctx.gamePersistence = gamePersistence;
    ctx.recorder = recorder;
    ctx.buildId = buildId;
    ctx.boardId = boardId;
    ctx.rngSeed = rngSeed;
#ifndef ARDUINO
    ctx.replayClock = replayClock;
#endif

    console.RegisterCommand(
        "trace",
        "Trace: record|replay",
        [&ctx](const fxe::Console::Args& args) { return TraceCommand(ctx, args); },
        [](const fxe::Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return TraceComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        },
        "Usage: trace record <on|off> | trace replay <path>");
}

} // namespace modules
