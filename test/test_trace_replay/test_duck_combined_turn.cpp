#include "bounded_hint_engine.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/manual_millis_guard.hpp"
#include "logger.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "trace/trace_recorder.hpp"
#include "trace/trace_replay.hpp"

namespace {

constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
constexpr uint8_t DUCK_FROM = 24U;
constexpr uint8_t DUCK_TO = 25U;
constexpr uint8_t E2 = 52U;
constexpr uint8_t E4 = 36U;
constexpr size_t MAX_TRACE_LINES = 96U;
constexpr char EXPECTED_FEN[] =
    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1";

struct TraceCapture {
    void Append(const char* line) {
        if (line == nullptr || line[0] != '{' || count >= MAX_TRACE_LINES) {
            return;
        }
        lines[count] = line;
        ++count;
    }

    size_t CountType(const char* type) const {
        size_t matches = 0U;
        for (size_t i = 0U; i < count; ++i) {
            if (lines[i].find(type) != std::string::npos) {
                ++matches;
            }
        }
        return matches;
    }

    void MakeLinePointers(const char* out[MAX_TRACE_LINES]) const {
        for (size_t i = 0U; i < count; ++i) {
            out[i] = lines[i].c_str();
        }
    }

    std::string lines[MAX_TRACE_LINES];
    size_t count = 0U;
};

TraceCapture* g_capture = nullptr;

void CaptureTraceLine(const char* line) {
    if (g_capture != nullptr) {
        g_capture->Append(line);
    }
}

modules::SettingsStoreConfig MakeSettingsConfig(const char* path) {
    modules::SettingsStoreConfig config;
    config.path = path;
    config.deferredSaveDelayMs = 100000U;
    return config;
}

runtime::GameStateMachineConfig MakeStateMachineConfig() {
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    return config;
}

void RemoveSettingsFiles(const char* path) {
    (void)std::remove(path);
    char sibling[192] = {};
    (void)std::snprintf(sibling, sizeof(sibling), "%s.bak", path);
    (void)std::remove(sibling);
    (void)std::snprintf(sibling, sizeof(sibling), "%s.tmp", path);
    (void)std::remove(sibling);
}

struct Harness {
    explicit Harness(const char* path)
        : settingsConfig(MakeSettingsConfig(path)),
          stateMachineConfig(MakeStateMachineConfig()),
          settings(settingsConfig),
          stateMachine(stateMachineConfig),
          coordinator(settings, engine, stateMachine, &eventQueue),
          dispatcher(stateMachine, coordinator, eventQueue) {
        RemoveSettingsFiles(path);
        eventQueue.Reset();
        stateMachine.Reset();
    }

    bool LoadPersistedDuck() {
        if (!settings.Load()) {
            return false;
        }
        settings.Doc()["BOARD_ROTATE"] = 0U;
        settings.Doc()["GAME_FIRST_PLAYER"] = 1U;
        settings.Doc()["VS_CPU_LEVEL"] = 0U;
        settings.Doc()["TIMER"] = 0U;

        modules::GamePersistenceState state;
        if (!state.fen.Assign(modules::INITIAL_FEN) ||
            !state.undoBaseFen.Assign(modules::INITIAL_FEN)) {
            return false;
        }
        modules::DuckGameDurableState duck;
        duck.variant = modules::GameVariant::Duck;
        duck.duckSquare = DUCK_FROM;
        char encoded[32] = {};
        if (!modules::EncodeDuckGameDurableState(
                duck, encoded, sizeof(encoded)) ||
            !state.variantState.Assign(encoded) ||
            !modules::WriteGamePersistenceState(settings.Doc(), state) ||
            !settings.SaveNow()) {
            return false;
        }
        return coordinator.LoadOrInitialize();
    }

    modules::SettingsStoreConfig settingsConfig;
    runtime::GameStateMachineConfig stateMachineConfig;
    modules::SettingsStore settings;
    test_support::BoundedHintEngine engine;
    runtime::GameStateMachine stateMachine;
    runtime::AppEventQueue eventQueue;
    modules::GamePersistenceCoordinator coordinator;
    runtime::InteractionDispatcher dispatcher;
};

void RecordObservation(Harness& harness,
                       trace::TraceRecorder& recorder,
                       const uint64_t changedMask,
                       const uint64_t occupiedMask) {
    const uint32_t sequence = harness.dispatcher.DispatchBoardObservation(
        runtime::BoardObservationEvent{
            .tMs = fxe::Millis(),
            .sequence = 0U,
            .changedMask = changedMask,
            .occupiedMask = occupiedMask,
        });
    recorder.RecordBoardObservation(changedMask, occupiedMask, sequence);
    recorder.RecordCheckpointInteraction(
        harness.stateMachine.GetChessInteractionModel(),
        harness.stateMachine.GetInteractionInvariantStatus());
    harness.coordinator.Update();
}

void RecordCheckpoint(Harness& harness,
                      trace::TraceRecorder& recorder) {
    recorder.RecordCheckpointInteraction(
        harness.stateMachine.GetChessInteractionModel(),
        harness.stateMachine.GetInteractionInvariantStatus());
    recorder.RecordCheckpointVariant(
        harness.coordinator.GetGameVariantStateSnapshot(),
        runtime::ComputeChessInteractionModelHash(
            harness.stateMachine.GetChessInteractionModel()));
}

void RecordTick(Harness& harness,
                trace::TraceRecorder& recorder) {
    recorder.RecordTick();
    harness.dispatcher.DispatchTick(fxe::Millis());
    harness.coordinator.Update();
}

bool ReadFen(modules::IEngine& engine, char* out, const size_t outSize) {
    return engine.GetFen(out, outSize);
}

} // namespace

int RunDuckCombinedTurnTraceReplayTest() {
    int failures = 0;
    const auto Check = [&failures](const bool condition, const char* message) {
        if (!condition) {
            std::printf("[FAIL] %s\n", message);
            ++failures;
        }
    };

    fxe::test_support::ManualMillisGuard millisGuard;
    fxe::ManualMillis::SetNowMs(0U);

    Harness source("build/test_trace_replay_duck_source.json");
    Check(source.LoadPersistedDuck(), "Duck trace source initializes");

    TraceCapture capture;
    g_capture = &capture;
    const logger::Level previousLogLevel = logger::GetMinLevel();
    logger::SetMinLevel(logger::Level::TRACE);
    logger::SetOutputLineCallback(&CaptureTraceLine);

    trace::TraceRecorder recorder;
    source.coordinator.SetTraceRecorder(&recorder);
    trace::TraceSettingsSnapshot traceSettings;
    traceSettings.gameFirstPlayer = 1U;
    traceSettings.gameFlipSides = 0U;
    traceSettings.vsCpuLevel = 0U;
    traceSettings.timer = 0U;
    recorder.Start("test", "native", 1U, traceSettings);
    recorder.RecordSettings(traceSettings);
    recorder.RecordCheckpointInteraction(
        source.stateMachine.GetChessInteractionModel(),
        source.stateMachine.GetInteractionInvariantStatus());
    recorder.RecordCheckpointVariant(
        source.coordinator.GetGameVariantStateSnapshot(),
        runtime::ComputeChessInteractionModelHash(
            source.stateMachine.GetChessInteractionModel()));

    uint64_t physical = INITIAL_OCCUPANCY | (1ULL << DUCK_FROM);
    RecordObservation(source, recorder, physical, physical);
    physical &= ~(1ULL << E2);
    RecordObservation(source, recorder, 1ULL << E2, physical);
    physical |= 1ULL << E4;
    RecordObservation(source, recorder, 1ULL << E4, physical);

    fxe::ManualMillis::AdvanceMs(10U);
    recorder.RecordTick();
    source.dispatcher.DispatchTick(fxe::Millis());
    source.coordinator.Update();

    physical &= ~(1ULL << DUCK_FROM);
    RecordObservation(source, recorder, 1ULL << DUCK_FROM, physical);
    physical |= 1ULL << DUCK_TO;
    RecordObservation(source, recorder, 1ULL << DUCK_TO, physical);
    Check(source.coordinator.GetUndoHistoryMoveCount() == 0U &&
              source.coordinator.GetDuckSquare() == DUCK_FROM,
          "recorded human Duck placement remains uncommitted before settling");
    fxe::ManualMillis::AdvanceMs(9U);
    RecordTick(source, recorder);
    Check(source.coordinator.GetUndoHistoryMoveCount() == 0U,
          "recorded Duck turn remains pending one millisecond before its fixture deadline");
    fxe::ManualMillis::AdvanceMs(1U);
    RecordTick(source, recorder);
    Check(source.coordinator.GetUndoHistoryMoveCount() == 1U &&
              source.coordinator.GetDuckSquare() == DUCK_TO,
          "recorded Duck turn commits at its configured settle boundary");

    Check(source.coordinator.RequestUndo(1U),
          "recorded Duck whole-turn undo begins");
    RecordCheckpoint(source, recorder);
    physical &= ~(1ULL << DUCK_TO);
    RecordObservation(source, recorder, 1ULL << DUCK_TO, physical);
    physical |= 1ULL << DUCK_FROM;
    RecordObservation(source, recorder, 1ULL << DUCK_FROM, physical);

    fxe::ManualMillis::AdvanceMs(1U);
    RecordTick(source, recorder);

    Check(source.coordinator.RequestUndo(1U),
          "recorded cancel-undo begins Duck redo");
    fxe::ManualMillis::AdvanceMs(1U);
    RecordTick(source, recorder);
    RecordCheckpoint(source, recorder);
    physical &= ~(1ULL << DUCK_FROM);
    RecordObservation(source, recorder, 1ULL << DUCK_FROM, physical);
    physical |= 1ULL << DUCK_TO;
    RecordObservation(source, recorder, 1ULL << DUCK_TO, physical);
    recorder.Stop();
    source.coordinator.SetTraceRecorder(nullptr);

    logger::SetOutputLineCallback(nullptr);
    logger::SetMinLevel(previousLogLevel);
    g_capture = nullptr;

    Check(capture.CountType("\"type\":\"action_result\"") >= 5U,
          "recorded Duck turn and undo/redo contain all action results");
    Check(capture.CountType("\"type\":\"checkpoint_interaction\"") >= 3U,
          "recorded Duck turn contains resumable interaction checkpoints");
    Check(capture.CountType("\"type\":\"checkpoint_variant\"") >= 3U,
          "recorded Duck turn contains staged and committed variant checkpoints");

    char sourceFen[litch_max::FEN_BUFFER_CAPACITY] = {};
    Check(ReadFen(source.engine, sourceFen, sizeof(sourceFen)) &&
              std::strcmp(sourceFen, EXPECTED_FEN) == 0 &&
              source.coordinator.GetDuckSquare() == DUCK_TO &&
              source.coordinator.GetUndoHistoryMoveCount() == 1U,
          "recording source restores one complete Duck turn after undo/redo");

    fxe::ManualMillis::SetNowMs(0U);
    Harness replay("build/test_trace_replay_duck_target.json");
    Check(replay.LoadPersistedDuck(), "Duck trace replay target initializes");
    const char* replayLines[MAX_TRACE_LINES] = {};
    capture.MakeLinePointers(replayLines);
    runtime::ManualClock replayClock(0U);
    trace::ReplayResult replayResult;
    const bool replayed = trace::ReplayJsonLines(
        replayLines, capture.count, replayClock, replay.stateMachine,
        replayResult, &replay.coordinator, &replay.engine);
    if (!replayed && replayResult.failureLine != 0U &&
        replayResult.failureLine <= capture.count) {
        const size_t failed = replayResult.failureLine - 1U;
        const size_t first = failed > 3U ? failed - 3U : 0U;
        const size_t last = failed + 3U < capture.count
                                ? failed + 3U
                                : capture.count - 1U;
        for (size_t i = first; i <= last; ++i) {
            std::printf("[TRACE_REPLAY_DUCK] line=%lu%s record=%s\n",
                        static_cast<unsigned long>(i + 1U),
                        i == failed ? " failed" : "",
                        capture.lines[i].c_str());
        }
    }
    Check(replayed,
          replayResult.ok ? "Duck combined-turn replay failed"
                          : replayResult.message);

    char replayFen[litch_max::FEN_BUFFER_CAPACITY] = {};
    Check(ReadFen(replay.engine, replayFen, sizeof(replayFen)) &&
              std::strcmp(replayFen, EXPECTED_FEN) == 0,
          "Duck trace replay restores the final FEN after undo/redo");
    Check(replay.coordinator.GetDuckSquare() == DUCK_TO,
          "Duck trace replay restores the duck square after undo/redo");
    Check(replay.coordinator.GetUndoHistoryMoveCount() == 1U,
          "Duck trace replay retains one combined-turn history entry");
    Check(replay.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::ChessMove &&
              !replay.coordinator.GetVariantOutcome().IsTerminal(),
          "Duck trace replay finishes in a non-terminal chess-move phase");
    Check(!replay.coordinator.AreInteractionEventsExternalized(),
          "Duck trace replay restores ordinary action dispatch mode");
    return failures;
}
