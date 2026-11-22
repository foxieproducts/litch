#include <cstdint>
#include <cstdio>

#include "duck_coordinator_test_support.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/game_variant.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"

namespace {

using duck_coordinator_test::Harness;
using duck_coordinator_test::INITIAL_OCCUPANCY;

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

void SetTime() {
    fxe::ManualMillis::SetNowMs(1000U);
}

void LatchDuck(Harness& harness, const uint8_t duckSquare) {
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | MaskFor(duckSquare));
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
}

void TestDuckLiftDuringChessSettleTransitionsDirectly(
    const uint32_t settleDurationMs) {
    SetTime();
    Harness harness("build/test_duck_coord_early_duck_lift.json");
    Check(harness.LoadFresh(), "early-duck-lift harness initializes");
    harness.stateMachine.SetMoveConfirmPolicy(settleDurationMs, false);
    constexpr uint8_t DUCK_FROM = 24U;
    constexpr uint8_t DUCK_TO = 25U;
    LatchDuck(harness, DUCK_FROM);

    uint64_t physical = INITIAL_OCCUPANCY | MaskFor(DUCK_FROM);
    physical &= ~MaskFor(52U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(52U), physical);
    physical |= MaskFor(36U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(36U), physical);
    Check(harness.stateMachine.IsMoveSettlingActive(),
          "quiet Duck chess move enters settling");

    physical &= ~MaskFor(DUCK_FROM);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_FROM), physical);
    harness.coordinator.Update();

    const runtime::TaggedInteractionState& relocation =
        harness.stateMachine.GetChessInteractionModel().phase;
    Check(!harness.stateMachine.IsMoveSettlingActive(),
          "duck pickup ends chess settling immediately");
    Check(harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::DuckRelocation &&
              relocation.tag ==
                  runtime::ChessInteractionPhase::DuckRelocation &&
              relocation.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary,
          "duck pickup continues directly as an in-hand relocation");
    Check(!harness.stateMachine.HasHumanIllegal(),
          "duck pickup during settling never enters invalid feedback");
    runtime::InteractionModelSnapshotBuildInputs snapshotInputs;
    snapshotInputs.stateMachine = &harness.stateMachine;
    snapshotInputs.derived = &harness.coordinator.GetEngineDerivedState();
    snapshotInputs.authoritativeFen = harness.coordinator.GetAuthoritativeFen();
    snapshotInputs.variant = harness.coordinator.GetGameVariant();
    snapshotInputs.duckSquare = harness.coordinator.GetDuckSquare();
    snapshotInputs.duckTurnPhase = harness.coordinator.GetDuckTurnPhase();
    const runtime::InteractionModelSnapshot snapshot =
        runtime::BuildInteractionModelSnapshot(snapshotInputs);
    modules::led_frame_composer::ComposeInputs composeInputs;
    composeInputs.settings = &harness.settings;
    composeInputs.interactionSnapshot = &snapshot;
    modules::led_frame_composer::internal::ComposeContext composeContext;
    Check(modules::led_frame_composer::internal::BuildComposeContext(
              composeInputs, composeContext) &&
              !modules::led_frame_composer::internal::
                   ComputeInvalidBoardState(composeContext).IsActive(),
          "early duck relocation renders guidance instead of invalid board");

    physical |= MaskFor(DUCK_TO);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_TO), physical);
    harness.coordinator.Update();
    Check(harness.coordinator.GetDuckSquare() == DUCK_TO &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U,
          "early duck pickup still commits one complete compound turn");
}

void TestExternalizedEarlyDuckLiftResumesAfterActionResult() {
    SetTime();
    Harness harness("build/test_duck_coord_external_early_lift.json");
    Check(harness.LoadFresh(),
          "external early-duck-lift harness initializes");
    constexpr uint8_t DUCK_FROM = 24U;
    LatchDuck(harness, DUCK_FROM);
    harness.coordinator.SetInteractionEventsExternalized(true);

    uint64_t physical = INITIAL_OCCUPANCY | MaskFor(DUCK_FROM);
    physical &= ~MaskFor(52U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(52U), physical);
    physical |= MaskFor(36U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(36U), physical);
    physical &= ~MaskFor(DUCK_FROM);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_FROM), physical);
    harness.coordinator.Update();

    const runtime::TaggedInteractionState& awaiting =
        harness.stateMachine.GetChessInteractionModel().phase;
    const uint32_t actionToken = awaiting.pendingActionToken;
    Check(awaiting.tag == runtime::ChessInteractionPhase::AwaitingAction &&
              actionToken != 0U,
          "externalized early lift waits for recorded chess action result");
    harness.stateMachine.HandleInteractionActionResult(actionToken, true);
    Check(harness.coordinator.ResumeExternalizedDuckActionAfterCheckpoint(),
          "externalized early lift resumes after action checkpoint");
    const runtime::TaggedInteractionState& relocation =
        harness.stateMachine.GetChessInteractionModel().phase;
    Check(relocation.tag ==
                  runtime::ChessInteractionPhase::DuckRelocation &&
              relocation.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary,
          "trace replay inherits the already-lifted duck");
    harness.coordinator.SetInteractionEventsExternalized(false);
}

} // namespace

int RunDuckCoordinatorEarlyLiftTests() {
    TestDuckLiftDuringChessSettleTransitionsDirectly(2000U);
    TestDuckLiftDuringChessSettleTransitionsDirectly(5000U);
    TestExternalizedEarlyDuckLiftResumesAfterActionResult();
    return g_failures;
}
