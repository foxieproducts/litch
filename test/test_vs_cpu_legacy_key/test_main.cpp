#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstdlib>

#include "foxie_embedded/millis.hpp"

#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
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
    std::puts("[VS_CPU_SETTING] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000);

    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_vs_cpu_legacy_key.json";
    cfg.deferredSaveDelayMs = 999999;

    (void)std::remove(cfg.path);
    (void)std::remove("build/test_vs_cpu_legacy_key.json.bak");
    (void)std::remove("build/test_vs_cpu_legacy_key.json.tmp");

    runtime::ManualClock clock(1000);

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 20;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::SettingsStore settings(cfg);
    Check(settings.Load(), "settings load");

    // Canonical VS-CPU key.
    auto& doc = settings.Doc();
    doc["VS_CPU_LEVEL"] = 1U;
    settings.SaveNow();

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    Check(coord.LoadOrInitialize(), "LoadOrInitialize");

    // Start in a playable state: physical occupancy matches expected.
    sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());

    // Human (White) plays first: e2 -> e4.
    sm.HandleSensorChange(52, false);
    coord.UpdateLegalDestinationsMask();
    sm.HandleSensorChange(36, true);
    Check(sm.IsMoveSettlingActive(), "human move enters Settling");

    // Settle timer expects monotonic time.
    clock.SetNowMs(1216);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "human settle exposes its action");

    coord.Update();
    Check(engine.GetSideToMove() == modules::SideToMove::Black, "engine switches to black to move");

    // Ensure board is still considered matched.
    sm.DebugSetPhysicalOccupancyMask(sm.GetExpectedOccupancyMask());

    // After idle delay, coordinator should queue a CPU move.
    clock.SetNowMs(1237);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    coord.Update();

    Check((sm.HasQueuedCpuMove() && !sm.IsMoveSettlingActive()), "vs-cpu queues CpuMoving with VS_CPU_LEVEL key");
    Check(sm.HasQueuedCpuMove(), "cpu move queued");

    if (g_failures == 0) {
        std::puts("[VS_CPU_SETTING] ok");
        return 0;
    }

    std::printf("[VS_CPU_SETTING] failures=%d\n", g_failures);
    return 1;
}
