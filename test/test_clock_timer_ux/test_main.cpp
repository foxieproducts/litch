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

runtime::ManualClock* g_clockForMillis = nullptr;

uint32_t TestMillisProvider() {
    return g_clockForMillis ? g_clockForMillis->NowMs() : 0U;
}

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckEqU32(const uint32_t a, const uint32_t b, const char* msg) {
    if (a == b) {
        return;
    }

    std::printf("[FAIL] %s (a=%lu b=%lu)\n", msg, static_cast<unsigned long>(a), static_cast<unsigned long>(b));
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[CLOCK_TIMER_UX] start");

    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_clock_timer_ux.json";
    cfg.deferredSaveDelayMs = 1000;

    (void)std::remove(cfg.path);
    (void)std::remove("build/test_clock_timer_ux.json.bak");
    (void)std::remove("build/test_clock_timer_ux.json.tmp");

    runtime::ManualClock clock(1000);
    g_clockForMillis = &clock;
    fxe::ScopedMillisProviderOverride millisOverride(&TestMillisProvider);

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::SettingsStore settings(cfg);
    Check(settings.Load(), "settings load");

    // Enable a 1-minute clock.
    settings.Doc()["TIMER"] = 1;

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::GamePersistenceCoordinator coord(settings, engine, sm);
    Check(coord.LoadOrInitialize(), "LoadOrInitialize");

    // Use a tiny legal FEN to keep expected/physical synchronization simple
    // without starting in an immediate insufficient-material draw.
    // (0=a8 mapping => e2=52, e1=60, h1=63)
    Check(coord.LoadExternalFen("8/8/8/8/8/8/4P3/4K2k w - - 0 1"), "LoadExternalFen");

    // Resolving initial placement is not play. A short stabilization window
    // filters a just-dropped piece that briefly disappears and returns.
    const uint64_t occ = (1ULL << 52) | (1ULL << 60) | (1ULL << 63);
    sm.DebugSetPhysicalOccupancyMask(occ);
    sm.Tick();
    Check(!sm.IsPlacementModeActive(), "placement mode exits when synced");
    Check(sm.IsExpectedOccupancyMatched(), "expected occupancy matched");

    coord.Update();
    const auto& gc = coord.GetGameClock();
    Check(gc.IsEnabled(), "clock enabled");
    Check(!gc.IsRunning(), "clock waits at playable initial position");
    Check(!gc.IsPaused(), "clock not paused once playable");

    const uint32_t whiteAtStart =
        gc.GetRemainingMs(runtime::ClockSide::White);
    const uint32_t blackAtStart =
        gc.GetRemainingMs(runtime::ClockSide::Black);
    clock.AdvanceMs(300U);
    sm.HandleSensorChange(52U, false);
    coord.Update();
    Check(!gc.IsRunning(), "brief pickup during setup grace does not start clock");
    sm.HandleSensorChange(52U, true);
    coord.Update();
    Check(!gc.IsRunning(), "returned setup piece keeps clock stopped");

    clock.AdvanceMs(749U);
    coord.Update();
    sm.HandleSensorChange(52U, false);
    coord.Update();
    Check(!gc.IsRunning(), "restored setup receives a fresh stabilization window");
    clock.AdvanceMs(1U);
    coord.Update();
    Check(gc.IsRunning(), "held first pickup starts at the end of setup grace");
    Check(gc.GetActiveSide() == runtime::ClockSide::White,
          "first pickup starts the mover clock");

    clock.AdvanceMs(100U);
    sm.HandleSensorChange(52U, true);
    coord.Update();
    Check(gc.IsRunning() && !gc.IsPaused(),
          "returning the first piece after start does not stop thinking time");
    sm.HandleSensorChange(52U, false);
    coord.Update();
    sm.HandleSensorChange(44U, true);
    coord.Update();
    Check(gc.IsRunning(), "clock continues during first-move settle");
    clock.AdvanceMs(11U);
    sm.Tick();
    coord.Update();
    Check(gc.IsRunning(), "clock remains running after first move settles");
    Check(gc.GetActiveSide() == runtime::ClockSide::Black,
          "black becomes active after white first move");
    Check(gc.GetRemainingMs(runtime::ClockSide::White) <= whiteAtStart,
          "white first move consumes time beginning at pickup");
    CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::Black), blackAtStart,
               "black starts with full base time");

    // Running clock consumes time.
    const uint32_t before = gc.GetRemainingMs(runtime::ClockSide::Black);
    clock.AdvanceMs(1000);
    coord.Update();
    const uint32_t after = gc.GetRemainingMs(runtime::ClockSide::Black);
    Check(after + 1000U == before, "clock consumes 1s for active side");

    // Mismatch pauses time.
    sm.DebugSetPhysicalOccupancyMask(0);
    sm.Tick();
    coord.Update();
    Check(gc.IsPaused(), "clock pauses while mismatched");

    const uint32_t pausedBefore = gc.GetRemainingMs(runtime::ClockSide::Black);
    clock.AdvanceMs(5000);
    coord.Update();
    const uint32_t pausedAfter = gc.GetRemainingMs(runtime::ClockSide::Black);
    CheckEqU32(pausedBefore, pausedAfter, "paused clock does not consume");

    // Target-first capture is reducer-authorized opening progress. An
    // unrelated opponent removal is correction and must never start time.
    {
        modules::SettingsStoreConfig captureCfg;
        captureCfg.path = "build/test_clock_timer_ux_capture.json";
        captureCfg.deferredSaveDelayMs = 1000U;
        (void)std::remove(captureCfg.path);
        (void)std::remove("build/test_clock_timer_ux_capture.json.bak");
        (void)std::remove("build/test_clock_timer_ux_capture.json.tmp");

        runtime::ManualClock captureClock(10000U);
        g_clockForMillis = &captureClock;
        runtime::GameStateMachine captureSm(smCfg);
        captureSm.Reset();
        modules::SettingsStore captureSettings(captureCfg);
        Check(captureSettings.Load(), "capture settings load");
        captureSettings.Doc()["TIMER"] = 1U;
        test_support::BoundedHintEngine captureEngine;
        captureEngine.Reset();
        modules::GamePersistenceCoordinator captureCoord(
            captureSettings, captureEngine, captureSm);
        Check(captureCoord.LoadOrInitialize(), "capture coordinator load");
        Check(captureCoord.LoadExternalFen(
                  "8/8/8/8/8/3p4/4P3/4K2k w - - 0 1"),
              "target-first capture position loads");

        const uint64_t captureOcc =
            (1ULL << 43U) | (1ULL << 52U) |
            (1ULL << 60U) | (1ULL << 63U);
        captureSm.DebugSetPhysicalOccupancyMask(captureOcc);
        captureSm.Tick();
        captureCoord.Update();
        captureClock.AdvanceMs(751U);

        captureSm.HandleSensorChange(63U, false);
        captureCoord.Update();
        Check(!captureCoord.GetGameClock().IsRunning(),
              "unexplained opponent removal does not start the clock");
        captureSm.HandleSensorChange(63U, true);
        captureCoord.Update();
        captureClock.AdvanceMs(751U);

        captureSm.HandleSensorChange(43U, false);
        Check(captureSm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::CaptureStaged,
              "legal target-first removal enters CaptureStaged");
        captureClock.AdvanceMs(25U);
        captureCoord.Update();
        Check(captureCoord.GetGameClock().IsRunning() &&
                  !captureCoord.GetGameClock().IsPaused(),
              "target-first capture starts the mover clock");
        CheckEqU32(captureCoord.GetGameClock().GetRemainingMs(
                       runtime::ClockSide::White),
                   59975U,
                   "target-first start charges from the sensor observation");
    }

    if (g_failures == 0) {
        std::puts("[CLOCK_TIMER_UX] ok");
        return 0;
    }

    std::printf("[CLOCK_TIMER_UX] failures=%d\n", g_failures);
    return 1;
}
