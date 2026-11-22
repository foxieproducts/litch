#include "bounded_hint_engine.hpp"
#include <cstdio>

#include "foxie_embedded/millis.hpp"

#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (!cond) {
        std::printf("[FAIL] %s\n", msg);
        ++g_failures;
    }
}

bool FenToOccupancyMask(const char* fen, uint64_t& outOcc64) {
    outOcc64 = 0;
    if (fen == nullptr) {
        return false;
    }

    uint8_t rank = 0;
    uint8_t file = 0;
    for (const char* p = fen; *p != '\0'; ++p) {
        const char c = *p;
        if (c == ' ') {
            break;
        }
        if (c == '/') {
            if (file != 8) {
                return false;
            }
            ++rank;
            file = 0;
            continue;
        }
        if (c >= '1' && c <= '8') {
            file = static_cast<uint8_t>(file + static_cast<uint8_t>(c - '0'));
            if (file > 8) {
                return false;
            }
            continue;
        }
        const bool isPiece = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!isPiece || rank >= 8 || file >= 8) {
            return false;
        }
        outOcc64 |= (1ULL << static_cast<uint8_t>((rank * 8) + file));
        ++file;
    }
    return rank == 7 && file == 8;
}

bool GetEngineOcc(modules::IEngine& engine, uint64_t& outOcc64) {
    char buf[128] = {};
    return engine.GetFen(buf, sizeof(buf)) && FenToOccupancyMask(buf, outOcc64);
}

void SeedCastlingPosition(runtime::GameStateMachine& sm,
                          modules::GamePersistenceCoordinator& coord) {
    const char* fen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";
    const uint64_t occ = (1ULL << 0) | (1ULL << 4) | (1ULL << 7) |
                         (1ULL << 56) | (1ULL << 60) | (1ULL << 63);
    sm.DebugSetPhysicalOccupancyMask(occ);
    Check(coord.LoadExternalFen(fen), "LoadExternalFen (seed)");
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "state idle after seed");
    Check(!sm.IsPlacementModeActive(), "no placement mode after seed");
}

void SeedCastlingPositionWithoutRights(
    runtime::GameStateMachine& sm,
    modules::GamePersistenceCoordinator& coord) {
    const char* fen = "r3k2r/8/8/8/8/8/8/R3K2R w - - 0 1";
    const uint64_t occ = (1ULL << 0) | (1ULL << 4) | (1ULL << 7) |
                         (1ULL << 56) | (1ULL << 60) | (1ULL << 63);
    sm.DebugSetPhysicalOccupancyMask(occ);
    Check(coord.LoadExternalFen(fen), "LoadExternalFen (seed without rights)");
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle,
          "state idle after seed without rights");
}

void DispatchBoardObservation(runtime::GameStateMachine& sm,
                              const uint32_t tMs,
                              const uint64_t changedMask,
                              const uint64_t occupiedMask) {
    runtime::DispatchAppEventToStateMachine(
        sm,
        runtime::AppEvent::MakeBoardObservation(tMs, changedMask, occupiedMask));
}

void RemoveSettingsFiles(const char* path, const char* backup, const char* temp) {
    (void)std::remove(path);
    (void)std::remove(backup);
    (void)std::remove(temp);
}

} // namespace

int main() {
    std::puts("[CASTLING_ATOMIC_OBSERVATIONS] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000);

    runtime::ManualClock clock(1000);
    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    const uint64_t expectedOcc = (1ULL << 0) | (1ULL << 4) | (1ULL << 7) |
                                 (1ULL << 56) | (1ULL << 62) | (1ULL << 61);

    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_atomic_king_first.json";
        cfg.deferredSaveDelayMs = 999999;
        RemoveSettingsFiles(cfg.path,
                            "build/test_castling_suite_atomic_king_first.json.bak",
                            "build/test_castling_suite_atomic_king_first.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();
        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (atomic king-first)");
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (atomic king-first)");
        SeedCastlingPosition(sm, coord);

        const uint64_t initialOcc = sm.GetPhysicalOccupancyMask();
        const uint64_t kingLiftedOcc = initialOcc & ~(1ULL << 60);
        DispatchBoardObservation(sm, clock.NowMs(), 1ULL << 60, kingLiftedOcc);
        Check(sm.IsHumanPieceInHand(),
              "atomic king lift starts pickup");
        Check(sm.GetChessInteractionModel().phase.selectedSquare == 60,
              "atomic king lift preserves king origin");

        const uint64_t bothLiftedOcc = kingLiftedOcc & ~(1ULL << 63);
        DispatchBoardObservation(sm, clock.NowMs(), 1ULL << 63, bothLiftedOcc);
        Check(sm.IsHumanPieceInHand(),
              "atomic rook lift keeps castle pickup active");
        Check(!sm.HasCaptureFirstStaging(),
              "atomic king-first rook lift is not capture staging");
        Check(!sm.HasHumanIllegal(),
              "atomic king-first rook lift is accepted");
        Check(!sm.GetInteractionInvariantStatus().IsActive(),
              "shadow model agrees with sequential king-first pickup");
    }

    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_atomic_rook_first.json";
        cfg.deferredSaveDelayMs = 999999;
        RemoveSettingsFiles(cfg.path,
                            "build/test_castling_suite_atomic_rook_first.json.bak",
                            "build/test_castling_suite_atomic_rook_first.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();
        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (atomic rook-first)");
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (atomic rook-first)");
        SeedCastlingPosition(sm, coord);

        const uint64_t initialOcc = sm.GetPhysicalOccupancyMask();
        const uint64_t rookLiftedOcc = initialOcc & ~(1ULL << 63);
        DispatchBoardObservation(sm, clock.NowMs(), 1ULL << 63, rookLiftedOcc);
        Check(sm.GetChessInteractionModel().phase.selectedSquare == 63,
              "atomic rook lift initially starts rook pickup");

        const uint64_t bothLiftedOcc = rookLiftedOcc & ~(1ULL << 60);
        DispatchBoardObservation(sm, clock.NowMs(), 1ULL << 60, bothLiftedOcc);
        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan,
              "observed rook-first switches to a castling plan");
        Check(sm.GetChessInteractionModel().phase.plan.kind ==
                  runtime::PhysicalTransitionKind::Castling,
              "observed rook-first identifies castling intent");
        Check(!sm.HasHumanIllegal(),
              "observed rook-first remains positive guidance");
        Check(!sm.HasCaptureFirstStaging(),
              "observed rook-first is not capture staging");
        Check(!sm.IsCastlingRookPromptActive(),
              "observed rook-first still requires king placement first");
        Check(!sm.GetInteractionInvariantStatus().IsActive(),
              "shadow model agrees with observed rook-first castling");

        const uint64_t rookAtDestinationOcc = bothLiftedOcc | (1ULL << 61);
        DispatchBoardObservation(sm, clock.NowMs(), 1ULL << 61, rookAtDestinationOcc);
        Check(sm.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary,
              "rook-first placement accepts rook destination and still prompts king");
        Check(!sm.HasCommittedMovePending(),
              "rook-first partial castle does not commit an ordinary rook move");
        uint8_t rookFeedbackSquare = 255U;
        uint32_t rookFeedbackElapsedMs = 0U;
        Check(sm.TryGetSettleAfterglow(
                  rookFeedbackSquare, rookFeedbackElapsedMs) &&
                  rookFeedbackSquare == 61U &&
                  !sm.IsSettleAfterglowAbort(),
              "rook-first partial castle gets positive relocation feedback");
        uint64_t engineOcc = 0;
        Check(GetEngineOcc(engine, engineOcc), "engine fen read (atomic rook-first)");
        Check(engineOcc == initialOcc,
              "observed rook-first leaves acknowledged engine position unchanged");
    }

    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_atomic_queenside_pickup.json";
        cfg.deferredSaveDelayMs = 999999;
        RemoveSettingsFiles(cfg.path,
                            "build/test_castling_suite_atomic_queenside_pickup.json.bak",
                            "build/test_castling_suite_atomic_queenside_pickup.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();
        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (atomic queenside pickup)");
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (atomic queenside pickup)");
        SeedCastlingPosition(sm, coord);

        const uint64_t initialOcc = sm.GetPhysicalOccupancyMask();
        const uint64_t pairMask = (1ULL << 56) | (1ULL << 60);
        DispatchBoardObservation(sm, clock.NowMs(), pairMask, initialOcc & ~pairMask);
        Check(sm.IsHumanPieceInHand(),
              "same-scan queenside pickup stays active");
        Check(sm.GetChessInteractionModel().phase.plan.moveFrom == 60,
              "same-scan queenside pickup uses king origin");
        Check(!sm.HasHumanIllegal(),
              "same-scan queenside pickup is not rejected as rook-first");
        Check(!sm.HasCaptureFirstStaging(),
              "same-scan queenside pickup is not capture staging");
        Check(!sm.GetInteractionInvariantStatus().IsActive(),
              "shadow model agrees with same-scan castle pickup");
    }

    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_rook_first_without_rights.json";
        cfg.deferredSaveDelayMs = 999999;
        RemoveSettingsFiles(
            cfg.path,
            "build/test_castling_suite_rook_first_without_rights.json.bak",
            "build/test_castling_suite_rook_first_without_rights.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();
        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (rook-first without rights)");
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(),
              "LoadOrInitialize (rook-first without rights)");
        SeedCastlingPositionWithoutRights(sm, coord);

        const uint64_t initialOcc = sm.GetPhysicalOccupancyMask();
        const uint64_t rookLiftedOcc = initialOcc & ~(1ULL << 63);
        DispatchBoardObservation(sm, clock.NowMs(), 1ULL << 63, rookLiftedOcc);
        coord.UpdateLegalDestinationsMask();
        const uint64_t ordinaryRookDestinations = sm.GetValidDestMask();
        Check(ordinaryRookDestinations != 0ULL,
              "rook-first begins with ordinary rook destinations");

        const uint64_t bothLiftedOcc = rookLiftedOcc & ~(1ULL << 60);
        DispatchBoardObservation(sm, clock.NowMs(), 1ULL << 60, bothLiftedOcc);
        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::PieceLifted,
              "illegal rook-first castle preserves ordinary rook intent");
        Check(sm.GetChessInteractionModel().phase.selectedSquare == 63U,
              "illegal rook-first castle keeps the rook selected");
        Check(sm.GetValidDestMask() == ordinaryRookDestinations,
              "illegal rook-first castle preserves rook destinations");
        Check(sm.GetCaptureReturnMask() == (1ULL << 60),
              "illegal rook-first castle locally prompts the king home");
        Check(!sm.HasHumanIllegal(),
              "illegal rook-first castle avoids the global alarm");
    }

    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_atomic_kingside_placement.json";
        cfg.deferredSaveDelayMs = 999999;
        RemoveSettingsFiles(cfg.path,
                            "build/test_castling_suite_atomic_kingside_placement.json.bak",
                            "build/test_castling_suite_atomic_kingside_placement.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();
        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (atomic kingside placement)");
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (atomic kingside placement)");
        SeedCastlingPosition(sm, coord);

        const uint64_t initialOcc = sm.GetPhysicalOccupancyMask();
        const uint64_t pickupMask = (1ULL << 60) | (1ULL << 63);
        const uint64_t bothLiftedOcc = initialOcc & ~pickupMask;
        DispatchBoardObservation(sm, clock.NowMs(), pickupMask, bothLiftedOcc);

        const uint64_t placementMask = (1ULL << 61) | (1ULL << 62);
        DispatchBoardObservation(sm,
                                 clock.NowMs(),
                                 placementMask,
                                 bothLiftedOcc | placementMask);
        Check(!sm.GetInteractionInvariantStatus().IsActive(),
              "reducer-owned castle avoids pre-acknowledgement expected-state drift");
        Check(sm.IsMoveSettlingActive(),
              "completed castle starts its orthogonal settle window");
        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::AwaitingAction,
              "completed castle exposes its action before settle expiry");
        coord.Update();

        uint64_t engineOcc = 0;
        Check(GetEngineOcc(engine, engineOcc),
              "engine fen read (atomic kingside placement)");
        Check(engineOcc == expectedOcc,
              "same-scan kingside placement completes one castle");
        Check(!sm.HasHumanIllegal(),
              "same-scan kingside placement avoids index-order invalid state");
        Check(!sm.HasCommittedMovePending(),
              "same-scan kingside placement drains the single castle commit");
        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle,
              "successful engine acknowledgement completes the atomic castle plan");
        Check(sm.GetChessInteractionModel().acknowledged.occupiedMask == expectedOcc,
              "atomic castle acknowledgement advances shadow logical position");

        // The action can be acknowledged while the visual settle is still
        // active. A legal target-first pickup for the next turn must take over
        // immediately rather than retaining the completed castle's settle.
        Check(sm.IsMoveSettlingActive(),
              "castle settle remains active immediately after action acknowledgement");
        constexpr uint8_t NEXT_CAPTURE_TARGET = 56U;
        clock.AdvanceMs(6U);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        const uint64_t stagedCaptureOcc =
            expectedOcc & ~(1ULL << NEXT_CAPTURE_TARGET);
        DispatchBoardObservation(sm,
                                 clock.NowMs(),
                                 1ULL << NEXT_CAPTURE_TARGET,
                                 stagedCaptureOcc);
        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::CaptureStaged,
              "post-ack legal target-first removal starts the next interaction");
        Check(!sm.IsMoveSettlingActive(),
              "post-ack target-first interaction ends the completed castle settle");

        constexpr uint8_t NEXT_CAPTURE_MOVER = 0U;
        const uint64_t bothCapturePiecesLiftedOcc =
            stagedCaptureOcc & ~(1ULL << NEXT_CAPTURE_MOVER);
        DispatchBoardObservation(sm,
                                 clock.NowMs(),
                                 1ULL << NEXT_CAPTURE_MOVER,
                                 bothCapturePiecesLiftedOcc);
        Check(sm.IsHumanPieceInHand(),
              "target-first capture accepts its legal mover pickup");
        sm.SetValidDestMask(1ULL << NEXT_CAPTURE_TARGET);
        const uint64_t completedCaptureOcc =
            bothCapturePiecesLiftedOcc | (1ULL << NEXT_CAPTURE_TARGET);
        DispatchBoardObservation(sm,
                                 clock.NowMs(),
                                 1ULL << NEXT_CAPTURE_TARGET,
                                 completedCaptureOcc);
        Check(sm.IsMoveSettlingActive(),
              "completed target-first capture starts a fresh settle window");

        clock.AdvanceMs(5U);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        sm.Tick();
        Check(sm.IsMoveSettlingActive(),
              "fresh capture settle does not inherit the castle timer elapsed time");
        clock.AdvanceMs(6U);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        sm.Tick();
        runtime::PhysicalTransitionPlan captureAction;
        Check(sm.TryClaimPendingInteractionMove(captureAction),
              "completed target-first capture exposes one action");
        Check(captureAction.moveFrom == NEXT_CAPTURE_MOVER &&
                  captureAction.moveTo == NEXT_CAPTURE_TARGET,
              "target-first capture action preserves mover and target");
        Check(!sm.TryClaimPendingInteractionMove(captureAction),
              "target-first capture action is exposed exactly once");
    }

    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_settle_action_boundary.json";
        cfg.deferredSaveDelayMs = 999999;
        RemoveSettingsFiles(
            cfg.path,
            "build/test_castling_suite_settle_action_boundary.json.bak",
            "build/test_castling_suite_settle_action_boundary.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();
        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (castle settle action boundary)");
        test_support::BoundedHintEngine engine;
        engine.Reset();
        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(),
              "LoadOrInitialize (castle settle action boundary)");
        SeedCastlingPosition(sm, coord);

        const uint64_t initialOcc = sm.GetPhysicalOccupancyMask();
        const uint64_t pickupMask = (1ULL << 60) | (1ULL << 63);
        const uint64_t bothLiftedOcc = initialOcc & ~pickupMask;
        const uint64_t placementMask = (1ULL << 61) | (1ULL << 62);
        const uint64_t castledOcc = bothLiftedOcc | placementMask;
        DispatchBoardObservation(sm, clock.NowMs(), pickupMask, bothLiftedOcc);
        DispatchBoardObservation(sm, clock.NowMs(), placementMask, castledOcc);
        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::AwaitingAction,
              "completed castle exposes one pending action");
        Check(sm.IsMoveSettlingActive(),
              "castle action boundary retains orthogonal settle feedback");

        constexpr uint8_t KING_DESTINATION = 62U;
        DispatchBoardObservation(sm,
                                 clock.NowMs(),
                                 1ULL << KING_DESTINATION,
                                 castledOcc & ~(1ULL << KING_DESTINATION));
        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan,
              "settle-square relift reopens castle placement");
        Check(sm.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary,
              "relifted king destination is prompted again");
        Check(!sm.HasCommittedMovePending(),
              "settle-square relift invalidates the old castle token");
        Check(!sm.IsMoveSettlingActive(),
              "settle-square relift ends the old visual window");

        coord.Update();
        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan,
              "coordinator cannot apply a castle with its king airborne");

        DispatchBoardObservation(sm,
                                 clock.NowMs(),
                                 1ULL << KING_DESTINATION,
                                 castledOcc);
        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::AwaitingAction,
              "replacing the king exposes a fresh castle token");
        Check(sm.IsMoveSettlingActive(),
              "replacement starts a fresh composite settle window");
        coord.Update();
        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle,
              "replacement castle is applied exactly once");
        Check(coord.GetUndoHistoryMoveCount() == 1U,
              "replacement castle records one engine move");
    }

    if (g_failures == 0) {
        std::puts("[CASTLING_ATOMIC_OBSERVATIONS] PASS");
        return 0;
    }
    std::printf("[CASTLING_ATOMIC_OBSERVATIONS] FAILURES=%d\n", g_failures);
    return 1;
}
