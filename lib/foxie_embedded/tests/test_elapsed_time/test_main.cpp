#include <cstdint>
#include <cstdio>

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/millis.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckEqU32(const uint32_t got, const uint32_t want, const char* msg) {
    if (got == want) {
        return;
    }

    std::printf("[FAIL] %s (got=%u want=%u)\n", msg, got, want);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[ELAPSED_TIME] start");

    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);

    fxe::ElapsedTime t;
    fxe::ManualMillis::SetNowMs(123);
    Check(!t.Started(), "default not started");
    CheckEqU32(t.ElapsedMs(), 0u, "ElapsedMs returns 0 when not started");
    CheckEqU32(t.RemainingMs(50), 50u, "RemainingMs returns full duration when not started");
    Check(!t.HasElapsed(1), "HasElapsed false when not started");

    fxe::ManualMillis::SetNowMs(100);
    t.Reset();
    Check(t.Started(), "Reset starts timer");
    CheckEqU32(t.StartMs(), 100u, "StartMs is set");
    CheckEqU32(t.ElapsedMs(), 0u, "Elapsed 0 at start");
    fxe::ManualMillis::SetNowMs(149);
    CheckEqU32(t.ElapsedMs(), 49u, "Elapsed advances");
    Check(!t.HasElapsed(50), "HasElapsed false before boundary");
    fxe::ManualMillis::SetNowMs(150);
    Check(t.HasElapsed(50), "HasElapsed true at boundary");

    fxe::ManualMillis::SetNowMs(120);
    CheckEqU32(t.RemainingMs(50), 30u, "RemainingMs counts down");
    fxe::ManualMillis::SetNowMs(150);
    CheckEqU32(t.RemainingMs(50), 0u, "RemainingMs clamps to 0 after elapsed");

    // Convenience: StartNow() should start immediately.
    fxe::ManualMillis::SetNowMs(200);
    fxe::ElapsedTime started = fxe::ElapsedTime::StartNow();
    Check(started.Started(), "StartNow starts timer");
    CheckEqU32(started.StartMs(), 200u, "StartNow sets StartMs");
    fxe::ManualMillis::SetNowMs(225);
    CheckEqU32(started.ElapsedMs(), 25u, "StartNow elapsed advances");

    // Wrap-around safety: unsigned subtraction should remain correct.
    // start=0xFFFFFFF0, now=0x00000010 => elapsed = 0x20 (32).
    fxe::ElapsedTime wrap(0xFFFFFFF0u);
    fxe::ManualMillis::SetNowMs(0x00000010u);
    CheckEqU32(wrap.ElapsedMs(), 32u, "wrap-around elapsed");

    if (g_failures == 0) {
        std::puts("[ELAPSED_TIME] ok");
        return 0;
    }

    std::printf("[ELAPSED_TIME] failures=%d\n", g_failures);
    return 1;
}
