#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_clock.hpp"

namespace {

int g_failures = 0;

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

void CheckEqSide(const runtime::ClockSide a, const runtime::ClockSide b, const char* msg) {
    if (a == b) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[GAME_CLOCK] start");

    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);

    // Preset mapping sanity.
    {
        const runtime::TimerPreset off = runtime::TimerPresetForTimerSetting(0);
        CheckEqU32(off.baseSeconds, 0, "TIMER=0 base");
        CheckEqU32(off.incrementSeconds, 0, "TIMER=0 inc");

        const runtime::TimerPreset blitz = runtime::TimerPresetForTimerSetting(2);
        CheckEqU32(blitz.baseSeconds, 180, "TIMER=2 base");
        CheckEqU32(blitz.incrementSeconds, 2, "TIMER=2 inc");

        const runtime::TimerPreset invalid = runtime::TimerPresetForTimerSetting(99);
        CheckEqU32(invalid.baseSeconds, 0, "invalid TIMER maps to OFF");
    }

    runtime::ManualClock clock(0);
    runtime::GameClock gc;

    // Basic running consumption on active side.
    {
        gc.ResetFromTimerSetting(1, runtime::ClockSide::White); // 1 min
        gc.Start();

        clock.AdvanceMs(1000);
        gc.Tick();

        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::White), 59U * 1000U, "white consumed 1s");
        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::Black), 60U * 1000U, "black unchanged");
        Check(gc.GetActiveSide() == runtime::ClockSide::White, "active side remains white");

        gc.Stop();
    }

    // An authoritative event timestamp is charged even if the cooperative
    // coordinator observes it on a later pass.
    {
        clock.SetNowMs(5000U);
        gc.ResetFromTimerSetting(1, runtime::ClockSide::White);
        gc.StartAt(4975U);
        gc.Tick();
        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::White), 59975U,
                   "timestamped start includes coordinator latency");
        gc.Stop();
    }

    // Increment-on-commit + side switch.
    {
        gc.ResetFromTimerSetting(2, runtime::ClockSide::White); // 3+2
        gc.Start();

        clock.AdvanceMs(10U * 1000U);
        gc.Tick();

        gc.CommitMoveAndSwitch(runtime::ClockSide::Black);

        // White: 180s - 10s + 2s = 172s
        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::White), 172U * 1000U, "white gets increment on commit");
        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::Black), 180U * 1000U, "black still full");
        Check(gc.GetActiveSide() == runtime::ClockSide::Black, "active side switched to black");

        gc.Stop();
    }

    // Pause/resume stops consumption.
    {
        gc.ResetFromTimerSetting(1, runtime::ClockSide::White);
        gc.Start();

        clock.AdvanceMs(2000);
        gc.Tick();
        const uint32_t beforePause = gc.GetRemainingMs(runtime::ClockSide::White);

        gc.SetPaused(true);
        clock.AdvanceMs(5000);
        gc.Tick();
        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::White), beforePause, "paused does not consume");

        gc.SetPaused(false);
        clock.AdvanceMs(1000);
        gc.Tick();
        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::White), beforePause - 1000U, "resume consumes again");

        gc.Stop();
    }

    // Exact zero latches the active side and delivers one expiry transition.
    {
        clock.SetNowMs(0);
        gc.ResetFromTimerSetting(1, runtime::ClockSide::White);
        gc.Start();
        clock.AdvanceMs(60U * 1000U);
        gc.Tick();

        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::White), 0, "exact zero clamps white");
        Check(gc.HasTimedOut(), "exact zero latches timeout");
        Check(!gc.IsRunning() && !gc.IsPaused(), "timeout stops accounting");
        CheckEqSide(gc.GetTimedOutSide(), runtime::ClockSide::White, "timeout records losing side");
        Check(gc.ConsumeTimeoutEvent(), "first expiry consume succeeds");
        Check(!gc.ConsumeTimeoutEvent(), "second expiry consume is suppressed");

        gc.Tick();
        (void)gc.CaptureState();
        Check(!gc.ConsumeTimeoutEvent(), "tick and capture do not re-emit expiry");
    }

    // A timeout observed while pausing is terminal before pause state can change it.
    {
        clock.SetNowMs(0);
        gc.ResetFromTimerSetting(1, runtime::ClockSide::White);
        gc.Start();
        clock.AdvanceMs(60U * 1000U);
        gc.SetPaused(true);
        Check(gc.HasTimedOut() && !gc.IsRunning() && !gc.IsPaused(), "pause-time expiry stays terminal");
    }

    // Ordinary undo side switching still avoids Fischer increment.
    {
        clock.SetNowMs(0);
        gc.ResetFromTimerSetting(2, runtime::ClockSide::White); // 3+2
        gc.Start();
        clock.AdvanceMs(1000U);
        gc.SwitchActiveSideWithoutIncrement(runtime::ClockSide::Black);
        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::White), 179U * 1000U, "undo switch charges elapsed time");
        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::Black), 180U * 1000U, "undo switch applies no increment");
        CheckEqSide(gc.GetActiveSide(), runtime::ClockSide::Black, "undo switch remains ordinary before timeout");
        gc.Stop();
    }

    // Overshoot and terminal operations cannot revive a flagged side.
    {
        clock.SetNowMs(0);
        gc.ResetFromTimerSetting(2, runtime::ClockSide::Black); // 3+2
        gc.Start();
        clock.AdvanceMs(181U * 1000U);
        gc.Tick();

        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::Black), 0, "overshoot clamps black");
        CheckEqSide(gc.GetTimedOutSide(), runtime::ClockSide::Black, "overshoot records black");
        Check(gc.ConsumeTimeoutEvent(), "overshoot emits expiry");

        gc.CommitMoveAndSwitch(runtime::ClockSide::White);
        gc.SwitchActiveSideWithoutIncrement(runtime::ClockSide::White);
        gc.Start();
        gc.SetPaused(true);
        gc.SetPaused(false);
        gc.Stop();
        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::Black), 0, "terminal operations cannot add increment");
        CheckEqSide(gc.GetActiveSide(), runtime::ClockSide::Black, "terminal operations cannot switch side");
        Check(!gc.IsRunning() && gc.HasTimedOut(), "terminal operations keep timeout latched");

        gc.ResetFromTimerSetting(1, runtime::ClockSide::White);
        Check(!gc.HasTimedOut(), "reset clears timeout latch");
        Check(!gc.ConsumeTimeoutEvent(), "reset clears expiry event");
        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::White), 60U * 1000U, "reset restores preset time");
    }

    // The durable latch round-trips, while its delivery event is intentionally ephemeral.
    {
        clock.SetNowMs(0);
        gc.ResetFromTimerSetting(1, runtime::ClockSide::White);
        gc.Start();
        clock.AdvanceMs(60U * 1000U);
        gc.Tick();
        const runtime::GameClockState timedOut = gc.CaptureState();

        runtime::GameClock restored;
        restored.RestoreState(timedOut);
        Check(restored.HasTimedOut(), "restored timeout remains latched");
        Check(!restored.IsRunning() && !restored.IsPaused(), "restored timeout stays stopped");
        CheckEqSide(restored.GetTimedOutSide(), runtime::ClockSide::White, "restored timeout keeps loser");
        Check(!restored.ConsumeTimeoutEvent(), "restore emits no phantom expiry");
        restored.Start();
        restored.CommitMoveAndSwitch(runtime::ClockSide::Black);
        restored.RestoreState(runtime::GameClockState{
            .enabled = true,
            .running = true,
            .activeSide = runtime::ClockSide::Black,
            .remainingMsWhite = 1000,
            .remainingMsBlack = 1000,
        });
        CheckEqU32(restored.GetRemainingMs(runtime::ClockSide::White), 0, "restored timeout cannot revive");
        Check(!restored.IsRunning() && restored.HasTimedOut(), "restore cannot revive latched timeout");
    }

    // Corrupt or contradictory state fails closed or normalizes to terminal time.
    {
        runtime::GameClock corrupt;
        corrupt.RestoreState(runtime::GameClockState{
            .enabled = true,
            .running = true,
            .activeSide = static_cast<runtime::ClockSide>(99),
            .remainingMsWhite = 1000,
            .remainingMsBlack = 1000,
        });
        Check(!corrupt.IsEnabled() && !corrupt.IsRunning(), "invalid side restore fails closed");

        runtime::GameClock contradictory;
        contradictory.RestoreState(runtime::GameClockState{
            .enabled = true,
            .running = true,
            .paused = true,
            .timedOut = true,
            .activeSide = runtime::ClockSide::White,
            .timedOutSide = runtime::ClockSide::Black,
            .remainingMsWhite = 1000,
            .remainingMsBlack = 1000,
        });
        Check(contradictory.HasTimedOut() && !contradictory.IsRunning(), "timed-out restore normalizes stopped");
        CheckEqSide(contradictory.GetActiveSide(), runtime::ClockSide::Black, "timed-out restore normalizes active loser");
        CheckEqU32(contradictory.GetRemainingMs(runtime::ClockSide::Black), 0, "timed-out restore clamps loser");
        Check(!contradictory.ConsumeTimeoutEvent(), "normalized restore emits no expiry");
    }

    // Unsigned elapsed subtraction remains correct across the uint32 millisecond wrap.
    {
        clock.SetNowMs(UINT32_MAX - 500U);
        gc.ResetFromTimerSetting(1, runtime::ClockSide::White);
        gc.Start();
        clock.AdvanceMs(1000U);
        gc.Tick();
        CheckEqU32(gc.GetRemainingMs(runtime::ClockSide::White), 59U * 1000U, "elapsed accounting wraps safely");
    }

    // Non-terminal increment remains saturating.
    {
        runtime::GameClock saturation;
        saturation.RestoreState(runtime::GameClockState{
            .enabled = true,
            .running = true,
            .activeSide = runtime::ClockSide::White,
            .remainingMsWhite = UINT32_MAX - 1U,
            .remainingMsBlack = 1000U,
            .incrementMs = 10U,
        });
        saturation.CommitMoveAndSwitch(runtime::ClockSide::Black);
        CheckEqU32(saturation.GetRemainingMs(runtime::ClockSide::White), UINT32_MAX, "increment saturates");
        CheckEqSide(saturation.GetActiveSide(), runtime::ClockSide::Black, "saturating increment still switches side");
    }

    // OFF stays inert.
    {
        gc.ResetFromTimerSetting(0, runtime::ClockSide::Black);
        gc.Start();
        clock.AdvanceMs(1000U);
        gc.Tick();
        Check(!gc.IsEnabled() && !gc.IsRunning() && !gc.HasTimedOut(), "OFF remains disabled");
    }

    if (g_failures == 0) {
        std::puts("[GAME_CLOCK] ok");
        return 0;
    }

    std::printf("[GAME_CLOCK] failures=%d\n", g_failures);
    return 1;
}
