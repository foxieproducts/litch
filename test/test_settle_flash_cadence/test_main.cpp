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
    std::puts("[SETTLE_FLASH_CADENCE] start");

    fxe::ScopedMillisProviderOverride millisGuard(
        &fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    runtime::ManualClock clock(0);
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 1000;
    cfg.settleFlashMs = 100;
    cfg.cpuIdleDelayMs = 999999;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    sm.HandleInteractionPositionAcknowledged(1ULL << 4U, 0ULL, 0ULL,
                                             1ULL << 4U);
    sm.ApplyPhysicalOccupancySnapshot(1ULL << 4U);
    sm.HandleSensorChange(4U, false);
    sm.HandleSensorChange(12U, true);
    Check(sm.IsMoveSettlingActive(), "real move starts settle cadence");

    // Flash is a pure function of time and configured cadence.
    clock.SetNowMs(0);
    Check(sm.IsSettleFlashOn(), "flash on at t=0");
    clock.SetNowMs(99);
    Check(!sm.IsSettleFlashOn(), "accelerated flash is off near 100ms");
    clock.SetNowMs(100);
    Check(!sm.IsSettleFlashOn(), "flash off at first toggle");
    clock.SetNowMs(199);
    Check(sm.IsSettleFlashOn(), "accelerated flash is on near 200ms");
    clock.SetNowMs(200);
    Check(sm.IsSettleFlashOn(), "flash on at second toggle");

    if (g_failures == 0) {
        std::puts("[SETTLE_FLASH_CADENCE] ok");
        return 0;
    }

    std::printf("[SETTLE_FLASH_CADENCE] failures=%d\n", g_failures);
    return 1;
}
