#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "runtime/desync_detector.hpp"

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
    std::puts("[DESYNC_DETECTOR] start");

    runtime::DesyncDetectorConfig cfg;
    cfg.stableRequiredMs = 100;

    runtime::DesyncDetector detector(cfg);
    fxe::ManualMillis::SetNowMs(0);
    detector.Reset();

    // No mismatch: never enters.
    {
        fxe::ManualMillis::SetNowMs(0);
        const auto st = detector.Evaluate(runtime::SensorStability::Stable, 0b1010, 0b1010);
        Check(!st.recoveryActive, "no mismatch => no recovery");
        Check(st.transition == runtime::DesyncTransition::None,
              "no mismatch => no transition");
        Check(!st.diff.IsMismatch(), "no mismatch => diff empty");
    }

    // Mismatch but unstable: do not enter.
    {
        fxe::ManualMillis::SetNowMs(10);
        const auto st = detector.Evaluate(runtime::SensorStability::Unstable, 0b1010, 0b1000);
        Check(!st.recoveryActive, "unstable mismatch => no recovery");
        Check(st.transition == runtime::DesyncTransition::None,
              "unstable mismatch => no transition");
        Check(st.diff.IsMismatch(), "unstable mismatch => diff present");
    }

    // Stable mismatch, but not long enough yet.
    {
        fxe::ManualMillis::SetNowMs(20);
        const auto st = detector.Evaluate(runtime::SensorStability::Stable, 0b1010, 0b1000);
        Check(!st.recoveryActive, "stable mismatch before delay => no recovery");
        Check(st.transition == runtime::DesyncTransition::None,
              "stable mismatch before delay => no transition");
        Check(st.diff.IsMismatch(), "stable mismatch => diff present");
    }

    // Stable mismatch long enough: enter recovery.
    {
        fxe::ManualMillis::SetNowMs(150);
        const auto st = detector.Evaluate(runtime::SensorStability::Stable, 0b1010, 0b1000);
        Check(st.recoveryActive, "stable mismatch after delay => recovery");
        Check(st.transition == runtime::DesyncTransition::EnteredRecovery,
              "enter recovery transition");
        Check(st.diff.IsMismatch(), "enter recovery => diff present");
        Check(st.diff.missingMask == 0b0010, "missing mask computed");
        Check(st.diff.extraMask == 0b0000, "extra mask computed");
    }

    // Stay in recovery while mismatch persists.
    {
        fxe::ManualMillis::SetNowMs(160);
        const auto st = detector.Evaluate(runtime::SensorStability::Stable, 0b1010, 0b1000);
        Check(st.recoveryActive, "mismatch persists => still in recovery");
        Check(st.transition == runtime::DesyncTransition::None,
              "no transition while persisting");
    }

    // Resolve mismatch while stable: exit recovery.
    {
        fxe::ManualMillis::SetNowMs(170);
        const auto st = detector.Evaluate(runtime::SensorStability::Stable, 0b1010, 0b1010);
        Check(!st.recoveryActive, "resolved => exit recovery");
        Check(st.transition == runtime::DesyncTransition::ExitedRecovery,
              "exit recovery transition");
        Check(!st.diff.IsMismatch(), "resolved => diff empty");
    }

    if (g_failures == 0) {
        std::puts("[DESYNC_DETECTOR] ok");
        return 0;
    }

    std::printf("[DESYNC_DETECTOR] failures=%d\n", g_failures);
    return 1;
}
