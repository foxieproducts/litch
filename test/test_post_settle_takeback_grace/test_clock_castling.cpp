#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"

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
    const uint64_t lifted = start & ~(1ULL << from);
    DispatchObservation(dispatcher, sequence, start, lifted);
    const uint64_t placed = lifted | (1ULL << to);
    DispatchObservation(dispatcher, sequence, lifted, placed);
    fxe::ManualMillis::AdvanceMs(11U);
    dispatcher.DispatchTick(fxe::Millis());
    coordinator.Update();
}

void TestSecondMoveRestoresItsImmediatePreMoveMask() {
    constexpr const char* path =
        "build/test_post_settle_takeback_second_mask.json";
    (void)std::remove(path);
    fxe::ManualMillis::SetNowMs(54000U);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "second mask: settings load");
    settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(),
          "second mask: coordinator load");
    stateMachine.ApplyPhysicalOccupancySnapshot(
        stateMachine.GetExpectedOccupancyMask());
    stateMachine.Tick();
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;

    CommitQuietMove(
        stateMachine, coordinator, dispatcher, sequence, 52U, 44U);
    const uint64_t firstMoveMask = (1ULL << 52U) | (1ULL << 44U);
    Check(stateMachine.GetMovedSquaresMask() == firstMoveMask,
          "second mask: first move establishes lifecycle history");
    CommitQuietMove(
        stateMachine, coordinator, dispatcher, sequence, 12U, 20U);

    const uint64_t forward = stateMachine.GetExpectedOccupancyMask();
    const uint64_t lifted = forward & ~(1ULL << 20U);
    DispatchObservation(dispatcher, sequence, forward, lifted);
    coordinator.Update();
    const uint64_t restored = lifted | (1ULL << 12U);
    DispatchObservation(dispatcher, sequence, lifted, restored);
    coordinator.Update();
    Check(stateMachine.GetMovedSquaresMask() == firstMoveMask,
          "second mask: grace restores the immediate pre-second-move mask");
}

void TestP2KingsideCastleCanBeTakenBackAtomically() {
    constexpr const char* path =
        "build/test_post_settle_takeback_p2_castle.json";
    constexpr const char* fen =
        "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1";
    (void)std::remove(path);
    fxe::ManualMillis::SetNowMs(55000U);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "P2 castle: settings load");
    settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(),
          "P2 castle: coordinator load");
    Check(coordinator.LoadExternalFen(fen),
          "P2 castle: fixture FEN load");
    char normalizedFen[160] = {};
    Check(engine.GetFen(normalizedFen, sizeof(normalizedFen)),
          "P2 castle: normalized fixture FEN read");
    const uint64_t priorMovedSquares =
        (1ULL << 4U) | (1ULL << 10U);
    stateMachine.SetMovedSquaresMask(priorMovedSquares);
    stateMachine.ApplyPhysicalOccupancySnapshot(
        stateMachine.GetExpectedOccupancyMask());
    stateMachine.Tick();
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;

    const uint64_t start = stateMachine.GetExpectedOccupancyMask();
    const uint64_t bothLifted =
        start & ~(1ULL << 4U) & ~(1ULL << 7U);
    DispatchObservation(dispatcher, sequence, start, bothLifted);
    const uint64_t bothPlaced =
        bothLifted | (1ULL << 6U) | (1ULL << 5U);
    DispatchObservation(dispatcher, sequence, bothLifted, bothPlaced);
    fxe::ManualMillis::AdvanceMs(11U);
    dispatcher.DispatchTick(fxe::Millis());
    coordinator.Update();
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "P2 castle: simultaneous king and rook pickup commits castle");

    const uint64_t inverseLifted =
        bothPlaced & ~(1ULL << 6U) & ~(1ULL << 5U);
    DispatchObservation(
        dispatcher, sequence, bothPlaced, inverseLifted);
    Check(stateMachine.IsUndoPromptActive() &&
              coordinator.HasPendingUndoCommand() &&
              !stateMachine.HasHumanIllegal(),
          "P2 castle: simultaneous inverse starts one semantic takeback");
    coordinator.Update();
    const uint64_t restored =
        inverseLifted | (1ULL << 4U) | (1ULL << 7U);
    DispatchObservation(dispatcher, sequence, inverseLifted, restored);
    coordinator.Update();

    char restoredFen[160] = {};
    Check(engine.GetFen(restoredFen, sizeof(restoredFen)) &&
              std::strcmp(restoredFen, normalizedFen) == 0 &&
              coordinator.GetUndoHistoryMoveCount() == 0U,
          "P2 castle: one inverse restores both pieces and engine history");
    Check(stateMachine.GetMovedSquaresMask() == priorMovedSquares,
          "P2 castle: inverse restores the pre-castle lifecycle mask");
}

void TestGraceTakebackRefundsIncrementAndOpponentTime() {
    constexpr const char* path =
        "build/test_post_settle_takeback_clock.json";
    (void)std::remove(path);
    fxe::ManualMillis::SetNowMs(60000U);

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = path;
    settingsConfig.deferredSaveDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(MakeConfig());
    modules::SettingsStore settings(settingsConfig);
    test_support::BoundedHintEngine engine;
    Check(settings.Load(), "clock: settings load");
    settings.Doc()["TAKEBACK_GRACE_SECONDS"] = 3U;
    settings.Doc()["TIMER"] = 2U;
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "clock: coordinator load");
    stateMachine.ApplyPhysicalOccupancySnapshot(
        stateMachine.GetExpectedOccupancyMask());
    stateMachine.Tick();
    coordinator.Update();
    runtime::InteractionDispatcher dispatcher(stateMachine, coordinator);
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;

    fxe::ManualMillis::AdvanceMs(750U);
    coordinator.Update();
    const uint64_t start = stateMachine.GetExpectedOccupancyMask();
    const uint64_t lifted = start & ~(1ULL << 52U);
    DispatchObservation(dispatcher, sequence, start, lifted);
    coordinator.Update();
    const uint64_t placed = lifted | (1ULL << 44U);
    DispatchObservation(dispatcher, sequence, lifted, placed);
    coordinator.Update();
    fxe::ManualMillis::AdvanceMs(11U);
    dispatcher.DispatchTick(fxe::Millis());
    coordinator.Update();
    // The commit cadence still carries the pre-ack pause. The next normal
    // cadence sees the acknowledged/matched board and starts the opponent.
    coordinator.Update();

    const runtime::GameClock& clock = coordinator.GetGameClock();
    const uint32_t forwardWhite = clock.GetRemainingMs(
        runtime::ClockSide::White);
    const uint32_t forwardBlack = clock.GetRemainingMs(
        runtime::ClockSide::Black);
    Check(clock.GetActiveSide() == runtime::ClockSide::Black,
          "clock: forward move switches to opponent");

    fxe::ManualMillis::AdvanceMs(500U);
    coordinator.Update();
    Check(clock.GetRemainingMs(runtime::ClockSide::Black) <
              forwardBlack,
          "clock: opponent time runs before takeback");

    const uint64_t forward = stateMachine.GetExpectedOccupancyMask();
    const uint64_t takebackLifted = forward & ~(1ULL << 44U);
    DispatchObservation(
        dispatcher, sequence, forward, takebackLifted);
    coordinator.Update();

    Check(clock.GetActiveSide() == runtime::ClockSide::White,
          "clock: takeback restores the original mover");
    Check(clock.GetRemainingMs(runtime::ClockSide::White) + 2000U ==
              forwardWhite,
          "clock: takeback removes the awarded increment");
    Check(clock.GetRemainingMs(runtime::ClockSide::Black) ==
              forwardBlack,
          "clock: takeback refunds opponent grace-window time");

    Check(coordinator.IsCancelUndoAvailable() &&
              coordinator.RequestUndo(1U),
          "clock: ordinary X cancel path remains available");
    coordinator.Update();
    char canceledFen[160] = {};
    Check(engine.GetFen(canceledFen, sizeof(canceledFen)) &&
              std::strstr(canceledFen, "4P3") != nullptr &&
              coordinator.GetUndoHistoryMoveCount() == 1U,
          "clock: cancel restores the settled forward move");
    Check(clock.GetActiveSide() == runtime::ClockSide::Black &&
              clock.GetRemainingMs(runtime::ClockSide::White) ==
                  forwardWhite &&
              clock.GetRemainingMs(runtime::ClockSide::Black) ==
                  forwardBlack,
          "clock: cancel restores the exact post-move clock snapshot");
}

} // namespace

int RunPostSettleTakebackClockAndCastlingTests() {
    TestSecondMoveRestoresItsImmediatePreMoveMask();
    TestP2KingsideCastleCanBeTakenBackAtomically();
    TestGraceTakebackRefundsIncrementAndOpponentTime();
    return g_failures;
}
