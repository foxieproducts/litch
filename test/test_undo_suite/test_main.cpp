#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/app_event.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

bool GetFen(const modules::IEngine& engine, char* out, const size_t outSize) {
    if (!out || outSize == 0) {
        return false;
    }
    out[0] = '\0';
    return engine.GetFen(out, outSize);
}

void Sensor(runtime::ManualClock& clock, runtime::GameStateMachine& sm, const uint8_t square, const bool occ) {
    runtime::DispatchAppEventToStateMachine(sm, runtime::AppEvent::MakeSensorDelta(clock.NowMs(), square, occ));
}

void Tick(runtime::ManualClock& clock, runtime::GameStateMachine& sm, const uint32_t deltaMs) {
    clock.AdvanceMs(deltaMs);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    runtime::DispatchAppEventToStateMachine(sm, runtime::AppEvent::MakeTick(clock.NowMs()));
}

void PressP1(runtime::ManualClock& clock,
             runtime::GameStateMachine& sm,
             modules::GamePersistenceCoordinator& coordinator) {
    (void)clock;
    (void)sm;
    Check(coordinator.RequestUndo(1U), "undo control accepted");
}

void DoMove(runtime::ManualClock& clock,
            runtime::GameStateMachine& sm,
            const uint8_t from,
            const uint8_t to,
            const uint32_t settleMs) {
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.ApplyPhysicalOccupancySnapshot(sm.GetExpectedOccupancyMask());
    Sensor(clock, sm, from, false);
    Sensor(clock, sm, to, true);
    Tick(clock, sm, settleMs + 1);
}

} // namespace

int main() {
    std::puts("[UNDO_SUITE] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_undo_suite.json";
    cfg.deferredSaveDelayMs = 100000;
    (void)std::remove(cfg.path);
    (void)std::remove("build/test_undo_suite.json.bak");
    (void)std::remove("build/test_undo_suite.json.tmp");

    runtime::ManualClock clock(1000);
    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::SettingsStore settings(cfg);
    Check(settings.Load(), "settings load");

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    Check(coord.LoadOrInitialize(), "LoadOrInitialize");

    char baseFen[160] = {};
    Check(GetFen(engine, baseFen, sizeof(baseFen)), "base fen");

    // Commit a move and ensure it reaches the engine + persistence.
    DoMove(clock, sm, 52, 44, smCfg.settleDurationMs); // e2 -> e3 (0=a8)
    Check(sm.ShouldProcessCommittedMoveImmediately(), "move finalization stays immediate");
    const uint32_t savesAfterInit = settings.GetSaveCount();
    coord.Update();
    Check(settings.GetSaveCount() == savesAfterInit,
          "finalize queues persistence behind the debounce");
    Check(settings.SaveNow(),
          "finalized move reaches storage after debounce simulation");

    char movedFen[160] = {};
    Check(GetFen(engine, movedFen, sizeof(movedFen)), "moved fen");
    Check(std::strcmp(baseFen, movedFen) != 0, "fen changes after move");

    // Establish an already-visible Hint before testing its Undo restoration. The
    // current turn's initial analysis window must finish before any reveal.
    Tick(clock, sm, 500U);
    coord.Update();
    uint8_t originalHintFrom = 64U;
    uint8_t originalHintTo = 64U;
    Check(sm.TryGetHintMoveSquares(originalHintFrom, originalHintTo),
          "production analysis supplies the original-position Hint");
    uint64_t originalHintDestinations = 0ULL;
    Check(engine.GetLegalDestinationsMask(originalHintFrom, originalHintDestinations) &&
              originalHintTo < 64U &&
              ((originalHintDestinations >> originalHintTo) & 1ULL) != 0ULL,
          "original visible Hint is legal in the forward position");
    sm.HandleHintButtonTap();
    Check(sm.IsHintDisplayRequested(), "tap reveals cached hint before undo");

    // Undo should rebuild engine from undo-base + remaining history.
    const uint32_t savesBeforeUndo = settings.GetSaveCount();
    PressP1(clock, sm, coord);
    Check(sm.IsUndoPromptActive(), "undo prompt active after undo");
    Check(!sm.ShouldProcessCommittedMoveImmediately(), "undo commit defers heavy coordinator work");
    Check(!sm.IsPlacementModeActive(), "undo should not force placement mode");
    {
        uint8_t from = 0;
        uint8_t to = 0;
        Check(sm.TryGetUndoPromptMove(from, to), "undo prompt move available");
        Check(from == 52, "undo prompt from matches move");
        Check(to == 44, "undo prompt to matches move");
    }
    coord.Update();
    Check(settings.GetSaveCount() == savesBeforeUndo, "undo save is deferred while prompt remains active");

    char undoFen[160] = {};
    Check(GetFen(engine, undoFen, sizeof(undoFen)), "undo fen");
    Check(std::strcmp(baseFen, undoFen) == 0, "fen restored after undo");

    // Repeated updates during the physical Undo prompt keep Hint hidden;
    // that prompt intentionally prevents new background analysis.
    coord.Update();
    coord.Update();
    uint8_t hiddenHintFrom = 64U;
    uint8_t hiddenHintTo = 64U;
    Check(!sm.TryGetHintMoveSquares(hiddenHintFrom, hiddenHintTo) &&
              !sm.IsHintDisplayRequested(),
          "Undo prompt hides Hint while the physical inverse is pending");

    // Cancel-undo should re-apply the move if no physical changes.
    const uint32_t savesBeforeCancel = settings.GetSaveCount();
    PressP1(clock, sm, coord);
    Check(!sm.IsUndoPromptActive(), "cancel-undo clears undo prompt");
    Check(!sm.ShouldProcessCommittedMoveImmediately(), "cancel-undo commit defers heavy coordinator work");
    coord.Update();
    Check(settings.GetSaveCount() == savesBeforeCancel,
          "cancel-undo queues persistence behind the debounce");

    char cancelFen[160] = {};
    Check(GetFen(engine, cancelFen, sizeof(cancelFen)), "cancel fen");
    Check(std::strcmp(movedFen, cancelFen) == 0, "fen restored after cancel-undo");
    {
        uint8_t hintFrom = 0;
        uint8_t hintTo = 0;
        Check(sm.TryGetHintMoveSquares(hintFrom, hintTo), "hint move restored after cancel-undo");
        Check(hintFrom == originalHintFrom, "hint from-square preserved after cancel-undo");
        Check(hintTo == originalHintTo, "hint to-square preserved after cancel-undo");
        sm.HandleHintButtonTap();
        Check(sm.IsHintDisplayRequested(), "cancel-undo restored Hint can be revealed again");
        std::printf("[UNDO_HINT_RESTORE] original=%u-%u restored=%u-%u revealed=%u\n",
                    originalHintFrom, originalHintTo, hintFrom, hintTo,
                    sm.IsHintDisplayRequested());
    }

    // While undo prompt is active, cancel-undo should remain available even
    // after unrelated physical occupancy changes.
    PressP1(clock, sm, coord); // undo again
    Sensor(clock, sm, 7, true);
    Check(coord.IsCancelUndoAvailable(), "sensor delta keeps cancel-undo during undo prompt");
    coord.Update();
    Sensor(clock, sm, 44, false);
    Sensor(clock, sm, 52, true);
    coord.Update();
    char freshFen[160] = {};
    Check(GetFen(engine, freshFen, sizeof(freshFen)) &&
              std::strcmp(freshFen, modules::INITIAL_FEN) == 0 &&
              coord.GetUndoHistoryMoveCount() == 0U &&
              sm.GetMovedSquaresMask() == 0ULL &&
              coord.GetGameVariant() == modules::GameVariant::Undecided,
          "completing the final undo restores genuine fresh-game state");

    runtime::InteractionDispatcher freshDispatcher(sm, coord);
    const uint64_t initial = sm.GetPhysicalOccupancyMask();
    freshDispatcher.DispatchBoardObservation({
        .tMs = clock.NowMs(),
        .sequence = sm.GetLastBoardObservationSequence() + 1U,
        .changedMask = 1ULL << 18U,
        .occupiedMask = initial | (1ULL << 18U),
    });
    clock.AdvanceMs(200U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    coord.Update();
    Check(coord.GetGameVariant() == modules::GameVariant::Duck,
          "a valid Duck can be added after undoing to the beginning");

    // Undo button should not repeat: while an undo prompt is active, further
    // undo presses must not start a second undo.
    {
        modules::SettingsStoreConfig repCfg;
        repCfg.path = "build/test_undo_suite_no_repeat.json";
        repCfg.deferredSaveDelayMs = 100000;
        (void)std::remove(repCfg.path);
        (void)std::remove("build/test_undo_suite_no_repeat.json.bak");
        (void)std::remove("build/test_undo_suite_no_repeat.json.tmp");

        runtime::GameStateMachine smRep(smCfg);
        smRep.Reset();

        modules::SettingsStore settingsRep(repCfg);
        Check(settingsRep.Load(), "settings load (no-repeat)");

        test_support::BoundedHintEngine engineRep;
        engineRep.Reset();

        modules::GamePersistenceCoordinator coordRep(settingsRep, engineRep, smRep);
        Check(coordRep.LoadOrInitialize(), "LoadOrInitialize (no-repeat)");

        DoMove(clock, smRep, 52, 44, smCfg.settleDurationMs); // e2 -> e3
        coordRep.Update();
        DoMove(clock, smRep, 11, 27, smCfg.settleDurationMs); // d7 -> d5
        coordRep.Update();

        PressP1(clock, smRep, coordRep);
        Check(smRep.IsUndoPromptActive(), "undo prompt active (no-repeat)");
        uint8_t from0 = 0;
        uint8_t to0 = 0;
        Check(smRep.TryGetUndoPromptMove(from0, to0), "undo prompt move (no-repeat)");
        Check(from0 == 11, "undo prompt from is last move (no-repeat)");
        Check(to0 == 27, "undo prompt to is last move (no-repeat)");

        // Unrelated occupancy change should not invalidate cancel-undo while
        // undo prompt is active.
        Sensor(clock, smRep, 7, true);
        Check(coordRep.IsCancelUndoAvailable(), "cancel-undo remains available (no-repeat)");

        // Pressing undo while prompt is active should perform cancel-undo,
        // not start a second undo.
        PressP1(clock, smRep, coordRep);
        Check(!smRep.IsUndoPromptActive(), "cancel-undo clears prompt (no-repeat)");
        coordRep.Update();

        // Starting undo again should target the same last move.
        PressP1(clock, smRep, coordRep);
        uint8_t from1 = 0;
        uint8_t to1 = 0;
        Check(smRep.TryGetUndoPromptMove(from1, to1), "undo prompt move available after cancel (no-repeat)");
        Check(from1 == from0, "undo targets same last move after cancel (no-repeat)");
        Check(to1 == to0, "undo targets same destination after cancel (no-repeat)");
    }

    // Multi-move undo should restore to the previous position, not always to the initial FEN.
    // Sequence (legal): 1) e2e3 1... d7d5 2) g1f3
    {
        modules::SettingsStoreConfig multiCfg;
        multiCfg.path = "build/test_undo_suite_multi.json";
        multiCfg.deferredSaveDelayMs = 100000;
        (void)std::remove(multiCfg.path);
        (void)std::remove("build/test_undo_suite_multi.json.bak");
        (void)std::remove("build/test_undo_suite_multi.json.tmp");

        runtime::GameStateMachine sm2(smCfg);
        sm2.Reset();

        modules::SettingsStore settings2(multiCfg);
        Check(settings2.Load(), "settings load (multi)");

        test_support::BoundedHintEngine engine2;
        engine2.Reset();

        modules::GamePersistenceCoordinator coord2(settings2, engine2, sm2);
        Check(coord2.LoadOrInitialize(), "LoadOrInitialize (multi-move)");

        DoMove(clock, sm2, 52, 44, smCfg.settleDurationMs); // e2 -> e3
        coord2.Update();

        DoMove(clock, sm2, 11, 27, smCfg.settleDurationMs); // d7 -> d5
        coord2.Update();

        char fenAfterTwoMoves[160] = {};
        Check(GetFen(engine2, fenAfterTwoMoves, sizeof(fenAfterTwoMoves)), "fen after two moves");

        DoMove(clock, sm2, 62, 45, smCfg.settleDurationMs); // g1 -> f3
        coord2.Update();

        char fenAfterThreeMoves[160] = {};
        Check(GetFen(engine2, fenAfterThreeMoves, sizeof(fenAfterThreeMoves)), "fen after three moves");
        Check(std::strcmp(fenAfterTwoMoves, fenAfterThreeMoves) != 0, "fen changes after third move");

        PressP1(clock, sm2, coord2);
        Check(sm2.IsUndoPromptActive(), "undo prompt active (multi-move)");
        coord2.Update();

        char fenAfterUndo3[160] = {};
        Check(GetFen(engine2, fenAfterUndo3, sizeof(fenAfterUndo3)), "fen after undo third move");
        Check(std::strcmp(fenAfterTwoMoves, fenAfterUndo3) == 0, "undo restores to fen after two moves");

        // Completing the undo prompt must close the cancel-undo window so the
        // next undo traverses older history instead of replaying the move.
        uint8_t undoFrom = 0;
        uint8_t undoTo = 0;
        Check(sm2.TryGetUndoPromptMove(undoFrom, undoTo), "undo prompt move available for traversal");
        Sensor(clock, sm2, undoTo, false);
        Sensor(clock, sm2, undoFrom, true);
        Check(!sm2.IsUndoPromptActive(), "undo prompt clears after physical completion");
        PressP1(clock, sm2, coord2);
        Check(sm2.IsUndoPromptActive(), "second undo starts after prompt completion");
        uint8_t olderFrom = 0;
        uint8_t olderTo = 0;
        Check(sm2.TryGetUndoPromptMove(olderFrom, olderTo), "older undo prompt available");
        Check(olderFrom == 11, "second undo targets prior move from-square");
        Check(olderTo == 27, "second undo targets prior move to-square");
    }

    // Sleep/reboot while an undo prompt is active should not persist the
    // unresolved undone position. The flush path should cancel the prompt back
    // to the forward position before saving.
    {
        modules::SettingsStoreConfig rebootCfg;
        rebootCfg.path = "build/test_undo_suite_reboot_traversal.json";
        rebootCfg.deferredSaveDelayMs = 100000;
        (void)std::remove(rebootCfg.path);
        (void)std::remove("build/test_undo_suite_reboot_traversal.json.bak");
        (void)std::remove("build/test_undo_suite_reboot_traversal.json.tmp");

        runtime::ManualClock clockA(1000);
        runtime::GameStateMachine smA(smCfg);
        smA.Reset();

        modules::SettingsStore settingsA(rebootCfg);
        Check(settingsA.Load(), "settings load (reboot traversal phase A)");

        test_support::BoundedHintEngine engineA;
        engineA.Reset();

        modules::GamePersistenceCoordinator coordA(settingsA, engineA, smA);
        Check(coordA.LoadOrInitialize(), "LoadOrInitialize (reboot traversal phase A)");

        DoMove(clockA, smA, 52, 44, smCfg.settleDurationMs); // e2 -> e3
        coordA.Update();
        DoMove(clockA, smA, 11, 27, smCfg.settleDurationMs); // d7 -> d5
        coordA.Update();

        char fenBeforeUndoSleep[160] = {};
        Check(GetFen(engineA, fenBeforeUndoSleep, sizeof(fenBeforeUndoSleep)), "fen before undo sleep");

        PressP1(clockA, smA, coordA);
        Check(smA.IsUndoPromptActive(), "undo prompt active before reboot");
        coordA.Update();

        char fenAfterFirstUndo[160] = {};
        Check(GetFen(engineA, fenAfterFirstUndo, sizeof(fenAfterFirstUndo)), "fen after first undo before reboot");
        Check(std::strcmp(fenBeforeUndoSleep, fenAfterFirstUndo) != 0, "undo changed engine state before sleep flush");

        Check(coordA.FlushToSettingsNow(), "flush cancels unresolved undo before reboot");

        // Reboot after flush and ensure the forward position, not the unresolved
        // undone state, is restored.
        modules::SettingsStore settingsB(rebootCfg);
        Check(settingsB.Load(), "settings load (reboot traversal phase B)");

        runtime::ManualClock clockB(clockA.NowMs());
        runtime::GameStateMachine smB(smCfg);
        smB.Reset();

        test_support::BoundedHintEngine engineB;
        engineB.Reset();

        modules::GamePersistenceCoordinator coordB(settingsB, engineB, smB);
        Check(coordB.LoadOrInitialize(), "LoadOrInitialize (reboot traversal phase B)");

        char fenAfterReboot[160] = {};
        Check(GetFen(engineB, fenAfterReboot, sizeof(fenAfterReboot)), "fen after reboot following sleep flush");
        Check(std::strcmp(fenAfterReboot, fenBeforeUndoSleep) == 0,
              "reboot restores forward position when sleep interrupted undo");
        Check(coordB.GetUndoHistoryMoveCount() == 2U, "undo history preserved for forward position after reboot");
        Check(!smB.IsUndoPromptActive(), "undo prompt not resumed after reboot");

        PressP1(clockB, smB, coordB);
        Check(smB.IsUndoPromptActive(), "undo prompt active after rebooted forward position");
        coordB.Update();

        uint8_t rebootUndoFrom = 0;
        uint8_t rebootUndoTo = 0;
        Check(smB.TryGetUndoPromptMove(rebootUndoFrom, rebootUndoTo), "last undo prompt available after reboot");
        Check(rebootUndoFrom == 11, "rebooted undo targets last move from-square");
        Check(rebootUndoTo == 27, "rebooted undo targets last move to-square");

        char fenAfterSecondUndo[160] = {};
        Check(GetFen(engineB, fenAfterSecondUndo, sizeof(fenAfterSecondUndo)), "fen after rebooted undo");
        Check(std::strcmp(fenAfterSecondUndo, fenBeforeUndoSleep) != 0, "undo after reboot changes engine state");
        Check(std::strcmp(fenAfterSecondUndo, fenAfterFirstUndo) == 0, "undo after reboot reaches the same undone state");
    }

    // Hard reboot without an explicit flush should also restore the last saved
    // forward position instead of auto-completing an unresolved undo prompt.
    {
        modules::SettingsStoreConfig rebootCfg;
        rebootCfg.path = "build/test_undo_suite_unplug_during_undo.json";
        rebootCfg.deferredSaveDelayMs = 100000;
        (void)std::remove(rebootCfg.path);
        (void)std::remove("build/test_undo_suite_unplug_during_undo.json.bak");
        (void)std::remove("build/test_undo_suite_unplug_during_undo.json.tmp");

        runtime::ManualClock clockA(2000);
        runtime::GameStateMachine smA(smCfg);
        smA.Reset();

        modules::SettingsStore settingsA(rebootCfg);
        Check(settingsA.Load(), "settings load (unplug phase A)");

        test_support::BoundedHintEngine engineA;
        engineA.Reset();

        modules::GamePersistenceCoordinator coordA(settingsA, engineA, smA);
        Check(coordA.LoadOrInitialize(), "LoadOrInitialize (unplug phase A)");

        DoMove(clockA, smA, 52, 44, smCfg.settleDurationMs); // e2 -> e3
        coordA.Update();
        DoMove(clockA, smA, 11, 27, smCfg.settleDurationMs); // d7 -> d5
        coordA.Update();
        Check(settingsA.SaveNow(),
              "forward position reaches storage before unplug-undo fixture");

        char fenBeforeUndoUnplug[160] = {};
        Check(GetFen(engineA, fenBeforeUndoUnplug, sizeof(fenBeforeUndoUnplug)), "fen before undo unplug");

        PressP1(clockA, smA, coordA);
        Check(smA.IsUndoPromptActive(), "undo prompt active before unplug");
        coordA.Update();

        char fenAfterUndoPending[160] = {};
        Check(GetFen(engineA, fenAfterUndoPending, sizeof(fenAfterUndoPending)), "fen after undo before unplug");
        Check(std::strcmp(fenBeforeUndoUnplug, fenAfterUndoPending) != 0,
              "undo changes engine state before unplug reboot");

        modules::SettingsStore settingsB(rebootCfg);
        Check(settingsB.Load(), "settings load (unplug phase B)");

        runtime::ManualClock clockB(clockA.NowMs());
        runtime::GameStateMachine smB(smCfg);
        smB.Reset();

        test_support::BoundedHintEngine engineB;
        engineB.Reset();

        modules::GamePersistenceCoordinator coordB(settingsB, engineB, smB);
        Check(coordB.LoadOrInitialize(), "LoadOrInitialize (unplug phase B)");

        char fenAfterUnplugReboot[160] = {};
        Check(GetFen(engineB, fenAfterUnplugReboot, sizeof(fenAfterUnplugReboot)),
              "fen after reboot following unplug during undo");
        Check(std::strcmp(fenAfterUnplugReboot, fenBeforeUndoUnplug) == 0,
              "hard reboot during undo restores the last saved forward position");
        Check(coordB.GetUndoHistoryMoveCount() == 2U, "undo history preserved after unplug reboot");
        Check(!smB.IsUndoPromptActive(), "undo prompt not resumed after unplug reboot");

        PressP1(clockB, smB, coordB);
        Check(smB.IsUndoPromptActive(), "undo prompt can be started again after unplug reboot");
        coordB.Update();

        uint8_t rebootUndoFrom = 0;
        uint8_t rebootUndoTo = 0;
        Check(smB.TryGetUndoPromptMove(rebootUndoFrom, rebootUndoTo),
              "undo prompt move available after unplug reboot");
        Check(rebootUndoFrom == 11, "unplug reboot undo targets last move from-square");
        Check(rebootUndoTo == 27, "unplug reboot undo targets last move to-square");
    }

    if (g_failures == 0) {
        std::puts("[UNDO_SUITE] ok");
        return 0;
    }

    std::printf("[UNDO_SUITE] failures=%d\n", g_failures);
    return 1;
}
