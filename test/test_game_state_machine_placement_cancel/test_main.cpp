#include <cstdio>

#include "foxie_embedded/millis.hpp"
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
    std::puts("[GAME_STATE_MACHINE_PLACEMENT_CANCEL] start");

#ifndef ARDUINO
    fxe::ManualMillis::SetNowMs(1000);
    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);

    runtime::GameStateMachineConfig cfg;
    cfg.placementStableRequiredMs = 200;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    // Create a mismatch and advance time to satisfy the stability guard.
    sm.SetExpectedOccupancyMask(1ULL << 0);
    sm.DebugSetPhysicalOccupancyMask(0);

    fxe::ManualMillis::AdvanceMs(cfg.placementStableRequiredMs);

    const bool entered = sm.TryEnterPlacementMode();
    Check(entered, "entered placement mode");
    Check(sm.IsPlacementModeActive(), "placement mode active");
    Check(sm.IsPlacementModeActive(), "state=Placing (entered)");

    // P1X press must NOT allow exiting placement mode without resolving mismatch.
    sm.HandleButtonEvent(modules::UserButton::P1X, fxe::Button::Event::Press);

    Check(sm.IsPlacementModeActive(), "placement mode still active");
    Check(sm.IsPlacementModeActive(), "state=Placing (after press)");
#else
    std::puts("[GAME_STATE_MACHINE_PLACEMENT_CANCEL] skipped (ARDUINO)");
#endif

    if (g_failures == 0) {
        std::puts("[GAME_STATE_MACHINE_PLACEMENT_CANCEL] ok");
        return 0;
    }

    std::printf("[GAME_STATE_MACHINE_PLACEMENT_CANCEL] failures=%d\n", g_failures);
    return 1;
}
