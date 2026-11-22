#include <cstdint>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/performance_telemetry.hpp"

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
    std::puts("[PERF_GUARD] start");

    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);

    fxe::PerformanceTelemetry perf;
    perf.Reset();

    // First loop establishes baseline; no delta yet.
    fxe::ManualMillis::SetNowMs(100);
    perf.OnLoop();
    {
        const auto s = perf.GetSnapshot();
        CheckEqU32(s.loopCount, 1u, "loopCount increments");
        CheckEqU32(s.lastLoopDeltaMs, 0u, "first loop delta is 0");
        CheckEqU32(s.minLoopDeltaMs, 0u, "first loop min is 0");
        CheckEqU32(s.maxLoopDeltaMs, 0u, "first loop max is 0");
    }

    fxe::ManualMillis::SetNowMs(120);
    perf.OnLoop();
    fxe::ManualMillis::SetNowMs(170);
    perf.OnLoop();
    {
        const auto s = perf.GetSnapshot();
        CheckEqU32(s.loopCount, 3u, "loopCount after 3 loops");
        CheckEqU32(s.lastLoopDeltaMs, 50u, "lastLoopDeltaMs uses elapsed time");
        CheckEqU32(s.minLoopDeltaMs, 20u, "minLoopDeltaMs tracks minimum");
        CheckEqU32(s.maxLoopDeltaMs, 50u, "maxLoopDeltaMs tracks maximum");
    }

    perf.OnFrameDropped();
    perf.OnFrameDropped();
    perf.OnI2cError();
    perf.OnI2cErrors(3);
    {
        const auto s = perf.GetSnapshot();
        CheckEqU32(s.droppedFrames, 2u, "droppedFrames increments");
        CheckEqU32(s.i2cErrors, 4u, "i2cErrors increments and adds");
    }

    if (g_failures == 0) {
        std::puts("[PERF_GUARD] ok");
        return 0;
    }

    std::printf("[PERF_GUARD] failures=%d\n", g_failures);
    return 1;
}
