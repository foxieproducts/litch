#include "bounded_hint_engine.hpp"
#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "trace/trace_replay.hpp"

namespace {

std::vector<std::string> ListJsonlFiles(const char* dirPath) {
    std::vector<std::string> files;

    DIR* dir = opendir(dirPath);
    if (!dir) {
        return files;
    }

    while (dirent* ent = readdir(dir)) {
        const std::string name = ent->d_name;
        if (name.size() < 6) {
            continue;
        }

        const std::string suffix = ".jsonl";
        if (name.size() < suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }

        files.push_back(std::string(dirPath) + "/" + name);
    }

    closedir(dir);
    std::sort(files.begin(), files.end());
    return files;
}

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
    std::puts("[TRACE_REPLAY_CORPUS] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);

    const auto files = ListJsonlFiles("test/data/traces");
    Check(files.size() >= 1, "found at least one trace file");

    for (const auto& path : files) {
        std::printf("[TRACE] %s\n", path.c_str());

        runtime::GameStateMachineConfig cfg;
        cfg.settleDurationMs = 100;
        cfg.cpuIdleDelayMs = 999999;

        modules::SettingsStoreConfig settingsCfg;
        settingsCfg.path = "build/test_trace_replay_corpus.json";
        settingsCfg.deferredSaveDelayMs = 100000;
        (void)std::remove(settingsCfg.path);
        (void)std::remove("build/test_trace_replay_corpus.json.bak");
        (void)std::remove("build/test_trace_replay_corpus.json.tmp");

        modules::SettingsStore settings(settingsCfg);
        Check(settings.Load(), "settings load");

        runtime::ManualClock clock(0);
        runtime::GameStateMachine sm(cfg);
        sm.Reset();

        test_support::BoundedHintEngine engine;
        modules::GamePersistenceCoordinator gamePersistence(settings, engine, sm);
        Check(gamePersistence.LoadOrInitialize(), "persistence load/init");

        trace::ReplayResult result;
        const bool ok = trace::ReplayJsonlFile(path.c_str(), clock, sm, result,
                                               &gamePersistence, &engine);
        if (!ok) {
            std::printf("[TRACE] replay failed line=%u: %s\n",
                        static_cast<unsigned>(result.failureLine),
                        result.message);
        }
        Check(ok, "ReplayJsonlFile returned true");
    }

    if (g_failures == 0) {
        std::puts("[PASS] all traces replayed");
    }

    return g_failures == 0 ? 0 : 1;
}
