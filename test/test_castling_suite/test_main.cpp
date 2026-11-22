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
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

#include "castling_suite_support.hpp"

void SeedCastlingPosition(runtime::GameStateMachine& sm,
                          runtime::ManualClock& clock,
                          modules::GamePersistenceCoordinator& coord,
                          modules::IEngine& engine) {
    // FEN with only kings+rooks and open files: castling is legal for both sides.
    const char* fen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";

    // Keep physical occupancy consistent so the coordinator won't force placement mode.
    const uint64_t occ = (1ULL << 0) | (1ULL << 4) | (1ULL << 7) | (1ULL << 56) | (1ULL << 60) | (1ULL << 63);
    sm.DebugSetPhysicalOccupancyMask(occ);

    Check(coord.LoadExternalFen(fen), "LoadExternalFen (seed)");

    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "state idle after seed");
    Check(!sm.IsPlacementModeActive(), "no placement mode after seed");
}

void DispatchBoardObservation(runtime::GameStateMachine& sm,
                              const uint32_t tMs,
                              const uint64_t changedMask,
                              const uint64_t occupiedMask) {
    runtime::DispatchAppEventToStateMachine(
        sm,
        runtime::AppEvent::MakeBoardObservation(tMs, changedMask, occupiedMask));
}

} // namespace

int main() {
    std::puts("[CASTLING_SUITE] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000);

    runtime::ManualClock clock(1000);
    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    const uint64_t expectedOcc = (1ULL << 0) | (1ULL << 4) | (1ULL << 7) | (1ULL << 56) | (1ULL << 62) | (1ULL << 61);

    // 1) Separate observations preserve an observed king-first pickup without
    // misclassifying the rook as capture staging.
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_atomic_king_first.json";
        cfg.deferredSaveDelayMs = 999999;
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_castling_suite_atomic_king_first.json.bak");
        (void)std::remove("build/test_castling_suite_atomic_king_first.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (atomic king-first)");

        test_support::BoundedHintEngine engine;
        engine.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (atomic king-first)");
        SeedCastlingPosition(sm, clock, coord, engine);

        const uint64_t initialOcc = sm.GetPhysicalOccupancyMask();
        const uint64_t kingLiftedOcc = initialOcc & ~(1ULL << 60);
        DispatchBoardObservation(sm, clock.NowMs(), (1ULL << 60), kingLiftedOcc);

        Check(sm.IsHumanPieceInHand(),
              "atomic king lift starts pickup");
        Check(sm.GetChessInteractionModel().phase.selectedSquare == 60,
              "atomic king lift preserves king origin");

        const uint64_t bothLiftedOcc = kingLiftedOcc & ~(1ULL << 63);
        DispatchBoardObservation(sm, clock.NowMs(), (1ULL << 63), bothLiftedOcc);

        Check(sm.IsHumanPieceInHand(),
              "atomic rook lift keeps castle pickup active");
        Check(!sm.HasCaptureFirstStaging(),
              "atomic king-first rook lift is not capture staging");
        Check(!sm.HasHumanIllegal(),
              "atomic king-first rook lift is accepted");
        Check(!sm.GetInteractionInvariantStatus().IsActive(),
              "shadow model agrees with sequential king-first pickup");
    }

    // 2) Rook-first across separate observations is observable and must be
    // rejected as a correction, never reinterpreted as castling or a rook move.
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_atomic_rook_first.json";
        cfg.deferredSaveDelayMs = 999999;
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_castling_suite_atomic_rook_first.json.bak");
        (void)std::remove("build/test_castling_suite_atomic_rook_first.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (atomic rook-first)");

        test_support::BoundedHintEngine engine;
        engine.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (atomic rook-first)");
        SeedCastlingPosition(sm, clock, coord, engine);

        const uint64_t initialOcc = sm.GetPhysicalOccupancyMask();
        const uint64_t rookLiftedOcc = initialOcc & ~(1ULL << 63);
        DispatchBoardObservation(sm, clock.NowMs(), (1ULL << 63), rookLiftedOcc);
        Check(sm.GetChessInteractionModel().phase.selectedSquare == 63,
              "atomic rook lift initially starts rook pickup");

        const uint64_t bothLiftedOcc = rookLiftedOcc & ~(1ULL << 60);
        DispatchBoardObservation(sm, clock.NowMs(), (1ULL << 60), bothLiftedOcc);

        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan &&
                  sm.GetChessInteractionModel().phase.plan.kind ==
                      runtime::PhysicalTransitionKind::Castling,
              "observed rook-first switches to castling intent");
        Check(!sm.HasHumanIllegal(),
              "observed rook-first keeps positive guidance");
        const uint64_t rookAtDestinationOcc = bothLiftedOcc | (1ULL << 61);
        DispatchBoardObservation(sm, clock.NowMs(), (1ULL << 61), rookAtDestinationOcc);
        Check(sm.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary &&
                  !sm.HasCommittedMovePending(),
              "rook-first destination waits for the king without committing");

        uint64_t engineOcc = 0;
        Check(GetEngineOcc(engine, engineOcc), "engine fen read (atomic rook-first)");
        Check(engineOcc == initialOcc,
              "observed rook-first leaves acknowledged engine position unchanged");
    }

    // 3) A king+rook disappearance in one scan is order-ambiguous. Accept it
    // as king-first-equivalent even when square ordering lists the rook first.
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_atomic_queenside_pickup.json";
        cfg.deferredSaveDelayMs = 999999;
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_castling_suite_atomic_queenside_pickup.json.bak");
        (void)std::remove("build/test_castling_suite_atomic_queenside_pickup.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (atomic queenside pickup)");

        test_support::BoundedHintEngine engine;
        engine.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (atomic queenside pickup)");
        SeedCastlingPosition(sm, clock, coord, engine);

        const uint64_t initialOcc = sm.GetPhysicalOccupancyMask();
        const uint64_t pairMask = (1ULL << 56) | (1ULL << 60);
        DispatchBoardObservation(sm,
                                 clock.NowMs(),
                                 pairMask,
                                 initialOcc & ~pairMask);

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

    // 4) When both destinations appear in one scan, completion is independent
    // of square indices. Kingside lists rook destination before king destination.
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_atomic_kingside_placement.json";
        cfg.deferredSaveDelayMs = 999999;
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_castling_suite_atomic_kingside_placement.json.bak");
        (void)std::remove("build/test_castling_suite_atomic_kingside_placement.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (atomic kingside placement)");

        test_support::BoundedHintEngine engine;
        engine.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (atomic kingside placement)");
        SeedCastlingPosition(sm, clock, coord, engine);

        const uint64_t initialOcc = sm.GetPhysicalOccupancyMask();
        const uint64_t pickupMask = (1ULL << 60) | (1ULL << 63);
        const uint64_t bothLiftedOcc = initialOcc & ~pickupMask;
        DispatchBoardObservation(sm,
                                 clock.NowMs(),
                                 pickupMask,
                                 bothLiftedOcc);

        const uint64_t placementMask = (1ULL << 61) | (1ULL << 62);
        DispatchBoardObservation(sm,
                                 clock.NowMs(),
                                 placementMask,
                                 bothLiftedOcc | placementMask);

        Check(!sm.GetInteractionInvariantStatus().IsActive(),
              "reducer-owned castle avoids pre-acknowledgement expected-state drift");

        clock.AdvanceMs(20);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        sm.Tick();
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
        Check(sm.GetChessInteractionModel().acknowledged.occupiedMask ==
                  expectedOcc,
              "atomic castle acknowledgement advances shadow logical position");
    }

    // 5) King-first: after the king settles, rook completion is forced and
    // unrelated pickups are invalid.
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_king_first.json";
        cfg.deferredSaveDelayMs = 999999;
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_castling_suite_king_first.json.bak");
        (void)std::remove("build/test_castling_suite_king_first.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (king-first)");

        test_support::BoundedHintEngine engine;
        engine.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (king-first)");

        SeedCastlingPosition(sm, clock, coord, engine);

        const uint32_t savesBefore = settings.GetSaveCount();

        sm.HandleSensorChange(60, false); // e1 up
        sm.HandleSensorChange(62, true);  // g1 down

        clock.AdvanceMs(20);
        sm.Tick();
        coord.Update();

        Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan,
              "king-first castle keeps its reducer plan for the rook prompt");
        Check(!sm.HasCpuIllegal(), "castling rook prompt starts clean");
        Check(settings.GetSaveCount() == savesBefore,
              "castling rook prompt queues a debounced save");
        Check(settings.SaveNow(),
              "castling rook prompt reaches storage after debounce simulation");
        Check(settings.GetSaveCount() == savesBefore + 1U,
              "castling rook prompt completes one deferred save");

        {
            modules::SettingsStore settingsAfterReboot(cfg);
            Check(settingsAfterReboot.Load(), "settings load (castling reboot)");

            runtime::GameStateMachine smAfterReboot(smCfg);
            smAfterReboot.Reset();

            test_support::BoundedHintEngine engineAfterReboot;
            engineAfterReboot.Reset();

            modules::GamePersistenceCoordinator coordAfterReboot(settingsAfterReboot, engineAfterReboot, smAfterReboot);
            Check(coordAfterReboot.LoadOrInitialize(), "LoadOrInitialize (castling reboot)");

            uint8_t restoredRookFrom = 255;
            uint8_t restoredRookTo = 255;
            Check(smAfterReboot.TryGetCastlingRookPromptMove(restoredRookFrom, restoredRookTo),
                  "castling reboot restores rook prompt");
            Check(restoredRookFrom == 63 && restoredRookTo == 61,
                  "castling reboot restores correct rook prompt squares");

            smAfterReboot.HandleSensorChange(restoredRookFrom, false);
            smAfterReboot.HandleSensorChange(restoredRookTo, true);

            clock.AdvanceMs(20);
            fxe::ManualMillis::SetNowMs(clock.NowMs());
            smAfterReboot.Tick();
            coordAfterReboot.Update();

            uint64_t occAfterReboot = 0;
            Check(GetEngineOcc(engineAfterReboot, occAfterReboot), "engine fen read (castling reboot)");
            Check(occAfterReboot == expectedOcc, "engine occupancy matches castled after rebooted rook prompt");
        }

        // LitchMaxEngine's native backend is process-global. The nested reboot
        // instance above therefore leaves this instance's backend castled even
        // though its coordinator still owns the pre-castle position.
        Check(engine.SetFen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"),
              "restore original native engine context after nested reboot");

        sm.HandleSensorChange(56, false); // wrong rook up
        Check(sm.HasHumanIllegal(),
              "castling rook prompt rejects unrelated pickup");
        sm.HandleSensorChange(56, true); // wrong rook restored
        Check(!sm.HasHumanIllegal() &&
                  sm.GetChessInteractionModel().phase.tag ==
                      runtime::ChessInteractionPhase::ExecutingPlan,
              "corrected unrelated pickup resumes the castle plan");

        sm.HandleSensorChange(63, false); // h1 up
        sm.HandleSensorChange(61, true);  // f1 down

        clock.AdvanceMs(20);
        sm.Tick();

        coord.Update();
        Check(settings.GetSaveCount() == savesBefore + 1U,
              "completed castle remains queued behind the debounce");
        Check(settings.SaveNow(),
              "completed castle reaches storage after debounce simulation");
        Check(settings.GetSaveCount() == savesBefore + 2U,
              "completed castle adds one deferred save");

        uint64_t occAfter = 0;
        Check(GetEngineOcc(engine, occAfter), "engine fen read (king-first)");
        if (occAfter != expectedOcc) {
            char fenBuf[128] = {};
            if (GetEngineFen(engine, fenBuf, sizeof(fenBuf))) {
                std::printf("[INFO] engine fen (king-first): %s\n", fenBuf);
            }
        }
        Check(occAfter == expectedOcc, "engine occupancy matches castled (king-first)");
    }

    // 6) Pickup tolerance: lift both before placing.
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_pickup_tolerance.json";
        cfg.deferredSaveDelayMs = 999999;
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_castling_suite_pickup_tolerance.json.bak");
        (void)std::remove("build/test_castling_suite_pickup_tolerance.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (pickup tolerance)");

        test_support::BoundedHintEngine engine;
        engine.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (pickup tolerance)");

        SeedCastlingPosition(sm, clock, coord, engine);

        const uint32_t savesBefore = settings.GetSaveCount();

        sm.HandleSensorChange(60, false); // king up
        sm.HandleSensorChange(63, false); // rook up (while king in hand)
        sm.HandleSensorChange(62, true);  // king down
        sm.HandleSensorChange(61, true);  // rook down

        clock.AdvanceMs(20);
        sm.Tick();

        coord.Update();
        Check(settings.GetSaveCount() == savesBefore,
              "completed pickup-tolerance castle queues a debounced save");
        Check(settings.SaveNow(),
              "pickup-tolerance castle reaches storage after debounce simulation");
        Check(settings.GetSaveCount() == savesBefore + 1U,
              "pickup-tolerance castle adds one deferred save");
        uint64_t occAfter = 0;
        Check(GetEngineOcc(engine, occAfter), "engine fen read (pickup tolerance)");
        if (occAfter != expectedOcc) {
            char fenBuf[128] = {};
            if (GetEngineFen(engine, fenBuf, sizeof(fenBuf))) {
                std::printf("[INFO] engine fen (pickup tolerance): %s\n", fenBuf);
            }
        }
        Check(occAfter == expectedOcc, "engine occupancy matches castled (pickup tolerance)");
    }

    // 7) Rook move to castling rook destination must remain a normal move.
    {
        modules::SettingsStoreConfig cfg;
        cfg.path = "build/test_castling_suite_rook_to_d1_is_normal.json";
        cfg.deferredSaveDelayMs = 999999;
        (void)std::remove(cfg.path);
        (void)std::remove("build/test_castling_suite_rook_to_d1_is_normal.json.bak");
        (void)std::remove("build/test_castling_suite_rook_to_d1_is_normal.json.tmp");

        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::SettingsStore settings(cfg);
        Check(settings.Load(), "settings load (rook d1 normal)");

        test_support::BoundedHintEngine engine;
        engine.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (rook d1 normal)");

        const char* fen = "4k3/8/8/8/8/8/8/R3K2R w Q - 0 1";
        const uint64_t occ = (1ULL << 4) | (1ULL << 56) | (1ULL << 60) | (1ULL << 63);
        sm.DebugSetPhysicalOccupancyMask(occ);
        Check(coord.LoadExternalFen(fen), "LoadExternalFen (rook d1 normal seed)");

        const uint32_t savesBefore = settings.GetSaveCount();

        sm.HandleSensorChange(56, false); // a1 up
        sm.HandleSensorChange(59, true);  // d1 down

        clock.AdvanceMs(20);
        sm.Tick();

        coord.Update();
        Check(settings.GetSaveCount() == savesBefore,
              "normal rook move queues a debounced save");
        Check(settings.SaveNow(),
              "normal rook move reaches storage after debounce simulation");
        Check(settings.GetSaveCount() == savesBefore + 1U,
              "normal rook move adds one deferred save");

        uint64_t occAfter = 0;
        Check(GetEngineOcc(engine, occAfter), "engine fen read (rook d1 normal)");
        const uint64_t expectedNormalOcc = (1ULL << 4) | (1ULL << 59) | (1ULL << 60) | (1ULL << 63);
        Check(occAfter == expectedNormalOcc, "rook d1 normal occupancy committed");

        char fenBuf[128] = {};
        Check(GetEngineFen(engine, fenBuf, sizeof(fenBuf)), "engine fen text read (rook d1 normal)");
        Check(FenSideToMoveIsBlack(fenBuf), "rook d1 normal advances side to move");
    }

    if (g_failures == 0) {
        std::puts("[CASTLING_SUITE] ok");
        return 0;
    }

    std::printf("[CASTLING_SUITE] failures=%d\n", g_failures);
    return 1;
}
