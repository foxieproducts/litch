#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

bool FenToOccupancy(const char* fen, uint64_t& outMask) {
    outMask = 0;
    uint8_t rank = 0;
    uint8_t file = 0;
    for (const char* cursor = fen; cursor != nullptr && *cursor != '\0';
         ++cursor) {
        const char value = *cursor;
        if (value == ' ') {
            break;
        }
        if (value == '/') {
            ++rank;
            file = 0;
            continue;
        }
        if (value >= '1' && value <= '8') {
            file = static_cast<uint8_t>(file + value - '0');
            continue;
        }
        if (rank >= 8U || file >= 8U) {
            return false;
        }
        outMask |= (1ULL << static_cast<uint8_t>(rank * 8U + file));
        ++file;
    }
    return rank == 7U && file == 8U;
}

struct CastleCase {
    const char* settingsPath;
    const char* fen;
    uint8_t kingFrom;
    uint8_t kingTo;
    uint8_t rookFrom;
    uint8_t rookTo;
};

void RunCastleCase(const CastleCase& castle,
                   const uint32_t sequenceBase) {
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = castle.settingsPath;
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    char backupPath[128] = {};
    char temporaryPath[128] = {};
    std::snprintf(backupPath, sizeof(backupPath), "%s.bak", castle.settingsPath);
    std::snprintf(temporaryPath, sizeof(temporaryPath), "%s.tmp", castle.settingsPath);
    (void)std::remove(backupPath);
    (void)std::remove(temporaryPath);
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "castle matrix settings load");

    runtime::GameStateMachineConfig stateConfig;
    stateConfig.settleDurationMs = 20U;
    stateConfig.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(), "castle matrix persistence init");

    uint64_t before = 0;
    Check(FenToOccupancy(castle.fen, before),
          "castle matrix parses seed occupancy");
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    Check(coordinator.LoadExternalFen(castle.fen),
          "castle matrix loads seed FEN");
    const uint32_t savesBefore = settings.GetSaveCount();

    const uint64_t pickup = (1ULL << castle.kingFrom) |
                            (1ULL << castle.rookFrom);
    const uint64_t placement = (1ULL << castle.kingTo) |
                               (1ULL << castle.rookTo);
    const uint64_t lifted = before & ~pickup;
    runtime::AppEvent pickupEvent = runtime::AppEvent::MakeBoardObservation(
        sequenceBase,
        pickup,
        lifted);
    pickupEvent.seq = sequenceBase;
    runtime::DispatchAppEventToStateMachine(stateMachine, pickupEvent);
    runtime::AppEvent placementEvent = runtime::AppEvent::MakeBoardObservation(
        sequenceBase + 1U,
        placement,
        lifted | placement);
    placementEvent.seq = sequenceBase + 1U;
    runtime::DispatchAppEventToStateMachine(stateMachine, placementEvent);

    const runtime::ChessInteractionModel& pending =
        stateMachine.GetChessInteractionModel();
    Check(pending.phase.tag == runtime::ChessInteractionPhase::AwaitingAction,
          "castle matrix waits at transactional action boundary");
    Check(pending.phase.plan.kind ==
              runtime::PhysicalTransitionKind::Castling,
          "castle matrix uses one castling plan");
    Check(pending.phase.plan.moveFrom == castle.kingFrom &&
              pending.phase.plan.moveTo == castle.kingTo,
          "castle matrix action identity is the king move");
    Check(pending.phase.plan.relocationCount == 2U,
          "castle matrix plan retains both relocations");
    Check(pending.acknowledged.occupiedMask == before,
          "castle matrix logical position waits for engine success");

    coordinator.Update();
    char fenAfter[128] = {};
    Check(engine.GetFen(fenAfter, sizeof(fenAfter)),
          "castle matrix reads final FEN");
    uint64_t actual = 0;
    Check(FenToOccupancy(fenAfter, actual),
          "castle matrix parses final occupancy");
    const uint64_t expected = lifted | placement;
    Check(actual == expected,
          "castle matrix engine reaches planned occupancy");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle,
          "castle matrix acknowledgement completes reducer plan");
    Check(stateMachine.GetChessInteractionModel()
                  .acknowledged.occupiedMask == expected,
          "castle matrix acknowledgement advances logical occupancy");
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "castle matrix adds one acknowledged undo move");
    Check(settings.GetSaveCount() == savesBefore,
          "castle matrix queues the acknowledged move behind the debounce");
    Check(settings.SaveNow(),
          "castle matrix reaches storage after debounce simulation");
    Check(settings.GetSaveCount() == savesBefore + 1U,
          "castle matrix completes exactly one deferred save");
}

void RunAtomicKingRelocationCase(const CastleCase& castle,
                                 const uint32_t sequenceBase,
                                 const bool rookAlreadyHeld = false) {
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = castle.settingsPath;
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    char backupPath[128] = {};
    char temporaryPath[128] = {};
    std::snprintf(backupPath, sizeof(backupPath), "%s.bak", castle.settingsPath);
    std::snprintf(temporaryPath, sizeof(temporaryPath), "%s.tmp", castle.settingsPath);
    (void)std::remove(backupPath);
    (void)std::remove(temporaryPath);
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "atomic king relocation settings load");

    runtime::GameStateMachineConfig stateConfig;
    stateConfig.settleDurationMs = 20U;
    stateConfig.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(),
          "atomic king relocation persistence init");

    uint64_t before = 0ULL;
    Check(FenToOccupancy(castle.fen, before),
          "atomic king relocation parses seed occupancy");
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    Check(coordinator.LoadExternalFen(castle.fen),
          "atomic king relocation loads seed FEN");

    const uint64_t rookPickup = rookAlreadyHeld
        ? (1ULL << castle.rookFrom) : 0ULL;
    const uint64_t kingChange = (1ULL << castle.kingFrom) |
                                (1ULL << castle.kingTo) | rookPickup;
    const uint64_t kingRelocated =
        (before & ~((1ULL << castle.kingFrom) | rookPickup)) |
        (1ULL << castle.kingTo);
    runtime::AppEvent kingEvent = runtime::AppEvent::MakeBoardObservation(
        fxe::Millis(),
        kingChange,
        kingRelocated);
    kingEvent.seq = sequenceBase;
    runtime::DispatchAppEventToStateMachine(stateMachine, kingEvent);

    const runtime::ChessInteractionModel& partial =
        stateMachine.GetChessInteractionModel();
    Check(partial.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan,
          "atomic king relocation keeps the composite castle in progress");
    Check(partial.phase.plan.kind ==
              runtime::PhysicalTransitionKind::Castling,
          "atomic king relocation is inferred as castling, not a quiet move");
    Check(partial.phase.requiredAction ==
              (rookAlreadyHeld
                   ? runtime::RequiredPhysicalAction::PlaceSecondary
                   : runtime::RequiredPhysicalAction::LiftSecondary),
          "atomic king relocation prompts for the remaining rook action");
    Check(stateMachine.IsCastlingRookPromptActive(),
          "atomic king relocation exposes the rook follow-up prompt");
    Check(!stateMachine.HasHumanIllegal(),
          "atomic king relocation does not latch invalid feedback");
    Check(!stateMachine.HasCommittedMovePending(),
          "atomic king relocation cannot commit before the rook moves");
    Check(partial.acknowledged.occupiedMask == before &&
              coordinator.GetUndoHistoryMoveCount() == 0U,
          "atomic king relocation cannot acknowledge the incomplete castle");

    const uint64_t rookLifted =
        kingRelocated & ~(1ULL << castle.rookFrom);
    if (!rookAlreadyHeld) {
        runtime::AppEvent rookLiftEvent = runtime::AppEvent::MakeBoardObservation(
            fxe::Millis(),
            1ULL << castle.rookFrom,
            rookLifted);
        rookLiftEvent.seq = sequenceBase + 1U;
        runtime::DispatchAppEventToStateMachine(stateMachine, rookLiftEvent);
    }

    for (uint32_t elapsed = 0U; elapsed < 250U; elapsed += 10U) {
        fxe::ManualMillis::AdvanceMs(10U);
        stateMachine.Tick();
        coordinator.Update();
    }
    Check(coordinator.GetUndoHistoryMoveCount() == 0U &&
              !stateMachine.HasCommittedMovePending(),
          "holding the castle rook cannot expose or apply an engine action");
    const uint64_t complete = rookLifted | (1ULL << castle.rookTo);
    runtime::AppEvent rookPlaceEvent = runtime::AppEvent::MakeBoardObservation(
        fxe::Millis(),
        1ULL << castle.rookTo,
        complete);
    rookPlaceEvent.seq = sequenceBase + 2U;
    runtime::DispatchAppEventToStateMachine(stateMachine, rookPlaceEvent);
    fxe::ManualMillis::AdvanceMs(stateConfig.settleDurationMs);
    stateMachine.Tick();
    coordinator.Update();

    char fenAfter[128] = {};
    uint64_t actual = 0ULL;
    Check(engine.GetFen(fenAfter, sizeof(fenAfter)) &&
              FenToOccupancy(fenAfter, actual),
          "atomic king relocation reads final FEN");
    Check(actual == complete,
          "atomic king relocation completes the intended castle");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle,
          "atomic king relocation returns the reducer to idle after acknowledgement");
    Check(!stateMachine.HasHumanIllegal(),
          "atomic king relocation completes without invalid feedback");
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "atomic king relocation acknowledges one composite history move");
    coordinator.Update();
    Check(coordinator.GetUndoHistoryMoveCount() == 1U,
          "atomic king relocation cannot acknowledge the castle twice");
}

void RunDestinationOrderCase(const CastleCase& castle,
                             const uint32_t sequenceBase,
                             const bool rookPickupFirst,
                             const bool rookDestinationFirst,
                             const bool reliftKingBeforeRook) {
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = castle.settingsPath;
    settingsConfig.deferredSaveDelayMs = 999999U;
    (void)std::remove(settingsConfig.path);
    char backupPath[128] = {};
    char temporaryPath[128] = {};
    std::snprintf(backupPath, sizeof(backupPath), "%s.bak", castle.settingsPath);
    std::snprintf(temporaryPath, sizeof(temporaryPath), "%s.tmp", castle.settingsPath);
    (void)std::remove(backupPath);
    (void)std::remove(temporaryPath);
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "ordered castle settings load");

    runtime::GameStateMachineConfig stateConfig;
    stateConfig.settleDurationMs = 20U;
    stateConfig.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine stateMachine(stateConfig);
    stateMachine.Reset();
    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(
        settings, engine, stateMachine);
    Check(coordinator.LoadOrInitialize(),
          "ordered castle persistence init");

    uint64_t before = 0ULL;
    Check(FenToOccupancy(castle.fen, before),
          "ordered castle parses seed occupancy");
    stateMachine.DebugSetPhysicalOccupancyMask(before);
    Check(coordinator.LoadExternalFen(castle.fen),
          "ordered castle loads seed FEN");

    const uint64_t sourcePair = (1ULL << castle.kingFrom) |
                                (1ULL << castle.rookFrom);
    const uint64_t lifted = before & ~sourcePair;
    uint32_t firstPlacementSequence = sequenceBase + 1U;
    if (rookPickupFirst) {
        const uint64_t rookLifted = before & ~(1ULL << castle.rookFrom);
        runtime::AppEvent rookPickup = runtime::AppEvent::MakeBoardObservation(
            sequenceBase, 1ULL << castle.rookFrom, rookLifted);
        rookPickup.seq = sequenceBase;
        runtime::DispatchAppEventToStateMachine(stateMachine, rookPickup);
        coordinator.UpdateLegalDestinationsMask();
        Check(stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::PieceLifted &&
                  stateMachine.GetChessInteractionModel().phase.selectedSquare ==
                      castle.rookFrom,
              "rook-first matrix begins as an ordinary rook pickup");
        Check((stateMachine.GetValidDestMask() &
               (1ULL << castle.rookTo)) != 0ULL,
              "rook-first matrix initially exposes ordinary rook moves");
        runtime::AppEvent kingPickup = runtime::AppEvent::MakeBoardObservation(
            sequenceBase + 1U, 1ULL << castle.kingFrom, lifted);
        kingPickup.seq = sequenceBase + 1U;
        runtime::DispatchAppEventToStateMachine(stateMachine, kingPickup);
        firstPlacementSequence = sequenceBase + 2U;
    } else {
        runtime::AppEvent pickupEvent = runtime::AppEvent::MakeBoardObservation(
            sequenceBase, sourcePair, lifted);
        pickupEvent.seq = sequenceBase;
        runtime::DispatchAppEventToStateMachine(stateMachine, pickupEvent);
    }
    stateMachine.SetValidDestMask(1ULL << castle.kingTo);

    const uint8_t firstTo = rookDestinationFirst
                                ? castle.rookTo
                                : castle.kingTo;
    const uint8_t secondTo = rookDestinationFirst
                                 ? castle.kingTo
                                 : castle.rookTo;
    const uint64_t firstPlaced = lifted | (1ULL << firstTo);
    runtime::AppEvent firstPlaceEvent = runtime::AppEvent::MakeBoardObservation(
        firstPlacementSequence,
        1ULL << firstTo,
        firstPlaced);
    firstPlaceEvent.seq = firstPlacementSequence;
    runtime::DispatchAppEventToStateMachine(stateMachine, firstPlaceEvent);

    const runtime::ChessInteractionModel& partial =
        stateMachine.GetChessInteractionModel();
    Check(partial.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan,
          "first castle placement keeps composite move in progress");
    Check(partial.phase.requiredAction ==
              (rookDestinationFirst
                   ? runtime::RequiredPhysicalAction::PlacePrimary
                   : runtime::RequiredPhysicalAction::PlaceSecondary),
          "first castle placement prompts for the remaining piece");
    const uint8_t remainingFrom = rookDestinationFirst
                                      ? castle.kingFrom
                                      : castle.rookFrom;
    uint8_t activePieceSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    Check(stateMachine.IsHumanPieceInHand() &&
              stateMachine.TryGetHumanPieceInHandSquare(activePieceSquare) &&
              activePieceSquare == remainingFrom,
          "first castle placement projects the remaining piece prompt");
    Check(!stateMachine.HasHumanIllegal(),
          "first castle placement does not trigger invalid feedback");

    uint32_t secondPlacementSequence = firstPlacementSequence + 1U;
    uint64_t beforeSecondPlacement = firstPlaced;
    if (reliftKingBeforeRook) {
        const uint64_t kingRelifted =
            firstPlaced & ~(1ULL << castle.kingTo);
        runtime::AppEvent reliftEvent =
            runtime::AppEvent::MakeBoardObservation(
                sequenceBase + 2U,
                1ULL << castle.kingTo,
                kingRelifted);
        reliftEvent.seq = secondPlacementSequence;
        runtime::DispatchAppEventToStateMachine(stateMachine, reliftEvent);
        beforeSecondPlacement = kingRelifted;
        ++secondPlacementSequence;
    }
    const uint64_t secondPlaced =
        beforeSecondPlacement | (1ULL << secondTo);
    runtime::AppEvent secondPlaceEvent = runtime::AppEvent::MakeBoardObservation(
        secondPlacementSequence,
        1ULL << secondTo,
        secondPlaced);
    secondPlaceEvent.seq = secondPlacementSequence;
    runtime::DispatchAppEventToStateMachine(stateMachine, secondPlaceEvent);
    if (reliftKingBeforeRook) {
        Check(stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan,
              "relifted castle destination keeps the plan in progress");
        Check(stateMachine.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary,
              "relifted king destination is prompted again after rook placement");
        Check(!stateMachine.HasCommittedMovePending(),
              "castle cannot expose an action while the king is airborne");
        runtime::AppEvent replaceKingEvent =
            runtime::AppEvent::MakeBoardObservation(
                secondPlacementSequence + 1U,
                1ULL << castle.kingTo,
                secondPlaced | (1ULL << castle.kingTo));
        replaceKingEvent.seq = secondPlacementSequence + 1U;
        runtime::DispatchAppEventToStateMachine(stateMachine, replaceKingEvent);
    }
    const uint64_t complete = secondPlaced | (1ULL << castle.kingTo);
    coordinator.Update();

    char fenAfter[128] = {};
    uint64_t actual = 0ULL;
    Check(engine.GetFen(fenAfter, sizeof(fenAfter)) &&
              FenToOccupancy(fenAfter, actual),
          "ordered castle reads final FEN");
    Check(actual == complete,
          "ordered castle completes the intended move");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle,
          "ordered castle returns the reducer to idle");
    Check(!stateMachine.HasHumanIllegal(),
          "ordered castle completes without invalid feedback");
}

} // namespace

int main() {
    std::puts("[CHESS_INTERACTION_CASTLE_MATRIX] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    constexpr CastleCase CASES[] = {
        {"build/castle_matrix_wk.json",
         "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", 60, 62, 63, 61},
        {"build/castle_matrix_wq.json",
         "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", 60, 58, 56, 59},
        {"build/castle_matrix_bk.json",
         "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1", 4, 6, 7, 5},
        {"build/castle_matrix_bq.json",
         "r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1", 4, 2, 0, 3},
    };
    uint32_t sequence = 1U;
    for (const CastleCase& castle : CASES) {
        RunCastleCase(castle, sequence);
        sequence += 2U;
    }
    for (const CastleCase& castle : CASES) {
        RunAtomicKingRelocationCase(castle, sequence);
        sequence += 3U;
        RunAtomicKingRelocationCase(castle, sequence, true);
        sequence += 3U;
    }
    for (const CastleCase& castle : CASES) {
        RunDestinationOrderCase(castle, sequence, false, true, false);
        sequence += 3U;
        RunDestinationOrderCase(castle, sequence, false, false, false);
        sequence += 3U;
        RunDestinationOrderCase(castle, sequence, false, false, true);
        sequence += 5U;
        RunDestinationOrderCase(castle, sequence, true, true, false);
        sequence += 4U;
        RunDestinationOrderCase(castle, sequence, true, false, false);
        sequence += 4U;
    }

    if (g_failures == 0) {
        std::puts("[CHESS_INTERACTION_CASTLE_MATRIX] ok");
        return 0;
    }
    std::printf("[CHESS_INTERACTION_CASTLE_MATRIX] failures=%d\n", g_failures);
    return 1;
}
