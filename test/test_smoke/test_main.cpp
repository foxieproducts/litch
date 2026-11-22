#include <cstdio>

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

} // namespace

int main() {
    std::puts("[SMOKE] start");

    runtime::ManualClock clock(1000);
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 100;
    cfg.cpuIdleDelayMs = 50;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "starts in Idle");

    // Idle timeout should not fire before delay.
    clock.AdvanceMs(49);
    sm.Tick();
    Check(!sm.ConsumeCpuIdleTimeoutFired(), "idle timeout not fired early");

    // Now it should fire.
    clock.AdvanceMs(1);
    sm.Tick();
    Check(sm.ConsumeCpuIdleTimeoutFired(), "idle timeout fires at threshold");

    // Any sensor interaction should clear/reset idle timeout.
    sm.HandleSensorChange(12, false); // removed
    Check(sm.IsHumanPieceInHand(), "Idle->PickedUp");

    // Placing the piece starts settling.
    sm.HandleSensorChange(20, true); // placed
    Check(sm.IsMoveSettlingActive(), "PickedUp->Settling");

    // Before settle duration elapsed, stay in Settling.
    clock.AdvanceMs(99);
    sm.Tick();
    Check(sm.IsMoveSettlingActive(),
          "settle not complete early");

    // At duration, finalize to Idle.
    clock.AdvanceMs(1);
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "settle timeout exposes its action");

    if (g_failures == 0) {
        std::puts("[SMOKE] ok");
        return 0;
    }

    std::printf("[SMOKE] failures=%d\n", g_failures);
    return 1;
}
