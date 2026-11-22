#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/game_state_machine.hpp"
#include "../support/led_frame_composer_snapshot_adapter.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

runtime::ChessInteractionEvent Position(const uint64_t occupied,
                                        const uint64_t kings,
                                        const uint64_t rooks,
                                        const uint64_t mover,
                                        const uint64_t opponent) {
    return runtime::ChessInteractionEvent::MakePositionAcknowledged(
        occupied,
        kings,
        rooks,
        mover,
        opponent,
        0ULL,
        runtime::CHESS_INTERACTION_INVALID_SQUARE,
        runtime::CHESS_INTERACTION_INVALID_SQUARE,
        false,
        false,
        true,
        opponent);
}

void Observe(runtime::ChessInteractionModel& model,
             const uint32_t sequence,
             const uint64_t changed,
             const uint64_t occupied) {
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            sequence * 10U,
            sequence,
            changed,
            occupied));
}

void ConfirmSettle(runtime::ChessInteractionModel& model) {
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeSettleConfirmed());
}

void TestQuietMoveAndCancel() {
    constexpr uint8_t FROM = 52U;
    constexpr uint8_t TO = 36U;
    constexpr uint8_t OPPONENT = 12U;
    constexpr uint64_t FROM_MASK = (1ULL << FROM);
    constexpr uint64_t TO_MASK = (1ULL << TO);
    constexpr uint64_t OPPONENT_MASK = (1ULL << OPPONENT);
    constexpr uint64_t BEFORE = FROM_MASK | OPPONENT_MASK;
    constexpr uint64_t AFTER = TO_MASK | OPPONENT_MASK;

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model,
        Position(BEFORE, 0, 0, FROM_MASK, OPPONENT_MASK));
    Observe(model, 1, 0, BEFORE);
    Observe(model, 2, FROM_MASK, OPPONENT_MASK);
    Check(model.phase.tag == runtime::ChessInteractionPhase::PieceLifted,
          "quiet pickup enters PieceLifted");
    Check(model.phase.selectedSquare == FROM,
          "quiet pickup retains source identity");

    Observe(model, 3, FROM_MASK, BEFORE);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Idle,
          "put-back cancels without a plan");
    Check(model.phase.pendingActionToken == 0U,
          "put-back leaves no pending action");

    Observe(model, 4, FROM_MASK, OPPONENT_MASK);
    Observe(model, 5, TO_MASK, AFTER);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Settling,
          "quiet placement waits for physical settle confirmation");
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::QuietMove,
          "quiet placement uses common transition plan");
    Check(model.phase.plan.moveFrom == FROM && model.phase.plan.moveTo == TO,
          "quiet plan owns engine move identity");
    Check(model.phase.plan.relocationCount == 1U,
          "quiet plan has one relocation");
    Check(model.phase.pendingActionToken == 0U,
          "quiet placement exposes no engine action before settle");
    Check(model.acknowledged.occupiedMask == BEFORE,
          "quiet physical completion does not advance logical position");

    ConfirmSettle(model);
    Check(model.phase.tag == runtime::ChessInteractionPhase::AwaitingAction,
          "settle confirmation exposes the engine action");
    const uint32_t token = model.phase.pendingActionToken;
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeActionResult(token, true));
    Check(model.phase.tag == runtime::ChessInteractionPhase::Idle,
          "quiet action acknowledgement completes plan");
    Check(model.acknowledged.occupiedMask == AFTER,
          "quiet action acknowledgement advances logical position");
}

void TestQuietClassificationGuards() {
    constexpr uint64_t MOVER = (1ULL << 52);
    constexpr uint64_t OPPONENT = (1ULL << 36);
    constexpr uint64_t BEFORE = MOVER | OPPONENT;

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model,
        Position(BEFORE, 0, 0, MOVER, OPPONENT));
    Observe(model, 1, 0, BEFORE);
    Observe(model, 2, OPPONENT, MOVER);
    Check(model.phase.tag == runtime::ChessInteractionPhase::CaptureStaged,
          "opponent removal is staged as a capture, not a quiet source");

    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model,
        Position(BEFORE, 0, 0, MOVER, OPPONENT));
    Observe(model, 1, 0, BEFORE);
    Observe(model, 2, MOVER, OPPONENT);
    Observe(model, 3, OPPONENT, MOVER | OPPONENT);
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::None,
          "occupied destination is deferred to capture slice");
    Check(model.phase.pendingActionToken == 0U,
          "capture-shaped placement exposes no quiet action");
}

void TestAtomicQuietAndFailedActionCorrection() {
    constexpr uint64_t FROM = (1ULL << 52);
    constexpr uint64_t TO = (1ULL << 36);

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model,
        Position(FROM, 0, 0, FROM, 0));
    Observe(model, 1, 0, FROM);
    Observe(model, 2, FROM | TO, TO);
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::QuietMove,
          "one observation can form an atomic quiet plan");
    Check(model.phase.tag == runtime::ChessInteractionPhase::Settling &&
              model.phase.pendingActionToken == 0U,
          "atomic quiet move waits for settle before exposing an action");

    ConfirmSettle(model);
    const uint32_t token = model.phase.pendingActionToken;
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeActionResult(token, false));
    Check(model.phase.tag == runtime::ChessInteractionPhase::Correction,
          "failed quiet action enters correction");
    Observe(model, 3, FROM | TO, FROM);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Idle,
          "restoring acknowledged board clears correction");
}

void TestActionAcknowledgementDoesNotInventCastleFromPartialBoard() {
    constexpr uint8_t FROM = 52U;
    constexpr uint8_t TO = 44U;
    constexpr uint8_t BLACK_PAWN = 11U;
    constexpr uint8_t WHITE_KING = 60U;
    constexpr uint8_t WHITE_ROOK = 63U;
    constexpr uint64_t FROM_MASK = (1ULL << FROM);
    constexpr uint64_t TO_MASK = (1ULL << TO);
    constexpr uint64_t BLACK_MASK = (1ULL << BLACK_PAWN);
    constexpr uint64_t KING_MASK = (1ULL << WHITE_KING);
    constexpr uint64_t ROOK_MASK = (1ULL << WHITE_ROOK);
    constexpr uint64_t BEFORE =
        FROM_MASK | BLACK_MASK | KING_MASK | ROOK_MASK;

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model,
        Position(BEFORE,
                 KING_MASK,
                 ROOK_MASK,
                 FROM_MASK | KING_MASK | ROOK_MASK,
                 BLACK_MASK));

    // Atomic observations are full-board snapshots; older partial fixtures
    // accidentally made the absent king/rook look like a second interaction.
    Observe(model, 1, 0, BEFORE);
    Observe(model, 2, FROM_MASK, BEFORE & ~FROM_MASK);
    Observe(model, 3, TO_MASK, (BEFORE & ~FROM_MASK) | TO_MASK);
    ConfirmSettle(model);
    const uint32_t token = model.phase.pendingActionToken;
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeActionResult(token, true));

    Check(model.phase.tag == runtime::ChessInteractionPhase::Idle,
          "action acknowledgement preserves unrelated full-board occupancy");
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::None,
          "quiet action completion cannot fabricate a castling plan");
}

struct CastleCase {
    uint8_t kingFrom;
    uint8_t kingTo;
    uint8_t rookFrom;
    uint8_t rookTo;
};

void TestAllHumanCastlesUseOnePlan() {
    constexpr CastleCase CASES[] = {
        {60, 62, 63, 61},
        {60, 58, 56, 59},
        {4, 6, 7, 5},
        {4, 2, 0, 3},
    };
    for (const CastleCase& castle : CASES) {
        const uint64_t kingFrom = (1ULL << castle.kingFrom);
        const uint64_t rookFrom = (1ULL << castle.rookFrom);
        const uint64_t before = kingFrom | rookFrom;
        const uint64_t after = (1ULL << castle.kingTo) |
                               (1ULL << castle.rookTo);

        runtime::ChessInteractionModel model;
        runtime::ResetChessInteractionModel(model);
        runtime::ReduceChessInteraction(
            model,
            Position(before, kingFrom, rookFrom, before, 0));
        Observe(model, 1, 0, before);
        Observe(model, 2, before, 0);
        Observe(model, 3, after, after);

        Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::Castling,
              "castle uses common plan kind");
        Check(model.phase.plan.relocationCount == 2U,
              "castle plan has two relocations");
        Check(model.phase.plan.moveFrom == castle.kingFrom &&
                  model.phase.plan.moveTo == castle.kingTo,
              "castle engine identity is always king move");
        Check(model.phase.tag == runtime::ChessInteractionPhase::AwaitingAction &&
                  model.phase.pendingActionToken != 0U,
              "castle exposes exactly one pending apply action");
    }
}

void TestAmbiguousAtomicCastleEntersCorrection() {
    constexpr uint64_t KING = (1ULL << 60U);
    constexpr uint64_t ROOK = (1ULL << 63U);
    constexpr uint64_t OTHER = (1ULL << 55U);
    constexpr uint64_t BEFORE = KING | ROOK | OTHER;

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model,
        Position(BEFORE, KING, ROOK, BEFORE, 0ULL));
    Observe(model, 1, 0ULL, BEFORE);
    Observe(model, 2, BEFORE, 0ULL);

    Check(model.phase.tag == runtime::ChessInteractionPhase::Correction,
          "castle pair plus unrelated removal enters correction");
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::None,
          "ambiguous multi-piece observation cannot retain a castle plan");
}

void TestAtomicQuietProductionProjection() {
    constexpr uint8_t FROM = 52U;
    constexpr uint8_t TO = 36U;
    constexpr uint64_t FROM_MASK = (1ULL << FROM);
    constexpr uint64_t TO_MASK = (1ULL << TO);

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(config);
    stateMachine.Reset();

    runtime::ChessPositionState expected;
    expected.occupiedMask = FROM_MASK;
    expected.moverOccupiedMask = FROM_MASK;
    stateMachine.DebugSetExpectedPositionModel(expected);
    stateMachine.DebugSetPhysicalOccupancyMask(FROM_MASK);

    stateMachine.HandleBoardObservation(
        FROM_MASK | TO_MASK,
        TO_MASK,
        100U,
        1U);
    Check(stateMachine.GetChessInteractionModel().phase.plan.kind ==
              runtime::PhysicalTransitionKind::QuietMove,
          "production atomic quiet observation builds reducer plan");
    Check(stateMachine.IsMoveSettlingActive(),
          "reducer plan projects atomic quiet move into settle compatibility UI");
    Check(!stateMachine.HasHumanIllegal(),
          "atomic quiet move does not inherit square-index ordering fault");

    fxe::ManualMillis::SetNowMs(110U);
    stateMachine.Tick();
    runtime::PhysicalTransitionPlan committed;
    Check(stateMachine.TryClaimPendingInteractionMove(committed),
          "settle completion exposes reducer-owned action");
    Check(committed.moveFrom == FROM && committed.moveTo == TO,
          "action identity comes from quiet plan");
}

void TestFailedEngineActionIsTransactional() {
    constexpr uint8_t FROM = 63U;
    constexpr uint8_t INVALID_TO = 43U;
    constexpr uint64_t FROM_MASK = (1ULL << FROM);
    constexpr uint64_t TO_MASK = (1ULL << INVALID_TO);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_interaction_failed_action.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    (void)std::remove("build/test_interaction_failed_action.json.bak");
    (void)std::remove("build/test_interaction_failed_action.json.tmp");
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "failed-action settings load");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 1U;
    config.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(config);
    stateMachine.Reset();
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "failed-action persistence init");

    char fenBefore[128] = {};
    Check(engine.GetFen(fenBefore, sizeof(fenBefore)),
          "failed-action captures initial FEN");
    const uint64_t before = stateMachine.GetExpectedOccupancyMask();
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    const uint64_t lifted = before & ~FROM_MASK;
    const uint64_t after = lifted | TO_MASK;
    stateMachine.SetValidDestMask(1ULL << 62U);
    stateMachine.HandleBoardObservation(
        FROM_MASK | TO_MASK,
        after,
        1000U,
        1U);
    Check(stateMachine.IsMoveSettlingActive(),
          "reducer owns validation despite stale legacy destination mask");
    Check(!stateMachine.HasHumanIllegal(),
          "legacy legality side channel cannot preempt reducer action result");

    const uint32_t savesBefore = settings.GetSaveCount();
    fxe::ManualMillis::SetNowMs(1001U);
    stateMachine.Tick();
    coordinator.Update();

    char fenAfter[128] = {};
    Check(engine.GetFen(fenAfter, sizeof(fenAfter)),
          "failed-action captures final FEN");
    Check(std::strcmp(fenAfter, fenBefore) == 0,
          "failed engine action leaves logical FEN unchanged");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction,
          "failed engine action remains explicit correction state");
    Check(stateMachine.GetChessInteractionModel()
                  .acknowledged.occupiedMask == before,
          "failed engine action does not advance acknowledged occupancy");
    Check(coordinator.GetUndoHistoryMoveCount() == 0U,
          "failed engine action adds no undo entry");
    Check(settings.GetSaveCount() == savesBefore,
          "failed engine action is not persisted as a move");
}

void TestSettleReliftReplacesPendingDestination() {
    constexpr uint8_t FROM = 52U;
    constexpr uint8_t FIRST_TO = 44U;
    constexpr uint8_t FINAL_TO = 36U;

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_interaction_settle_relift.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    (void)std::remove("build/test_interaction_settle_relift.json.bak");
    (void)std::remove("build/test_interaction_settle_relift.json.tmp");
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "settle relift settings load");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(2000U);
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(config);
    stateMachine.Reset();
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "settle relift persistence init");

    const uint64_t before = stateMachine.GetExpectedOccupancyMask();
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    stateMachine.SetValidDestMask(
        (1ULL << FIRST_TO) | (1ULL << FINAL_TO));

    const uint64_t lifted = before & ~(1ULL << FROM);
    const uint64_t firstPlacement = lifted | (1ULL << FIRST_TO);
    stateMachine.HandleBoardObservation(
        1ULL << FROM, lifted, 2000U, 1U);
    stateMachine.HandleBoardObservation(
        1ULL << FIRST_TO, firstPlacement, 2001U, 2U);
    Check(stateMachine.IsMoveSettlingActive(),
          "settle relift enters settling at first destination");

    stateMachine.HandleBoardObservation(
        1ULL << FIRST_TO, lifted, 2002U, 3U);
    Check(stateMachine.IsHumanPieceInHand(),
          "settle relift returns to placement");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::PieceLifted,
          "settle relift rewinds a quiet move to PieceLifted");
    Check(stateMachine.GetChessInteractionModel().phase.selectedSquare == FROM,
          "settle relift retains the original source");

    const uint64_t finalPlacement = lifted | (1ULL << FINAL_TO);
    stateMachine.HandleBoardObservation(
        1ULL << FINAL_TO, finalPlacement, 2003U, 4U);
    Check(stateMachine.GetChessInteractionModel().phase.plan.moveTo == FINAL_TO,
          "settle relift replaces the reducer destination");
    fxe::ManualMillis::AdvanceMs(config.settleDurationMs);
    stateMachine.Tick();
    coordinator.Update();

    const runtime::ChessInteractionModel& completed =
        stateMachine.GetChessInteractionModel();
    Check(completed.phase.tag == runtime::ChessInteractionPhase::Idle,
          "settle relift acknowledges the replacement move");
    Check(completed.acknowledged.occupiedMask == finalPlacement,
          "settle relift acknowledges only the final destination");
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "settle relift records one move identity");
}

void TestNextPickupDuringSettleSurvivesCommit() {
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E4 = 36U;
    constexpr uint8_t E7 = 12U;
    constexpr uint8_t E5 = 28U;

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_interaction_settle_next_pickup.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    (void)std::remove("build/test_interaction_settle_next_pickup.json.bak");
    (void)std::remove("build/test_interaction_settle_next_pickup.json.tmp");
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "settle next-pickup settings load");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(3000U);
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 100U;
    config.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(config);
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "settle next-pickup persistence init");

    const uint64_t before = stateMachine.GetExpectedOccupancyMask();
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    const uint64_t e2Lifted = before & ~(1ULL << E2);
    const uint64_t e4Placed = e2Lifted | (1ULL << E4);
    const uint64_t e7Lifted = e4Placed & ~(1ULL << E7);

    stateMachine.HandleBoardObservation(1ULL << E2, e2Lifted, 3000U, 1U);
    stateMachine.HandleBoardObservation(1ULL << E4, e4Placed, 3001U, 2U);
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Settling,
          "first move remains reducer-owned while settling");

    stateMachine.HandleBoardObservation(1ULL << E7, e7Lifted, 3002U, 3U);
    Check(!stateMachine.HasHumanIllegal() &&
              !stateMachine.HasCaptureReturn() &&
              !stateMachine.HasCpuIllegal(),
          "next-player pickup during settle never exposes invalid feedback before acknowledgement");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "next-player pickup remains a pending follow-up instead of entering correction before acknowledgement");
    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);
    const runtime::InteractionModelSnapshot preAckSnapshot =
        test_support::BuildInteractionSnapshot(
            stateMachine, derived, &engine);
    modules::led_frame_composer::ComposeInputs composeInputs;
    composeInputs.settings = &settings;
    composeInputs.interactionSnapshot = &preAckSnapshot;
    modules::led_frame_composer::internal::ComposeContext preAckContext;
    Check(modules::led_frame_composer::internal::BuildComposeContext(
              composeInputs, preAckContext),
          "next-player pickup pre-ack render context builds");
    Check(!preAckContext.invalidBoard.IsActive(),
          "recognized next-player pickup never renders invalid-board feedback before acknowledgement");
    coordinator.Update();

    const runtime::TaggedInteractionState& next =
        stateMachine.GetChessInteractionModel().phase;
    Check(next.tag == runtime::ChessInteractionPhase::PieceLifted,
          "action acknowledgement replays the next pickup");
    Check(next.selectedSquare == E7,
          "replayed pickup uses the new side-to-move source");
    Check(stateMachine.IsHumanPieceInHand(),
          "derived piece-in-hand query follows reducer next-pickup state");
    Check(!stateMachine.HasCaptureFirstStaging(),
          "pre-commit side identity cannot leak capture staging");

    const uint64_t e5Placed = e7Lifted | (1ULL << E5);
    stateMachine.HandleBoardObservation(1ULL << E5, e5Placed, 3003U, 4U);
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Settling,
          "replayed pickup can continue to its destination");
    Check(stateMachine.GetChessInteractionModel().phase.plan.moveFrom == E7 &&
              stateMachine.GetChessInteractionModel().phase.plan.moveTo == E5,
          "second move owns the replacement endpoints");
}

void TestSettleReliftReturnedToOriginCancelsMove() {
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E4 = 36U;

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_interaction_settle_return_origin.json";
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    (void)std::remove("build/test_interaction_settle_return_origin.json.bak");
    (void)std::remove("build/test_interaction_settle_return_origin.json.tmp");
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "settle return-origin settings load");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(4000U);
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 100U;
    config.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(config);
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "settle return-origin persistence init");

    char initialFen[128] = {};
    Check(engine.GetFen(initialFen, sizeof(initialFen)),
          "settle return-origin captures initial FEN");
    const uint64_t before = stateMachine.GetExpectedOccupancyMask();
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    const uint64_t lifted = before & ~(1ULL << E2);
    const uint64_t placed = lifted | (1ULL << E4);
    stateMachine.HandleBoardObservation(1ULL << E2, lifted, 4000U, 1U);
    stateMachine.HandleBoardObservation(1ULL << E4, placed, 4001U, 2U);
    Check(stateMachine.IsMoveSettlingActive(),
          "settle return-origin starts from a settling move");

    stateMachine.HandleBoardObservation(1ULL << E4, lifted, 4002U, 3U);
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::PieceLifted &&
              stateMachine.GetChessInteractionModel().phase.selectedSquare == E2,
          "lifting the settling piece preserves its original move source");

    stateMachine.HandleBoardObservation(1ULL << E2, before, 4003U, 4U);
    coordinator.Update();
    char finalFen[128] = {};
    Check(engine.GetFen(finalFen, sizeof(finalFen)),
          "settle return-origin captures final FEN");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle &&
              !stateMachine.IsMoveSettlingActive() &&
              stateMachine.GetExpectedOccupancyMask() == before &&
              coordinator.GetUndoHistoryMoveCount() == 0U &&
              std::strcmp(finalFen, initialFen) == 0,
          "returning the piece to origin restores the start of the same player's move");
}

} // namespace

int main() {
    std::puts("[CHESS_INTERACTION_PLANS] start");
    TestQuietMoveAndCancel();
    TestQuietClassificationGuards();
    TestAtomicQuietAndFailedActionCorrection();
    TestActionAcknowledgementDoesNotInventCastleFromPartialBoard();
    TestAllHumanCastlesUseOnePlan();
    TestAmbiguousAtomicCastleEntersCorrection();
    TestAtomicQuietProductionProjection();
    TestFailedEngineActionIsTransactional();
    TestSettleReliftReplacesPendingDestination();
    TestNextPickupDuringSettleSurvivesCommit();
    TestSettleReliftReturnedToOriginCancelsMove();

    if (g_failures == 0) {
        std::puts("[CHESS_INTERACTION_PLANS] ok");
        return 0;
    }
    std::printf("[CHESS_INTERACTION_PLANS] failures=%d\n", g_failures);
    return 1;
}
