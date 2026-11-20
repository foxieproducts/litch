#include "bounded_hint_engine.hpp"
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/settings_store.hpp"
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

bool WaitForHintReady(modules::GamePersistenceCoordinator& coord,
                      runtime::GameStateMachine& sm,
                      const int maxTicks) {
    for (int i = 0; i < maxTicks; ++i) {
        fxe::ManualMillis::AdvanceMs(100U);
        coord.Update();
    }
    // Finish the finite refinement ladder before testing that taps only
    // change visibility. Exact first-result timing has its own worker journey.
    uint8_t fromSquare = 0;
    uint8_t toSquare = 0;
    return !sm.IsHintThinkingActive() && sm.TryGetHintMoveSquares(fromSquare, toSquare);
}

} // namespace

int main() {
    std::puts("[HINT_TAP_TOGGLE] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig revealCfg;
    revealCfg.path = "build/test_hint_tap_toggle_reveal.json";
    revealCfg.deferredSaveDelayMs = 100000;
    (void)std::remove(revealCfg.path);
    (void)std::remove("build/test_hint_tap_toggle_reveal.json.bak");
    (void)std::remove("build/test_hint_tap_toggle_reveal.json.tmp");

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 0;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    {
        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::SettingsStore settings(revealCfg);
        Check(settings.Load(), "settings load (reveal)");

        test_support::BoundedHintEngine engine;
        engine.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (reveal)");
        runtime::InteractionDispatcher dispatcher(sm, coord);
        const uint64_t occupied = sm.GetExpectedOccupancyMask();
        (void)dispatcher.DispatchBoardObservation({fxe::Millis(), 1U, occupied, occupied});
        Check(WaitForHintReady(coord, sm, 24), "hint becomes ready without button demand");
        Check(!sm.IsHintDisplayRequested(), "hint starts hidden");

        uint8_t readyFrom = 0;
        uint8_t readyTo = 0;
        Check(sm.TryGetHintMoveSquares(readyFrom, readyTo), "cached hint squares available");

        sm.HandleHintButtonTap();
        Check(sm.IsHintDisplayRequested(), "tap reveals cached hint");
        coord.Update();

        uint8_t shownFrom = 0;
        uint8_t shownTo = 0;
        Check(sm.TryGetHintMoveSquares(shownFrom, shownTo), "revealed hint keeps cached move");
        Check(shownFrom == readyFrom, "revealed hint keeps from-square");
        Check(shownTo == readyTo, "revealed hint keeps to-square");
        Check(!sm.IsHintThinkingActive(), "revealing cached hint does not restart thinking");

        sm.HandleHintButtonTap();
        coord.Update();
        Check(!sm.IsHintDisplayRequested(), "second tap hides hint");
        Check(!sm.IsHintThinkingActive(), "hiding a ready hint does not restart thinking");
        Check(sm.TryGetHintMoveSquares(shownFrom, shownTo), "hiding keeps cached hint move");
        Check(shownFrom == readyFrom, "hidden cached hint keeps from-square");
        Check(shownTo == readyTo, "hidden cached hint keeps to-square");

        sm.HandleHintButtonTap();
        Check(sm.IsHintDisplayRequested(), "third tap shows cached hint again");
        Check(sm.TryGetHintMoveSquares(shownFrom, shownTo), "cached hint remains available after re-show");
    }

    modules::SettingsStoreConfig cancelCfg;
    cancelCfg.path = "build/test_hint_tap_toggle_cancel.json";
    cancelCfg.deferredSaveDelayMs = 100000;
    (void)std::remove(cancelCfg.path);
    (void)std::remove("build/test_hint_tap_toggle_cancel.json.bak");
    (void)std::remove("build/test_hint_tap_toggle_cancel.json.tmp");

    {
        runtime::GameStateMachine sm(smCfg);
        sm.Reset();

        modules::SettingsStore settings(cancelCfg);
        Check(settings.Load(), "settings load (cancel)");

        test_support::BoundedHintEngine engine;
        engine.Reset();

        modules::GamePersistenceCoordinator coord(settings, engine, sm);
        Check(coord.LoadOrInitialize(), "LoadOrInitialize (cancel)");
        runtime::InteractionDispatcher dispatcher(sm, coord);
        const uint64_t occupied = sm.GetExpectedOccupancyMask();
        (void)dispatcher.DispatchBoardObservation({fxe::Millis(), 1U, occupied, occupied});

        sm.HandleHintButtonTap();
        coord.Update();
        Check(sm.IsHintDisplayRequested(), "pending hint request stays armed");

        uint8_t fromSquare = 0;
        uint8_t toSquare = 0;
        // The bounded synchronous helper has no calculation minimum. The
        // actual worker/mailbox journey owns the 499/500 ms reveal boundary.

        Check(WaitForHintReady(coord, sm, 24), "pending hint request eventually completes");
        Check(sm.IsHintDisplayRequested(), "pending request stays armed until ready");
        Check(sm.TryGetHintMoveSquares(fromSquare, toSquare), "pending request eventually gets a cached move");
    }

    // Pending LED delivery is covered through raw O input and final physical
    // output in test_chess_assistance_feedback_virtual_litch_journey. Keep this
    // suite focused on the real coordinator's visibility/cache lifecycle.

    if (g_failures == 0) {
        std::puts("[HINT_TAP_TOGGLE] ok");
        return 0;
    }

    std::printf("[HINT_TAP_TOGGLE] failures=%d\n", g_failures);
    return 1;
}
