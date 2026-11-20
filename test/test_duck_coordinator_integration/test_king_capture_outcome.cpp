#include <cstdint>
#include <cstdio>
#include <cstring>

#include "duck_coordinator_test_support.hpp"
#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/game_variant_state.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"
#include "trace/trace_jsonl_event_reader.hpp"
#include "trace/trace_recorder.hpp"

namespace {

using duck_coordinator_test::Harness;

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

uint64_t MaskFor(const uint8_t square) {
    return 1ULL << square;
}

void SetTime(const uint32_t nowMs = 1000U) {
    fxe::ManualMillis::SetNowMs(nowMs);
}

} // namespace

int RunDuckKingCaptureOutcomeTest() {
    SetTime();
    static constexpr char PATH[] =
        "build/test_duck_coord_king_capture.json";
    Harness harness(PATH);
    static constexpr char FEN[] =
        "4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1";
    constexpr uint8_t DUCK = 32U;
    Check(duck_coordinator_test::SeedPersistedDuckPosition(
              harness, FEN, DUCK),
          "king-capture Duck position persists");
    harness.settings.Doc()["TIMER"] = 1U;
    harness.settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    Check(harness.coordinator.LoadOrInitialize(),
          "king-capture Duck position loads");
    uint64_t physical = duck_coordinator_test::FenOccupancy(FEN) | MaskFor(DUCK);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);

    char checkpoint[320] = {};
    logger::SetMinLevel(logger::Level::TRACE);
    logger::SetOutputLineCallback([&checkpoint](const char* line) {
        if (line != nullptr &&
            std::strstr(line, "\"type\":\"checkpoint_variant\"") !=
                nullptr) {
            std::strncpy(checkpoint, line, sizeof(checkpoint) - 1U);
        }
    });
    trace::TraceRecorder recorder;
    trace::TraceSettingsSnapshot traceSettings;
    recorder.Start("test", "native", 1U, traceSettings);
    harness.coordinator.SetTraceRecorder(&recorder);

    harness.coordinator.Update();
    Check(harness.coordinator.GetGameClock().IsRunning(),
          "king-capture fixture clock runs before the terminal action");
    physical &= ~MaskFor(4U);
    duck_coordinator_test::Observe(harness.stateMachine, MaskFor(4U), physical);
    physical &= ~MaskFor(52U);
    duck_coordinator_test::Observe(harness.stateMachine, MaskFor(52U), physical);
    physical |= MaskFor(4U);
    duck_coordinator_test::Observe(harness.stateMachine, MaskFor(4U), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
    recorder.Stop();
    logger::SetOutputLineCallback(nullptr);
    logger::SetMinLevel(logger::Level::INFO);
    const modules::VariantOutcome outcome =
        harness.coordinator.GetVariantOutcome();
    Check(outcome.reason == modules::VariantOutcomeReason::KingCaptured &&
              outcome.winner == modules::GameWinner::White,
          "king capture immediately awards White without duck relocation");
    Check(harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::ChessMove &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U,
          "king capture commits one terminal chess-only turn");
    Check(harness.stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle &&
              !harness.stateMachine.IsDuckRelocationActive() &&
              !harness.stateMachine.HasQueuedCpuMove() &&
              !harness.stateMachine.IsHintThinkingActive(),
          "king capture terminates interaction without a duck or search phase");
    Check(harness.coordinator.GetDuckSquare() == DUCK &&
              (physical & MaskFor(DUCK)) != 0ULL &&
              std::strstr(harness.engine.Fen(), "4k3") == nullptr,
          "terminal chess-only turn leaves the duck and removes the king");
    Check(!harness.coordinator.GetGameClock().IsRunning(),
          "variant terminal outcome stops the game clock");

    runtime::InteractionModelSnapshotBuildInputs snapshotInputs;
    snapshotInputs.stateMachine = &harness.stateMachine;
    snapshotInputs.derived = &harness.coordinator.GetEngineDerivedState();
    snapshotInputs.authoritativeFen = harness.coordinator.GetAuthoritativeFen();
    snapshotInputs.gameClock = &harness.coordinator.GetGameClock();
    snapshotInputs.variant = harness.coordinator.GetGameVariant();
    snapshotInputs.duckSquare = harness.coordinator.GetDuckSquare();
    snapshotInputs.duckTurnPhase = harness.coordinator.GetDuckTurnPhase();
    snapshotInputs.variantOutcome = outcome;
    const runtime::InteractionModelSnapshot snapshot =
        runtime::BuildInteractionModelSnapshot(snapshotInputs);
    const auto intent =
        modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &snapshot});
    Check(intent.celebration ==
              modules::led_frame_composer::internal::
                  CelebrationIntent::VariantVictory,
          "king capture selects Duck variant-victory celebration");

    const modules::GameVariantStateSnapshot terminalState =
        harness.coordinator.GetGameVariantStateSnapshot();
    const uint64_t terminalHash =
        modules::ComputeGameVariantStateHash(terminalState);
    trace::JsonlEventRecord traceRecord;
    Check(trace::TryParseJsonlEventRecord(checkpoint, traceRecord) &&
              traceRecord.hasVariantOutcomeReason &&
              traceRecord.variantOutcomeReason ==
                  static_cast<uint8_t>(
                      modules::VariantOutcomeReason::KingCaptured) &&
              traceRecord.hasVariantWinner &&
              traceRecord.variantWinner ==
                  static_cast<uint8_t>(modules::GameWinner::White) &&
              traceRecord.hasVariantStateHash &&
              traceRecord.variantStateHash == terminalHash,
          "terminal trace checkpoint records outcome, winner, and state hash");

    Check(harness.coordinator.FlushToSettingsNow(),
          "terminal Duck state flushes at the simulated reboot boundary");
    Harness after(PATH, false);
    Check(after.LoadExisting(), "terminal Duck snapshot reloads");
    after.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    after.coordinator.Update();
    Check(after.coordinator.GetVariantOutcome().reason ==
                  modules::VariantOutcomeReason::KingCaptured &&
              after.coordinator.GetVariantOutcome().winner ==
                  modules::GameWinner::White &&
              after.coordinator.GetDuckSquare() == DUCK &&
              std::strcmp(after.engine.Fen(), harness.engine.Fen()) == 0,
          "reboot restores captured-king FEN, winner, and duck square");
    Check(!after.coordinator.GetGameClock().IsRunning() &&
              modules::ComputeGameVariantStateHash(
                  after.coordinator.GetGameVariantStateSnapshot()) ==
                  terminalHash,
          "reboot keeps terminal clock stopped and variant hash stable");

    runtime::InteractionDispatcher dispatcher(
        harness.stateMachine, harness.coordinator);
    uint32_t sequence =
        harness.stateMachine.GetLastBoardObservationSequence() + 1U;
    physical &= ~MaskFor(4U);
    dispatcher.DispatchBoardObservation(runtime::BoardObservationEvent{
        fxe::Millis(), sequence++, MaskFor(4U), physical});
    Check(harness.stateMachine.IsUndoPromptActive() &&
              harness.coordinator.HasPendingUndoCommand(),
          "terminal Duck grace enters positive Undo without duck relocation");
    harness.coordinator.Update();
    Check(harness.coordinator.GetVariantOutcome().reason ==
                  modules::VariantOutcomeReason::None &&
              harness.coordinator.GetGameClock().IsRunning(),
          "terminal Duck inverse clears outcome and restores the live clock");
    physical |= MaskFor(52U);
    dispatcher.DispatchBoardObservation(runtime::BoardObservationEvent{
        fxe::Millis(), sequence++, MaskFor(52U), physical});
    Check(harness.stateMachine.GetChessInteractionModel().phase.requiredAction ==
              runtime::RequiredPhysicalAction::RestoreCapture,
          "terminal Duck inverse asks for the captured king last");
    physical |= MaskFor(4U);
    dispatcher.DispatchBoardObservation(runtime::BoardObservationEvent{
        fxe::Millis(), sequence++, MaskFor(4U), physical});
    harness.coordinator.Update();
    Check(harness.coordinator.GetUndoHistoryMoveCount() == 0U &&
              harness.coordinator.GetDuckSquare() == DUCK &&
              std::strcmp(harness.engine.Fen(), FEN) == 0 &&
              physical ==
                  (duck_coordinator_test::FenOccupancy(FEN) |
                   MaskFor(DUCK)),
          "terminal Duck guided inverse restores chess, duck, and history");
    return g_failures;
}
