#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <cstdlib>

#include "foxie_embedded/test_support/manual_millis_guard.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"

#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/runtime_health.hpp"

namespace {

struct Harness {
    runtime::ManualClock clock;
    fxe::test_support::ManualMillisGuard millisGuard;
    fxe::test_support::TestResults& results;
    runtime::GameStateMachine sm;
    modules::SettingsStore settings;
    test_support::BoundedHintEngine engine;
    modules::GamePersistenceCoordinator coord;

    explicit Harness(const char* settingsPath,
                     const uint32_t startMs,
                     const uint32_t timerSetting,
                     const runtime::GameStateMachineConfig& smCfg,
                     fxe::test_support::TestResults& resultsRef)
        : clock(),
          millisGuard(),
          results(resultsRef),
          sm(smCfg),
          settings([&] {
              modules::SettingsStoreConfig cfg;
              cfg.path = settingsPath;
              cfg.deferredSaveDelayMs = 1000;
              return cfg;
          }()),
          engine(),
          coord(settings, engine, sm) {
        clock.SetNowMs(startMs);
        sm.Reset();

        (void)std::remove(settingsPath);
        results.Check(settings.Load(), "settings load");

        settings.Doc()["TIMER"] = timerSetting;

        engine.Reset();
        results.Check(coord.LoadOrInitialize(), "LoadOrInitialize");
    }
};

uint64_t Mask3(const uint8_t a, const uint8_t b, const uint8_t c) {
    return (1ULL << a) | (1ULL << b) | (1ULL << c);
}

bool CommitFirstE2E3(Harness& h) {
    const uint64_t occ = Mask3(52U, 60U, 63U);
    h.sm.SetExpectedOccupancyMask(occ);
    h.sm.SetValidDestMask(UINT64_C(1) << 44U);
    h.clock.AdvanceMs(750U);
    h.coord.Update();
    h.sm.HandleSensorChange(52U, false);
    h.coord.Update();
    h.sm.HandleSensorChange(44U, true);
    h.clock.AdvanceMs(20U);
    h.sm.Tick();
    h.coord.Update();
    return h.coord.GetGameClock().IsRunning() &&
           h.coord.GetGameClock().GetActiveSide() ==
               runtime::ClockSide::Black;
}

} // namespace

int main() {
    fxe::test_support::TestResults results("CLOCK_CORE");
    results.Start();

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    // Clock runs during CPU thinking (it is still a turn).
    {
        Harness h("build/test_clock_core_thinking.json", 1000, 1, smCfg, results);
        results.Check(h.coord.LoadExternalFen("8/8/8/8/8/8/4P3/4K2k w - - 0 1"), "LoadExternalFen");

        const uint64_t occ = (1ULL << 52) | (1ULL << 60) | (1ULL << 63);
        h.sm.DebugSetPhysicalOccupancyMask(occ);
        h.sm.Tick();
        h.coord.Update();

        results.Check(!h.coord.GetGameClock().IsRunning(),
                      "clock waits for the first legal pickup");
        results.Check(CommitFirstE2E3(h),
                      "first legal pickup starts time before the move settles");

        h.sm.SetCpuThinkingActive(true);
        const uint32_t before = h.coord.GetGameClock().GetRemainingMs(runtime::ClockSide::Black);
        h.clock.AdvanceMs(2000);
        h.coord.Update();
        const uint32_t after = h.coord.GetGameClock().GetRemainingMs(runtime::ClockSide::Black);

        results.Check(after + 2000U == before, "CPU thinking does not pause clock");
        h.sm.SetCpuThinkingActive(false);
    }

    // Undo/cancel-undo and other correction flows should not apply increment; clock just
    // synchronizes active side and pauses while mismatched.
    {
        Harness h("build/test_clock_core_commit.json", 1000, 2, smCfg, results); // 3+2

        // Position: kings + a single pawn so we can commit a real engine move.
        // Squares (0=a8): e2=52, e3=44, e1=60, h1=63.
        results.Check(h.coord.LoadExternalFen("8/8/8/8/8/8/4P3/4K2k w - - 0 1"), "LoadExternalFen");

        const uint64_t occ = Mask3(52, 60, 63);
        h.sm.DebugSetPhysicalOccupancyMask(occ);
        h.sm.Tick();
        h.coord.Update();
        results.Check(!h.coord.GetGameClock().IsRunning(),
                      "clock stays stopped after initial placement resolves");

        // Spend 10s on white, then commit e2->e3.
        h.clock.AdvanceMs(10U * 1000U);
        h.coord.Update();

        const uint32_t beforeWhite = h.coord.GetGameClock().GetRemainingMs(runtime::ClockSide::White);

        // Drive a simple move finalize through the state machine.
        h.sm.SetExpectedOccupancyMask(occ);
        h.sm.SetValidDestMask(1ULL << 44);
        h.sm.HandleSensorChange(52, false);
        h.sm.HandleSensorChange(44, true);

        h.clock.AdvanceMs(20);
        h.sm.Tick();
        h.coord.Update();

        // White should have received +2s increment on commit, and side-to-move is black.
        results.Check(h.coord.GetGameClock().GetActiveSide() == runtime::ClockSide::Black,
                      "active side switches on commit");

        const uint32_t afterWhite = h.coord.GetGameClock().GetRemainingMs(runtime::ClockSide::White);
        results.Check(afterWhite >= beforeWhite, "white time not reduced by commit");
        results.Check(afterWhite >= beforeWhite + 1800U, "white received increment (lower bound)");
        results.Check(afterWhite <= beforeWhite + 2000U, "white received increment (upper bound)");

        // Now force a mismatch and ensure pause.
        h.sm.DebugSetPhysicalOccupancyMask(0);
        h.sm.Tick();
        h.coord.Update();
        results.Check(h.coord.GetGameClock().IsPaused(), "mismatch pauses clock");

        const uint32_t pausedBefore = h.coord.GetGameClock().GetRemainingMs(runtime::ClockSide::Black);
        h.clock.AdvanceMs(5000);
        h.coord.Update();
        const uint32_t pausedAfter = h.coord.GetGameClock().GetRemainingMs(runtime::ClockSide::Black);
        results.CheckEqU32(pausedBefore, pausedAfter, "paused clock does not consume");
    }

    // Runtime health is an additive pause reason: it freezes elapsed
    // accounting immediately and ordinary coordinator policy owns resume.
    {
        Harness h("build/test_clock_core_runtime_health.json", 1000, 1,
                  smCfg, results);
        results.Check(
            h.coord.LoadExternalFen("8/8/8/8/8/8/4P3/4K2k w - - 0 1"),
            "runtime-health clock position loads");
        const uint64_t occ = Mask3(52, 60, 63);
        h.sm.DebugSetPhysicalOccupancyMask(occ);
        h.sm.Tick();
        h.coord.Update();
        results.Check(CommitFirstE2E3(h),
                      "runtime-health fixture starts after its first move");

        runtime::RuntimeHealthSnapshot paused;
        paused.state = runtime::RuntimeHealthState::InputPaused;
        paused.causes = runtime::RuntimeHealthCauseMask(
            runtime::RuntimeHealthCause::BoardScanUncertain);
        paused.advice = runtime::RuntimeHealthAdvice{false, false, false};
        h.coord.SetRuntimeHealthSnapshot(paused);
        const uint32_t before = h.coord.GetGameClock().GetRemainingMs(
            runtime::ClockSide::Black);
        h.clock.AdvanceMs(5000U);
        h.coord.Update();
        results.CheckEqU32(
            before,
            h.coord.GetGameClock().GetRemainingMs(runtime::ClockSide::Black),
            "runtime-health pause freezes elapsed accounting");

        runtime::RuntimeHealthSnapshot healthy;
        h.coord.SetRuntimeHealthSnapshot(healthy);
        h.coord.Update();
        results.Check(!h.coord.GetGameClock().IsPaused(),
                      "healthy advice resumes through ordinary pause policy");
    }

    // A clock reaching zero is a coordinator-owned terminal result. Exact-zero
    // and overshoot both award the game to the side opposite the active clock.
    for (const uint32_t elapsedMs : {60000U, 60001U}) {
        Harness h("build/test_clock_core_timeout.json", 1000, 1, smCfg, results);
        results.Check(h.coord.LoadExternalFen("8/8/8/8/8/8/4P3/4K2k w - - 0 1"),
                      "timeout position loads");

        const uint64_t occ = Mask3(52, 60, 63);
        h.sm.DebugSetPhysicalOccupancyMask(occ);
        h.sm.Tick();
        h.coord.Update();
        results.Check(CommitFirstE2E3(h),
                      "timeout clock starts after the first move");

        h.clock.AdvanceMs(elapsedMs);
        h.coord.Update();

        results.Check(h.coord.HasClockTerminalOutcome(),
                      "clock timeout latches a coordinator terminal");
        results.Check(h.coord.GetClockTerminalWinner() == modules::GameWinner::White,
                      "clock timeout awards the opposite side");
        results.Check(!h.coord.GetGameClock().IsRunning(),
                      "clock timeout stops the clock");

        h.coord.ResetEngine();
        results.Check(!h.coord.HasClockTerminalOutcome(),
                      "new game clears the clock terminal");
    }

    // A direct engine replacement is another full-game boundary. Timeout must
    // not prevent the existing external-FEN detector from adopting it.
    {
        Harness h("build/test_clock_core_timeout_external_fen.json", 1000, 1, smCfg, results);
        results.Check(h.coord.LoadExternalFen("8/8/8/8/8/8/4P3/4K2k w - - 0 1"),
                      "external replacement fixture loads");
        const uint64_t occ = Mask3(52, 60, 63);
        h.sm.DebugSetPhysicalOccupancyMask(occ);
        h.sm.Tick();
        h.coord.Update();
        results.Check(CommitFirstE2E3(h),
                      "external replacement fixture starts after a move");
        h.clock.AdvanceMs(60000U);
        h.coord.Update();
        results.Check(h.coord.HasClockTerminalOutcome(),
                      "external replacement fixture reaches timeout");

        results.Check(h.engine.SetFen("8/8/8/8/8/8/3P4/4K2k b - - 0 1"),
                      "external engine accepts replacement FEN");
        h.coord.Update();

        results.Check(!h.coord.HasClockTerminalOutcome(),
                      "external engine replacement clears timeout terminal");
        results.Check(!h.coord.GetGameClock().HasTimedOut(),
                      "external engine replacement resets timed-out clock");
        results.Check(h.coord.GetGameClock().GetActiveSide() ==
                          runtime::ClockSide::Black,
                      "external engine replacement adopts replacement side");
    }

    // A previously derived orthodox terminal result wins over clock processing.
    {
        Harness h("build/test_clock_core_terminal_precedence.json", 1000, 1, smCfg, results);
        results.Check(h.coord.LoadExternalFen("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1"),
                      "checkmate position loads");
        h.sm.DebugSetPhysicalOccupancyMask((1ULL << 7) | (1ULL << 14) | (1ULL << 22));
        h.sm.Tick();
        h.coord.Update();
        results.Check(!h.coord.HasClockTerminalOutcome(),
                      "orthodox terminal result takes precedence over clock timeout");
    }

    return results.Finish();
}
