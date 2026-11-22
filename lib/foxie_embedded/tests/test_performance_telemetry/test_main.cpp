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

} // namespace

int main() {
    std::puts("[PERFORMANCE_TELEMETRY] start");

    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);

    fxe::PerformanceTelemetry t;

    // First loop establishes baseline.
    fxe::ManualMillis::SetNowMs(100);
    t.OnLoop();
    {
        const auto s = t.GetSnapshot();
        Check(s.loopCount == 1, "loopCount increments");
        Check(s.lastLoopDeltaMs == 0, "no delta on first loop");
        Check(s.minLoopDeltaMs == 0, "no min delta on first loop");
        Check(s.maxLoopDeltaMs == 0, "no max delta on first loop");
    }

    // Second loop produces a delta.
    fxe::ManualMillis::SetNowMs(120);
    t.OnLoop();
    {
        const auto s = t.GetSnapshot();
        Check(s.loopCount == 2, "loopCount increments again");
        Check(s.lastLoopDeltaMs == 20, "delta computed");
        Check(s.minLoopDeltaMs == 20, "min delta set");
        Check(s.maxLoopDeltaMs == 20, "max delta set");
    }

    // Third loop updates min/max.
    fxe::ManualMillis::SetNowMs(125);
    t.OnLoop();
    fxe::ManualMillis::SetNowMs(200);
    t.OnLoop();
    {
        const auto s = t.GetSnapshot();
        Check(s.loopCount == 4, "loopCount=4");
        Check(s.minLoopDeltaMs == 5, "min delta updates");
        Check(s.maxLoopDeltaMs == 75, "max delta updates");
    }

    // Counter hooks.
    t.OnFrameDropped();
    t.OnFrameDropped();
    t.OnI2cError();
    {
        const auto s = t.GetSnapshot();
        Check(s.droppedFrames == 2, "dropped frame count");
        Check(s.i2cErrors == 1, "i2c error count");
    }

    // The first sample straddles reset and must not contaminate a new window.
    t.ResetServiceGaps();
    t.OnInputCompletionGap(19000U); t.OnBleCompletionGap(23000U);
    t.OnInputCompletionGap(17U); t.OnBleCompletionGap(23U);
    Check(t.GetSnapshot().inputCompletionGapMs == 17U &&
          t.GetSnapshot().bleCompletionGapMs == 23U,
          "service window excludes old intervals and records actual new completion gaps");
    t.OnInputCompletionGap(11U); t.OnBleCompletionGap(9U);
    Check(t.GetSnapshot().inputCompletionGapMs == 17U &&
          t.GetSnapshot().bleCompletionGapMs == 23U,
          "independent service maxima cannot decrease");
    t.ResetServiceGaps();
    Check(t.GetSnapshot().inputCompletionGapMs == UINT32_MAX &&
          t.GetSnapshot().bleCompletionGapMs == UINT32_MAX,
          "reset reports unavailable until an interval completes");

    if (g_failures == 0) {
        std::puts("[PERFORMANCE_TELEMETRY] ok");
        return 0;
    }

    std::printf("[PERFORMANCE_TELEMETRY] failures=%d\n", g_failures);
    return 1;
}
