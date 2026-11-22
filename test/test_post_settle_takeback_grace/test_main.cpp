#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"

int RunPostSettleTakebackClockAndCastlingTests();
int RunPostSettleTakebackOrientationTests();
int RunPostSettleTakebackCpuQueueTests();
int RunPostSettleTakebackTerminalTests();

namespace {

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

runtime::GameStateMachineConfig MakeConfig() {
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 10U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    return config;
}

void DispatchObservation(runtime::InteractionDispatcher& dispatcher,
                         uint32_t& sequence,
                         const uint64_t before,
                         const uint64_t after) {
    dispatcher.DispatchBoardObservation(runtime::BoardObservationEvent{
        fxe::Millis(), sequence++, before ^ after, after});
}

void CommitQuietMove(runtime::GameStateMachine& stateMachine,
                     modules::GamePersistenceCoordinator& coordinator,
                     runtime::InteractionDispatcher& dispatcher,
                     uint32_t& sequence,
                     const uint8_t from,
                     const uint8_t to) {
    const uint64_t start = stateMachine.GetExpectedOccupancyMask();
    stateMachine.ApplyPhysicalOccupancySnapshot(start);
    const uint64_t lifted = start & ~(1ULL << from);
    DispatchObservation(dispatcher, sequence, start, lifted);
    const uint64_t placed = lifted | (1ULL << to);
    DispatchObservation(dispatcher, sequence, lifted, placed);
    fxe::ManualMillis::AdvanceMs(11U);
    dispatcher.DispatchTick(fxe::Millis());
    coordinator.Update();
}

void TestImmediatePhysicalInverseBeginsSemanticUndo() {
    constexpr const char* path =
        "build/test_post_settle_takeback_grace.json";
    (void)std::remove(path);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "grace: settings load");
    settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "grace: coordinator load");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence = 1U;

    CommitQuietMove(
        stateMachine, coordinator, dispatcher, sequence, 52U, 44U);
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "grace: forward move committed");
    const uint64_t forward = stateMachine.GetExpectedOccupancyMask();
    Check(stateMachine.GetPhysicalOccupancyMask() == forward,
          "grace: physical board matches committed move");

    const uint64_t lifted = forward & ~(1ULL << 44U);
    DispatchObservation(dispatcher, sequence, forward, lifted);

    Check(stateMachine.IsUndoPromptActive(),
          "grace: destination relift opens the semantic undo prompt");
    Check(coordinator.HasPendingUndoCommand(),
          "grace: inverse engine command waits behind prompt frame");
    Check(stateMachine.GetChessInteractionModel().phase.tag !=
              runtime::ChessInteractionPhase::Correction,
          "grace: immediate takeback never flashes invalid correction");

    coordinator.Update();
    const uint64_t restored = lifted | (1ULL << 52U);
    DispatchObservation(dispatcher, sequence, lifted, restored);
    coordinator.Update();

    char fen[160] = {};
    Check(engine.GetFen(fen, sizeof(fen)), "grace: read rewound FEN");
    Check(std::strcmp(
              fen,
              "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") == 0,
          "grace: physical inverse rewinds the authoritative position");
    Check(coordinator.GetUndoHistoryMoveCount() == 0U,
          "grace: exact takeback consumes one history entry");
    Check(stateMachine.GetMovedSquaresMask() == 0ULL,
          "grace: exact takeback restores the pre-move setup lifecycle");
}

void TestExactDeadlineBelongsToOrdinaryMoveHandling() {
    constexpr const char* path =
        "build/test_post_settle_takeback_deadline.json";
    (void)std::remove(path);
    fxe::ManualMillis::SetNowMs(10000U);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "deadline: settings load");
    settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "deadline: coordinator load");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence = 1U;

    CommitQuietMove(
        stateMachine, coordinator, dispatcher, sequence, 52U, 44U);
    const uint64_t forward = stateMachine.GetExpectedOccupancyMask();
    fxe::ManualMillis::AdvanceMs(3000U);
    DispatchObservation(
        dispatcher, sequence, forward,
        forward & ~(1ULL << 44U));

    Check(!stateMachine.IsUndoPromptActive() &&
              !coordinator.HasPendingUndoCommand(),
          "deadline: exact grace boundary does not auto-undo");
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "deadline: ordinary input leaves history untouched");
}

void TestZeroSecondsDisablesAutomaticTakeback() {
    constexpr const char* path =
        "build/test_post_settle_takeback_disabled.json";
    (void)std::remove(path);
    fxe::ManualMillis::SetNowMs(20000U);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "disabled: settings load");
    settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 0U;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "disabled: coordinator load");
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence = 1U;

    CommitQuietMove(
        stateMachine, coordinator, dispatcher, sequence, 52U, 44U);
    const uint64_t forward = stateMachine.GetExpectedOccupancyMask();
    DispatchObservation(
        dispatcher, sequence, forward,
        forward & ~(1ULL << 44U));

    Check(!stateMachine.IsUndoPromptActive() &&
              !coordinator.HasPendingUndoCommand(),
          "disabled: immediate inverse remains ordinary board input");
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "disabled: setting zero leaves history untouched");
}

void TestTargetFirstCaptureWaitsForInverseEvidence() {
    constexpr const char* path =
        "build/test_post_settle_takeback_ambiguous.json";
    constexpr const char* fen =
        "k7/8/8/8/3p4/8/4P3/7K w - - 0 1";
    (void)std::remove(path);
    fxe::ManualMillis::SetNowMs(30000U);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "ambiguous: settings load");
    settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "ambiguous: coordinator load");
    Check(coordinator.LoadExternalFen(fen), "ambiguous: fixture FEN load");
    stateMachine.ApplyPhysicalOccupancySnapshot(
        stateMachine.GetExpectedOccupancyMask());
    stateMachine.Tick();
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;

    CommitQuietMove(
        stateMachine, coordinator, dispatcher, sequence, 52U, 44U);
    const uint64_t forward = stateMachine.GetExpectedOccupancyMask();
    const uint64_t targetLifted = forward & ~(1ULL << 44U);
    DispatchObservation(
        dispatcher, sequence, forward, targetLifted);

    Check(!stateMachine.IsUndoPromptActive() &&
              stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::CaptureStaged,
          "ambiguous: legal target-first capture remains discoverable");
    Check(!coordinator.HasPendingUndoCommand(),
          "ambiguous: target lift alone does not guess takeback intent");

    const uint64_t sourceRestored = targetLifted | (1ULL << 52U);
    DispatchObservation(
        dispatcher, sequence, targetLifted, sourceRestored);
    Check(stateMachine.IsUndoPromptActive() &&
              coordinator.HasPendingUndoCommand(),
          "ambiguous: old-source placement resolves to takeback");
    coordinator.Update();
    Check(coordinator.GetUndoHistoryMoveCount() == 0U,
          "ambiguous: resolved takeback rewinds history");
}

void TestTargetFirstCaptureWinsWhenMoverIsLifted() {
    constexpr const char* path =
        "build/test_post_settle_takeback_new_capture.json";
    constexpr const char* fen =
        "k7/8/8/8/3p4/8/4P3/7K w - - 0 1";
    (void)std::remove(path);
    fxe::ManualMillis::SetNowMs(40000U);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "new capture: settings load");
    settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "new capture: coordinator load");
    Check(coordinator.LoadExternalFen(fen), "new capture: fixture FEN load");
    stateMachine.ApplyPhysicalOccupancySnapshot(
        stateMachine.GetExpectedOccupancyMask());
    stateMachine.Tick();
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;

    CommitQuietMove(
        stateMachine, coordinator, dispatcher, sequence, 52U, 44U);
    const uint64_t forward = stateMachine.GetExpectedOccupancyMask();
    const uint64_t targetLifted = forward & ~(1ULL << 44U);
    DispatchObservation(
        dispatcher, sequence, forward, targetLifted);
    const uint64_t moverLifted = targetLifted & ~(1ULL << 35U);
    DispatchObservation(
        dispatcher, sequence, targetLifted, moverLifted);

    const runtime::TaggedInteractionState& capture =
        stateMachine.GetChessInteractionModel().phase;
    Check(!stateMachine.IsUndoPromptActive() &&
              !coordinator.HasPendingUndoCommand(),
          "new capture: current mover pickup revokes grace candidate");
    Check(capture.plan.source ==
                  runtime::PhysicalTransitionSource::Human &&
              capture.plan.kind ==
                  runtime::PhysicalTransitionKind::Capture &&
              capture.plan.moveFrom == 35U && capture.plan.moveTo == 44U,
          "new capture: ordinary target-first capture keeps ownership");

    const uint64_t placed = moverLifted | (1ULL << 44U);
    DispatchObservation(dispatcher, sequence, moverLifted, placed);
    fxe::ManualMillis::AdvanceMs(11U);
    dispatcher.DispatchTick(fxe::Millis());
    coordinator.Update();
    Check(coordinator.GetUndoHistoryMoveCount() == 2U,
          "new capture: completed capture appends instead of rewinding");
}

void TestTargetFirstEnPassantRemainsOrdinaryCapture() {
    constexpr const char* path =
        "build/test_post_settle_takeback_en_passant.json";
    constexpr const char* fen =
        "k7/8/8/8/3p4/8/4P3/7K w - - 0 1";
    (void)std::remove(path);
    fxe::ManualMillis::SetNowMs(45000U);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "en passant: settings load");
    settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(),
          "en passant: coordinator load");
    Check(coordinator.LoadExternalFen(fen),
          "en passant: fixture FEN load");
    stateMachine.ApplyPhysicalOccupancySnapshot(
        stateMachine.GetExpectedOccupancyMask());
    stateMachine.Tick();
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;

    CommitQuietMove(
        stateMachine, coordinator, dispatcher, sequence, 52U, 36U);
    const uint64_t forward = stateMachine.GetExpectedOccupancyMask();
    const uint64_t capturedPawnLifted = forward & ~(1ULL << 36U);
    DispatchObservation(
        dispatcher, sequence, forward, capturedPawnLifted);

    Check(!stateMachine.IsUndoPromptActive() &&
              !coordinator.HasPendingUndoCommand(),
          "en passant: captured-pawn-first input does not guess takeback");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::CaptureStaged,
          "en passant: captured-pawn-first input remains discoverable");

    const uint64_t moverLifted =
        capturedPawnLifted & ~(1ULL << 35U);
    DispatchObservation(
        dispatcher, sequence, capturedPawnLifted, moverLifted);
    const runtime::TaggedInteractionState& capture =
        stateMachine.GetChessInteractionModel().phase;
    Check(capture.plan.source ==
                  runtime::PhysicalTransitionSource::Human &&
              capture.plan.kind ==
                  runtime::PhysicalTransitionKind::EnPassant &&
              capture.plan.moveFrom == 35U && capture.plan.moveTo == 44U,
          "en passant: mover pickup resolves the ordinary compound capture");
}

void TestCaptureTakebackGuidesCapturedPieceRestoration() {
    constexpr const char* path =
        "build/test_post_settle_takeback_capture.json";
    constexpr const char* fen =
        "k7/8/8/8/8/3p4/4P3/7K w - - 0 1";
    (void)std::remove(path);
    fxe::ManualMillis::SetNowMs(50000U);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "capture: settings load");
    settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "capture: coordinator load");
    Check(coordinator.LoadExternalFen(fen), "capture: fixture FEN load");
    char normalizedFen[160] = {};
    Check(engine.GetFen(normalizedFen, sizeof(normalizedFen)),
          "capture: normalized fixture FEN read");
    stateMachine.ApplyPhysicalOccupancySnapshot(
        stateMachine.GetExpectedOccupancyMask());
    stateMachine.Tick();
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;

    const uint64_t start = stateMachine.GetExpectedOccupancyMask();
    const uint64_t bothLifted =
        start & ~(1ULL << 52U) & ~(1ULL << 43U);
    DispatchObservation(
        dispatcher, sequence, start, bothLifted);
    const uint64_t captured = bothLifted | (1ULL << 43U);
    DispatchObservation(
        dispatcher, sequence, bothLifted, captured);
    fxe::ManualMillis::AdvanceMs(11U);
    dispatcher.DispatchTick(fxe::Millis());
    coordinator.Update();
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "capture: forward capture committed");

    const uint64_t moverLifted = captured & ~(1ULL << 43U);
    DispatchObservation(
        dispatcher, sequence, captured, moverLifted);
    Check(stateMachine.IsUndoPromptActive(),
          "capture: destination relift opens takeback prompt");
    coordinator.Update();

    const uint64_t moverRestored = moverLifted | (1ULL << 52U);
    DispatchObservation(
        dispatcher, sequence, moverLifted, moverRestored);
    const runtime::TaggedInteractionState& awaitingRestore =
        stateMachine.GetChessInteractionModel().phase;
    Check(awaitingRestore.requiredAction ==
              runtime::RequiredPhysicalAction::RestoreCapture,
          "capture: board gently asks for the captured piece last");
    const uint64_t fullyRestored = moverRestored | (1ULL << 43U);
    DispatchObservation(
        dispatcher, sequence, moverRestored, fullyRestored);
    coordinator.Update();

    char restoredFen[160] = {};
    Check(engine.GetFen(restoredFen, sizeof(restoredFen)) &&
              std::strcmp(restoredFen, normalizedFen) == 0,
          "capture: exact inverse restores captured piece and FEN");
    Check(coordinator.GetUndoHistoryMoveCount() == 0U,
          "capture: exact inverse consumes capture history");
}

} // namespace

int main() {
    std::puts("[POST_SETTLE_TAKEBACK_GRACE] start");
    fxe::ScopedMillisProviderOverride millisGuard(
        &fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);

    TestImmediatePhysicalInverseBeginsSemanticUndo();
    TestExactDeadlineBelongsToOrdinaryMoveHandling();
    TestZeroSecondsDisablesAutomaticTakeback();
    TestTargetFirstCaptureWaitsForInverseEvidence();
    TestTargetFirstCaptureWinsWhenMoverIsLifted();
    TestTargetFirstEnPassantRemainsOrdinaryCapture();
    TestCaptureTakebackGuidesCapturedPieceRestoration();
    g_failures += RunPostSettleTakebackClockAndCastlingTests();
    g_failures += RunPostSettleTakebackOrientationTests();
    g_failures += RunPostSettleTakebackCpuQueueTests();
    g_failures += RunPostSettleTakebackTerminalTests();

    if (g_failures == 0) {
        std::puts("[POST_SETTLE_TAKEBACK_GRACE] ok");
        return 0;
    }
    std::printf("[POST_SETTLE_TAKEBACK_GRACE] failures=%d\n", g_failures);
    return 1;
}
