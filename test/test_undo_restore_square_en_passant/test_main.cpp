#include "bounded_hint_engine.hpp"
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"

#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/app_event.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

void Sensor(runtime::ManualClock& clock, runtime::GameStateMachine& sm, const uint8_t square, const bool occ) {
    runtime::DispatchAppEventToStateMachine(sm, runtime::AppEvent::MakeSensorDelta(clock.NowMs(), square, occ));
}

void Tick(runtime::ManualClock& clock, runtime::GameStateMachine& sm, const uint32_t deltaMs) {
    clock.AdvanceMs(deltaMs);
    runtime::DispatchAppEventToStateMachine(sm, runtime::AppEvent::MakeTick(clock.NowMs()));
}

void PressUndo(runtime::ManualClock& clock, runtime::GameStateMachine& sm) {
    runtime::DispatchAppEventToStateMachine(
        sm, runtime::AppEvent::MakeButton(clock.NowMs(), modules::UserButton::P1X, fxe::Button::Event::Press));
}

} // namespace

int main() {
    fxe::test_support::TestResults results("UNDO_RESTORE_SQUARE_EN_PASSANT");
    results.Start();

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_undo_restore_square_en_passant.json";

    (void)std::remove(settingsCfg.path);
    (void)std::remove("build/test_undo_restore_square_en_passant.json.bak");
    (void)std::remove("build/test_undo_restore_square_en_passant.json.tmp");

    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    test_support::BoundedHintEngine engine;
    engine.Reset();

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 50;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    // Coordinator keeps cached FEN/undo history and configures undo metadata.
    modules::GamePersistenceCoordinator coord(settings, engine, sm);

    results.Check(coord.LoadOrInitialize(), "LoadOrInitialize");

    // Seed physical occupancy to match the engine-derived expected occupancy.
    // Use the state machine's expected occupancy after load/init.
#ifndef ARDUINO
    sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());
#endif

    runtime::ManualClock clock(0);

    // Reach a position where en-passant is available:
    // e2e4 a7a6 e4e5 d7d5
    static constexpr uint8_t E2 = 52;
    static constexpr uint8_t E4 = 36;
    static constexpr uint8_t E5 = 28;
    static constexpr uint8_t A7 = 8;
    static constexpr uint8_t A6 = 16;
    static constexpr uint8_t D7 = 11;
    static constexpr uint8_t D5 = 27;

    // e2 -> e4
    Sensor(clock, sm, E2, false);
    results.Check(sm.IsHumanPieceInHand(), "e2 pickup enters PickedUp");
    sm.SetValidDestMask(1ULL << E4);
    Sensor(clock, sm, E4, true);
    results.Check(sm.IsMoveSettlingActive(), "e4 place enters Settling");
    Tick(clock, sm, 60);
    results.Check(sm.GetChessInteractionModel().phase.tag ==
                      runtime::ChessInteractionPhase::AwaitingAction,
                  "e2e4 exposes its action");
    coord.Update();

    // a7 -> a6
    Sensor(clock, sm, A7, false);
    results.Check(sm.IsHumanPieceInHand(), "a7 pickup enters PickedUp");
    sm.SetValidDestMask(1ULL << A6);
    Sensor(clock, sm, A6, true);
    results.Check(sm.IsMoveSettlingActive(), "a6 place enters Settling");
    Tick(clock, sm, 60);
    results.Check(sm.GetChessInteractionModel().phase.tag ==
                      runtime::ChessInteractionPhase::AwaitingAction,
                  "a7a6 exposes its action");
    coord.Update();

    // e4 -> e5
    Sensor(clock, sm, E4, false);
    results.Check(sm.IsHumanPieceInHand(), "e4 pickup enters PickedUp");
    sm.SetValidDestMask(1ULL << E5);
    Sensor(clock, sm, E5, true);
    results.Check(sm.IsMoveSettlingActive(), "e5 place enters Settling");
    Tick(clock, sm, 60);
    results.Check(sm.GetChessInteractionModel().phase.tag ==
                      runtime::ChessInteractionPhase::AwaitingAction,
                  "e4e5 exposes its action");
    coord.Update();

    // d7 -> d5
    Sensor(clock, sm, D7, false);
    results.Check(sm.IsHumanPieceInHand(), "d7 pickup enters PickedUp");
    sm.SetValidDestMask(1ULL << D5);
    Sensor(clock, sm, D5, true);
    results.Check(sm.IsMoveSettlingActive(), "d5 place enters Settling");
    Tick(clock, sm, 60);
    results.Check(sm.GetChessInteractionModel().phase.tag ==
                      runtime::ChessInteractionPhase::AwaitingAction,
                  "d7d5 exposes its action");
    coord.Update();

    // En-passant move: e5d6 captures pawn on d5.
    static constexpr uint8_t TO_D6 = 19;
    static constexpr uint8_t CAP_D5 = D5;

    Sensor(clock, sm, E5, false);
    results.Check(sm.IsHumanPieceInHand(), "e5 pickup enters PickedUp");
    sm.SetValidDestMask(1ULL << TO_D6);
    Sensor(clock, sm, TO_D6, true);
    results.Check(sm.IsMoveSettlingActive(), "d6 place enters Settling");

    // En-passant requires captured pawn square to clear before finalize.
    Sensor(clock, sm, CAP_D5, false);
    results.Check(sm.GetChessInteractionModel().phase.tag ==
                      runtime::ChessInteractionPhase::AwaitingAction,
                  "en-passant exposes its action");

    coord.Update();

    // Request undo and let coordinator configure undo prompt metadata (restore square).
    results.Check(coord.RequestUndo(1U), "undo request accepted");
    results.Check(sm.IsUndoPromptActive(), "undo prompt active");
    coord.Update();

    uint8_t restoreSq = 0;
    char restorePiece = '\0';
    results.Check(sm.TryGetUndoRestoreInfo(restoreSq, restorePiece), "undo restore info available");
    results.Check(restoreSq == CAP_D5, "undo restore square is en-passant captured pawn square");
    results.Check(restorePiece == 'p', "undo restore piece is black pawn");

    return results.Finish();
}
