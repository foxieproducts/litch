#include "bounded_hint_engine.hpp"
#include <cstdio>

#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "trace/trace_replay.hpp"

namespace {

constexpr const char* kSettingsPath = "build/test_trace_replay_file.json";

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void RemoveSettingsFiles() {
    (void)std::remove(kSettingsPath);
    (void)std::remove("build/test_trace_replay_file.json.bak");
    (void)std::remove("build/test_trace_replay_file.json.tmp");
}

} // namespace

int main() {
    std::puts("[TRACE_REPLAY_FILE] start");
    RemoveSettingsFiles();

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = kSettingsPath;
    settingsCfg.deferredSaveDelayMs = 100000;

    modules::SettingsStore settings(settingsCfg);
    Check(settings.Load(), "settings load");

    runtime::ManualClock clock(0);
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 100;
    cfg.cpuIdleDelayMs = 999999;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator gamePersistence(settings, engine, sm);
    Check(gamePersistence.LoadOrInitialize(), "persistence load/init");

    trace::ReplayResult result;
    const bool ok = trace::ReplayJsonlFile("test/data/traces/sample_minimal.jsonl", clock,
                                           sm, result, &gamePersistence, &engine);

    Check(ok, result.ok ? "replay failed" : result.message);

    if (g_failures == 0) {
        std::puts("[TRACE_REPLAY_FILE] ok");
        RemoveSettingsFiles();
        return 0;
    }

    std::printf("[TRACE_REPLAY_FILE] failures=%d\n", g_failures);
    RemoveSettingsFiles();
    return 1;
}
