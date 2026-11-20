#include <cstdio>

#include "foxie_embedded/millis.hpp"
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
    std::puts("[ILLEGAL_SUITE] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    runtime::ManualClock clock(0);
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 100;
    cfg.cpuIdleDelayMs = 999999;
    cfg.illegalFlashMs = 300;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    // Flash cadence sanity.
    fxe::ManualMillis::SetNowMs(0);
    Check(sm.IsIllegalFlashOn(), "illegal flash on at t=0");
    fxe::ManualMillis::SetNowMs(299);
    Check(sm.IsIllegalFlashOn(), "illegal flash still on before toggle");
    fxe::ManualMillis::SetNowMs(300);
    Check(!sm.IsIllegalFlashOn(), "illegal flash toggles at period");

    // Placing while idle latches human-illegal.
    sm.HandleSensorChange(50, true);
    Check(sm.HasHumanIllegal(), "idle placement latches illegal");
    Check(sm.IsIllegalFlashOn(), "new illegal restarts flash on-phase immediately");

    // While illegal is active, pickup is ignored.
    sm.HandleSensorChange(12, false);
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "pickup ignored while illegal active");

    // Interacting on the illegal square clears it.
    sm.HandleSensorChange(50, false);
    Check(!sm.HasHumanIllegal(), "illegal clears on interaction");

    // Auto-clear: if board matches expected occupancy everywhere, illegal clears.
    sm.Reset();
    fxe::ManualMillis::SetNowMs(300);
    sm.HandleSensorChange(40, true);
    Check(sm.HasHumanIllegal(), "illegal latched (phase 2)");
    Check(sm.IsIllegalFlashOn(), "phase resets when illegal reappears");

    // Do not explicitly clear via interaction; instead set expected to match physical and tick.
    sm.SetExpectedOccupancyMask(1ULL << 40);
    sm.Tick();
    Check(!sm.HasHumanIllegal(), "illegal auto-clears when physical==expected");

    // CPU prompt lifecycle is not itself correction feedback. Queueing or
    // canceling a prompt with no CPU-illegal markers must leave the shared
    // cadence untouched.
    fxe::ManualMillis::SetNowMs(1000);
    runtime::GameStateMachine cpuSm(cfg);
    cpuSm.Reset();
    const uint64_t cpuSource = 1ULL << 8U;
    cpuSm.SetExpectedOccupancyMask(cpuSource);
    cpuSm.SetExpectedMoverAndOpponentOccupancyMasks(cpuSource, 0ULL);
    cpuSm.DebugSetPhysicalOccupancyMask(cpuSource);

    fxe::ManualMillis::SetNowMs(1300);
    Check(!cpuSm.IsIllegalFlashOn(),
          "shared cadence reaches off-phase before CPU prompt");
    Check(cpuSm.QueueCpuMove(8U, 16U, false, 0U, '\0'),
          "CPU prompt queues for cadence audit");
    Check(!cpuSm.IsIllegalFlashOn(),
          "queue without CPU-illegal markers preserves cadence");

    fxe::ManualMillis::SetNowMs(1301);
    cpuSm.CancelQueuedCpuMove();
    Check(!cpuSm.IsIllegalFlashOn(),
          "cancel without CPU-illegal markers preserves cadence");

    if (g_failures == 0) {
        std::puts("[ILLEGAL_SUITE] ok");
        return 0;
    }

    std::printf("[ILLEGAL_SUITE] failures=%d\n", g_failures);
    return 1;
}
