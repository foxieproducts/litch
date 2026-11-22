#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstdlib>

#include "foxie_embedded/millis.hpp"

#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "modules/vs_cpu_turn.hpp"
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

bool TryFindFirstLegalMove(modules::IEngine& engine,
                           uint8_t& outFrom,
                           uint8_t& outTo) {
    for (uint8_t from = 0; from < 64U; ++from) {
        uint64_t destinations = 0ULL;
        if (!engine.GetLegalDestinationsMask(from, destinations)) {
            continue;
        }
        for (uint8_t to = 0; to < 64U; ++to) {
            if (((destinations >> to) & 1ULL) == 0ULL) {
                continue;
            }
            outFrom = from;
            outTo = to;
            return true;
        }
    }
    return false;
}

void BootPreparationWaitsForUserAction() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    modules::SettingsStoreConfig config;
    config.path = "build/test_game_boot_no_write.json";
    std::remove(config.path);
    std::remove("build/test_game_boot_no_write.json.tmp");
    std::remove("build/test_game_boot_no_write.json.bak");
    modules::SettingsStore settings(config);
    Check(settings.Load(), "boot defaults load in RAM");
    unsigned requests = 0U;
    settings.SetAsyncSaveRequestCallback([&](const JsonDocument&) {
        ++requests;
        return true;
    });
    runtime::GameStateMachine state;
    state.Reset();
    test_support::BoundedHintEngine engine;
    engine.Reset();
    modules::GamePersistenceCoordinator coordinator(settings, engine, state);
    Check(coordinator.LoadOrInitialize(), "automatic boot game prepares in RAM");
    fxe::ManualMillis::AdvanceMs(10000U);
    settings.Update();
    Check(requests == 0U, "automatic boot New Game schedules no settings save");
    Check(coordinator.RequestControlNewGame(1U) == modules::ControlCoordinatorResult::Applied,
          "explicit user New Game is accepted");
    fxe::ManualMillis::AdvanceMs(config.mustSaveDelayMs + 1U);
    settings.Update();
    Check(requests == 1U, "explicit user New Game retains its durable save request");
}

} // namespace

int main() {
    std::puts("[GAME_PERSISTENCE_VS_CPU_LOOP] start");
    BootPreparationWaitsForUserAction();
    if (std::getenv("LITCH_BOOT_NO_WRITE") != nullptr) return g_failures == 0 ? 0 : 1;

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000);

    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_game_persistence_vs_cpu_loop.json";
    cfg.deferredSaveDelayMs = 999999;

    (void)std::remove(cfg.path);
    (void)std::remove("build/test_game_persistence_vs_cpu_loop.json.bak");
    (void)std::remove("build/test_game_persistence_vs_cpu_loop.json.tmp");

    runtime::ManualClock clock(1000);

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 20;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::SettingsStore settings(cfg);
    Check(settings.Load(), "settings load");

    // Start human-vs-human, then enable VS-CPU after White's move. This is the
    // hardware path exercised by the quick-settings toggle in mid-game.
    auto& doc = settings.Doc();
    doc["VS_CPU_LEVEL"] = 0U;
    doc["TIMER"] = 1U;

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    Check(coord.LoadOrInitialize(), "LoadOrInitialize");

    // Start in a playable state: physical occupancy matches expected.
    sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());

    // Start-only placement controls remain available while the untouched
    // opening is in ordinary correction for a missing setup piece.
    const uint64_t openingMask = sm.GetExpectedOccupancyMask();
    sm.DebugSetPhysicalOccupancyMask(openingMask & ~(1ULL << 52U));
    Check(sm.TryEnterPlacementMode(),
          "fresh setup: missing piece enters ordinary correction");
    Check(coord.CheckControlStartOnlyGate("GAME_FIRST_PLAYER") ==
              modules::ControlCoordinatorResult::Applied,
          "fresh setup: player-side/rotate gate remains available during correction");
    sm.Reset();
    engine.Reset();
    Check(coord.LoadOrInitialize(),
          "fresh setup: restore clean opening after start-gate check");
    sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());

    // Default: rotate=0, firstplayer=P1 => White moves first, so CPU (Black) should NOT move first.
    clock.SetNowMs(1205);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "opening: cpu does not move first when rotate=0 and firstplayer=P1");
    Check(!sm.HasQueuedCpuMove(), "opening: no cpu move queued");

    // Human (White) plays first: e2 -> e4.
    clock.AdvanceMs(750U);
    sm.HandleSensorChange(52, false);
    coord.Update();
    coord.UpdateLegalDestinationsMask();
    sm.HandleSensorChange(36, true);
    Check(sm.IsMoveSettlingActive(), "human move enters Settling");

    // Settle timer expects monotonic time.
    clock.AdvanceMs(11U);
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "human settle exposes its action before coordinator acknowledgement");

    coord.Update();
    Check(engine.GetSideToMove() == modules::SideToMove::Black, "engine switches to black to move");
    clock.AdvanceMs(1U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();
    const uint32_t blackClockBeforeCpuTurn = coord.GetGameClock().GetRemainingMs(
        runtime::ClockSide::Black);
    Check(coord.GetGameClock().IsRunning() &&
              !coord.GetGameClock().IsPaused() &&
              coord.GetGameClock().GetActiveSide() == runtime::ClockSide::Black,
          "CPU turn starts with the black clock running");

    doc["VS_CPU_LEVEL"] = 1U;
    settings.NotifyDocChanged();
    Check(settings.SaveNow(), "enable VS-CPU after the human move");

    // Ensure board is still considered matched.
    sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());

    // After idle delay, coordinator should queue a CPU move.
    clock.AdvanceMs(smCfg.cpuIdleDelayMs);
    sm.Tick();
    coord.Update();

    Check((sm.HasQueuedCpuMove() && !sm.IsMoveSettlingActive()), "vs-cpu queues CpuMoving after human opening");
    Check(sm.HasQueuedCpuMove(), "cpu move queued");
    Check(coord.GetGameClock().IsPaused(),
          "queued CPU physical move pauses the clock");
    const uint32_t blackClockAtCpuPrompt = coord.GetGameClock().GetRemainingMs(
        runtime::ClockSide::Black);
    Check(blackClockAtCpuPrompt < blackClockBeforeCpuTurn,
          "CPU clock runs before its physical move is queued");
    clock.AdvanceMs(10U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();
    Check(coord.GetGameClock().GetRemainingMs(runtime::ClockSide::Black) ==
              blackClockAtCpuPrompt,
          "queued CPU physical move does not consume CPU time");

    uint8_t queuedFrom = 255;
    uint8_t queuedTo = 255;
    bool queuedCaptureValid = false;
    uint8_t queuedCaptureSquare = 255;
    Check(sm.TryGetQueuedCpuMove(queuedFrom, queuedTo, queuedCaptureValid, queuedCaptureSquare),
          "queued cpu move readable");
    Check(settings.SaveNow(),
          "queued CPU prompt reaches storage after debounce simulation");

    // Reboot while waiting for physical CPU move: queued move must survive and
    // must not be recomputed to a different move.
    {
        modules::SettingsStore settingsAfterReboot(cfg);
        Check(settingsAfterReboot.Load(), "settings reload after reboot during cpu prompt");

        runtime::GameStateMachine smAfterReboot(smCfg);
        smAfterReboot.Reset();

        test_support::BoundedHintEngine engineAfterReboot;
        engineAfterReboot.Reset();

        modules::GamePersistenceCoordinator coordAfterReboot(settingsAfterReboot, engineAfterReboot, smAfterReboot);
        Check(coordAfterReboot.LoadOrInitialize(), "LoadOrInitialize after reboot during cpu prompt");

        Check((smAfterReboot.HasQueuedCpuMove() && !smAfterReboot.IsMoveSettlingActive()),
              "reboot restore: still waiting in CpuMoving");
        Check(smAfterReboot.HasQueuedCpuMove(), "reboot restore: queued cpu move still active");

        uint8_t restoredFrom = 255;
        uint8_t restoredTo = 255;
        bool restoredCaptureValid = false;
        uint8_t restoredCaptureSquare = 255;
        Check(smAfterReboot.TryGetQueuedCpuMove(restoredFrom, restoredTo, restoredCaptureValid, restoredCaptureSquare),
              "reboot restore: queued move readable");
        Check(restoredFrom == queuedFrom && restoredTo == queuedTo,
              "reboot restore: cpu move from/to unchanged");
    }

    // The same cancellation must work after the player has started following
    // the CPU prompt. It should immediately hand control to the prior White
    // move's undo prompt.
    {
        modules::SettingsStore settingsAfterReboot(cfg);
        Check(settingsAfterReboot.Load(),
              "settings reload for in-progress cpu undo");
        runtime::GameStateMachine smAfterReboot(smCfg);
        smAfterReboot.Reset();
        test_support::BoundedHintEngine engineAfterReboot;
        engineAfterReboot.Reset();
        modules::GamePersistenceCoordinator coordAfterReboot(
            settingsAfterReboot, engineAfterReboot, smAfterReboot);
        Check(coordAfterReboot.LoadOrInitialize(),
              "load queued cpu move for in-progress undo");
        smAfterReboot.DebugSetPhysicalOccupancyMask(
            smAfterReboot.GetExpectedOccupancyMask());
        smAfterReboot.HandleSensorChange(queuedFrom, false);
        Check(coordAfterReboot.RequestUndo(1U),
              "undo while lifting queued cpu piece succeeds");
        modules::PersistedSide undoMover = modules::PersistedSide::Unknown;
        Check(!smAfterReboot.HasQueuedCpuMove() &&
                  smAfterReboot.IsUndoPromptActive() &&
                  smAfterReboot.TryGetUndoPromptMoverSide(undoMover) &&
                  undoMover == modules::PersistedSide::White,
              "in-progress cpu cancellation prompts White to undo");
    }

    // If the guided CPU placement reaches the action boundary before the
    // button event, first restore that uncommitted physical move, then retain
    // the request and open White's undo prompt as soon as correction finishes.
    {
        modules::SettingsStore settingsAfterReboot(cfg);
        Check(settingsAfterReboot.Load(),
              "settings reload for pre-ack cpu undo");
        runtime::GameStateMachine smAfterReboot(smCfg);
        smAfterReboot.Reset();
        test_support::BoundedHintEngine engineAfterReboot;
        engineAfterReboot.Reset();
        modules::GamePersistenceCoordinator coordAfterReboot(
            settingsAfterReboot, engineAfterReboot, smAfterReboot);
        Check(coordAfterReboot.LoadOrInitialize(),
              "load queued cpu move for pre-ack undo");
        uint64_t physical = smAfterReboot.GetExpectedOccupancyMask();
        smAfterReboot.DebugSetPhysicalOccupancyMask(physical);
        physical &= ~(1ULL << queuedFrom);
        smAfterReboot.HandleSensorChange(queuedFrom, false);
        physical |= 1ULL << queuedTo;
        smAfterReboot.HandleSensorChange(queuedTo, true);
        fxe::ManualMillis::AdvanceMs(smCfg.settleDurationMs);
        smAfterReboot.Tick();
        Check(smAfterReboot.IsMoveActionAwaitingResult(),
              "cpu placement reaches pre-ack boundary");
        Check(coordAfterReboot.RequestUndo(1U),
              "undo at pre-ack cpu boundary succeeds");
        Check(!smAfterReboot.HasQueuedCpuMove() &&
                  !smAfterReboot.IsMoveActionAwaitingResult(),
              "pre-ack undo stops the uncommitted cpu move");
        physical &= ~(1ULL << queuedTo);
        physical |= 1ULL << queuedFrom;
        smAfterReboot.ApplyPhysicalOccupancySnapshot(physical);
        coordAfterReboot.Update();
        modules::PersistedSide undoMover = modules::PersistedSide::Unknown;
        Check(smAfterReboot.IsUndoPromptActive() &&
                  smAfterReboot.TryGetUndoPromptMoverSide(undoMover) &&
                  undoMover == modules::PersistedSide::White,
              "corrected pre-ack cpu move then prompts White to undo");
    }

    // Undo after reboot must still cancel the restored CPU prompt and undo the
    // last committed human move.
    {
        modules::SettingsStore settingsAfterReboot(cfg);
        Check(settingsAfterReboot.Load(), "settings reload after reboot for undo");

        runtime::GameStateMachine smAfterReboot(smCfg);
        smAfterReboot.Reset();

        test_support::BoundedHintEngine engineAfterReboot;
        engineAfterReboot.Reset();

        modules::GamePersistenceCoordinator coordAfterReboot(settingsAfterReboot, engineAfterReboot, smAfterReboot);
        Check(coordAfterReboot.LoadOrInitialize(), "LoadOrInitialize after reboot for undo");

        Check(coordAfterReboot.RequestUndo(2U), "reboot restore: undo during restored cpu prompt succeeds");
        Check(!smAfterReboot.HasQueuedCpuMove(), "reboot restore: undo cancels restored cpu prompt");
        Check(smAfterReboot.IsUndoPromptActive(), "reboot restore: undo prompt active after canceling restored cpu prompt");

        coordAfterReboot.Update();
        Check(engineAfterReboot.GetSideToMove() == modules::SideToMove::White,
              "reboot restore: undo during restored cpu prompt returns to human-to-move");
    }

    // Undo during queued CPU prompt should cancel that prompt and undo the
    // prior human move (engine-side).
    Check(coord.RequestUndo(2U), "undo during queued cpu prompt succeeds");
    Check(!sm.HasQueuedCpuMove(), "undo cancels queued cpu prompt");
    Check(sm.IsUndoPromptActive(), "undo prompt active after canceling queued cpu prompt");

    coord.Update();
    Check(engine.GetSideToMove() == modules::SideToMove::White,
          "undo after queued cpu prompt reverts to human-to-move");

    // With CPU playing Black in this scenario, no immediate CPU requeue should occur.
    Check(!sm.HasQueuedCpuMove(), "no immediate cpu requeue after undo when cpu is black");

    // Continue remaining assertions from a clean baseline on the original fixtures.
    sm.Reset();
    engine.Reset();
    modules::ClearGameState(settings.Doc());
    modules::ClearUndoState(settings.Doc());
    settings.SaveNow();
    Check(coord.LoadOrInitialize(), "LoadOrInitialize after reboot simulation");

    // Sanity: rotate must not change who is CPU (default configuration is CPU=P2).
    settings.Doc()["BOARD_ROTATE"] = true;
    settings.SaveNow();
    sm.Reset();
    engine.Reset();
    Check(coord.LoadOrInitialize(), "LoadOrInitialize after rotate");
    sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());

    auto ClearActiveKingQueenSwapPrompt = [&]() {
        uint64_t mask64 = 0;
        Check(sm.TryGetKingQueenSwapPromptMask(mask64), "kq-swap prompt active");
        for (uint8_t sq = 0; sq < 64; ++sq) {
            if (((mask64 >> sq) & 1ULL) != 0ULL) {
                sm.HandleSensorChange(sq, false);
                sm.HandleSensorChange(sq, true);
            }
        }
        clock.AdvanceMs(1);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        sm.Tick();
        Check(!sm.IsKingQueenSwapPromptActive(), "kq-swap prompt cleared");
    };

    clock.SetNowMs(2000);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();

    // Rotate toggles at the initial FEN can start a setup prompt requiring
    // lifting/restoring king/queen squares before play can continue.
    if (sm.IsKingQueenSwapPromptActive()) {
        ClearActiveKingQueenSwapPrompt();
    }
    coord.Update();

    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "rotate=1: cpu does not move first when firstplayer=P1");
    Check(!sm.HasQueuedCpuMove(), "rotate=1: no cpu move queued");

    // Moving P1 to the opposite physical end never changes CPU=P2 from Black.
    settings.Doc()["GAME_FIRST_PLAYER"] = 2U;
    settings.SaveNow();
    sm.Reset();
    engine.Reset();
    Check(coord.LoadOrInitialize(), "LoadOrInitialize after firstplayer=P2");
    sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());

    clock.SetNowMs(3000);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();

    // If the rotate setup prompt is still active, clear it before asserting CPU turn.
    if (sm.IsKingQueenSwapPromptActive()) {
        ClearActiveKingQueenSwapPrompt();
        coord.Update();
        if (sm.IsKingQueenSwapPromptActive()) {
            ClearActiveKingQueenSwapPrompt();
            coord.Update();
        }

        // Clearing the prompt resets the coordinator's "idle" timing; ensure we
        // wait long enough for the CPU idle delay before asserting CpuMoving.
        clock.AdvanceMs(smCfg.cpuIdleDelayMs + 5U);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        sm.Tick();
        coord.Update();
    }

    Check(!sm.HasQueuedCpuMove(),
          "opposite player end: CPU=P2 remains Black and does not move first");

    // Changing physical player-end placement cannot create or discard a CPU turn.
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;
    settings.SaveNow();
    coord.Update();
    Check(!sm.HasQueuedCpuMove(),
          "player-end change leaves CPU=P2 waiting for Black");
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle),
          "player-end change returns setup to Idle");

    // With CPU now playing Black at White-to-move, CPU must not requeue.
    clock.AdvanceMs(smCfg.cpuIdleDelayMs + 5U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();
    Check(!sm.HasQueuedCpuMove(), "firstplayer=1: cpu does not queue opening move");

    // CPU=P1 must work as a complete play loop, not only as a turn-policy
    // calculation. The CPU opens as White, the human replies as Black, and one
    // undo must preserve the CPU opening while removing the human reply.
    settings.Doc()["BOARD_ROTATE"] = false;
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;
    settings.Doc()["VS_CPU_PLAYER"] = 1U;
    modules::ClearGameState(settings.Doc());
    modules::ClearUndoState(settings.Doc());
    Check(settings.SaveNow(), "cpu=P1: save clean configuration");
    sm.Reset();
    engine.Reset();
    Check(coord.LoadOrInitialize(), "cpu=P1: initialize clean game");
    sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());

    clock.AdvanceMs(smCfg.cpuIdleDelayMs + 5U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();
    Check(sm.HasQueuedCpuMove(), "cpu=P1: White opening move is queued");

    uint8_t cpuOpeningFrom = 255U;
    uint8_t cpuOpeningTo = 255U;
    bool cpuOpeningCaptureValid = false;
    uint8_t cpuOpeningCaptureSquare = 255U;
    Check(sm.TryGetQueuedCpuMove(cpuOpeningFrom,
                                 cpuOpeningTo,
                                 cpuOpeningCaptureValid,
                                 cpuOpeningCaptureSquare),
          "cpu=P1: queued White opening is readable");
    Check(!coord.GetGameClock().IsRunning(),
          "cpu=P1: queued opening remains stopped during the arming window");
    clock.AdvanceMs(750U);
    sm.Tick();
    coord.Update();
    Check(coord.GetGameClock().IsRunning() &&
              coord.GetGameClock().IsPaused() &&
              coord.GetGameClock().GetActiveSide() ==
                  runtime::ClockSide::White,
          "cpu=P1: arming boundary starts then pauses the queued CPU clock");
    sm.HandleSensorChange(cpuOpeningFrom, false);
    sm.HandleSensorChange(cpuOpeningTo, true);
    clock.AdvanceMs(smCfg.settleDurationMs + 1U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();
    Check(engine.GetSideToMove() == modules::SideToMove::Black,
          "cpu=P1: completed opening hands the turn to human Black");
    Check(coord.GetGameClock().GetActiveSide() == runtime::ClockSide::Black,
          "cpu=P1: physical CPU commit switches the active clock to human Black");
    clock.AdvanceMs(10U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();
    Check(!coord.GetGameClock().IsPaused() &&
              coord.GetGameClock().IsRunning(),
          "cpu=P1: human clock resumes after physical CPU commit");
    Check(coord.GetUndoHistoryMoveCount() == 1U,
          "cpu=P1: opening is the first persisted undo ply");

    // At the first human decision boundary, Undo removes the completed CPU
    // opening (the requested two-ply cycle clamps to the one available ply),
    // then CPU=P1 performs a fresh opening think after correction completes.
    const uint8_t openingUndoCount = modules::GetVsCpuUndoPlyCount(
        engine.GetSideToMove(), 1U, true, false);
    Check(openingUndoCount == 2U,
          "cpu=P1: completed opening selects a full decision-boundary undo");
    Check(coord.RequestUndo(openingUndoCount),
          "cpu=P1: completed opening undo starts");
    coord.Update();
    Check(sm.IsUndoPromptActive(),
          "cpu=P1: completed opening requests physical correction");
    sm.HandleSensorChange(cpuOpeningTo, false);
    sm.HandleSensorChange(cpuOpeningFrom, true);
    coord.Update();
    Check(engine.GetSideToMove() == modules::SideToMove::White &&
              coord.GetUndoHistoryMoveCount() == 0U,
          "cpu=P1: opening undo restores untouched White boundary");

    clock.AdvanceMs(smCfg.cpuIdleDelayMs + 5U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();
    Check(sm.HasQueuedCpuMove(),
          "cpu=P1: completed opening undo triggers a fresh think");
    Check(sm.TryGetQueuedCpuMove(cpuOpeningFrom,
                                 cpuOpeningTo,
                                 cpuOpeningCaptureValid,
                                 cpuOpeningCaptureSquare),
          "cpu=P1: rethought opening is readable");
    sm.HandleSensorChange(cpuOpeningFrom, false);
    sm.HandleSensorChange(cpuOpeningTo, true);
    clock.AdvanceMs(smCfg.settleDurationMs + 1U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();
    Check(engine.GetSideToMove() == modules::SideToMove::Black &&
              coord.GetUndoHistoryMoveCount() == 1U,
          "cpu=P1: rethought opening returns to the human decision boundary");

    uint8_t humanFrom = 255U;
    uint8_t humanTo = 255U;
    Check(TryFindFirstLegalMove(engine, humanFrom, humanTo),
          "cpu=P1: human Black has a legal reply");
    sm.HandleSensorChange(humanFrom, false);
    coord.UpdateLegalDestinationsMask();
    sm.HandleSensorChange(humanTo, true);
    clock.AdvanceMs(smCfg.settleDurationMs + 1U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();
    Check(engine.GetSideToMove() == modules::SideToMove::White,
          "cpu=P1: human reply returns the turn to CPU White");
    Check(coord.GetUndoHistoryMoveCount() == 2U,
          "cpu=P1: human reply is recorded after the CPU opening");

    const uint8_t cpuP1UndoCount = modules::GetVsCpuUndoPlyCount(
        engine.GetSideToMove(), 1U, false, false);
    Check(cpuP1UndoCount == 1U,
          "cpu=P1: undo before the CPU reply removes only the human move");
    Check(coord.RequestUndo(cpuP1UndoCount),
          "cpu=P1: undo after the human reply succeeds");
    coord.Update();
    Check(engine.GetSideToMove() == modules::SideToMove::Black,
          "cpu=P1: undo restores the human turn after the CPU opening");
    Check(coord.GetUndoHistoryMoveCount() == 1U,
          "cpu=P1: undo preserves the CPU opening history ply");

    if (g_failures == 0) {
        std::puts("[GAME_PERSISTENCE_VS_CPU_LOOP] ok");
        return 0;
    }

    std::printf("[GAME_PERSISTENCE_VS_CPU_LOOP] failures=%d\n", g_failures);
    return 1;
}
