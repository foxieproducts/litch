#include <cstdio>

#include "firmware/p1x_deep_wake.hpp"
#include "firmware/sleep_controller.hpp"
#include "firmware/sleep_wake_pin_policy.hpp"
#include "modules/settings_store.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

} // namespace

int main() {
    std::puts("[P1X_DEEP_WAKE_POLICY] start");

    modules::SettingsStoreConfig storeConfig;
    storeConfig.path = "build/test_p1x_deep_wake_policy.json";
    modules::SettingsStore settings(storeConfig);
    settings.Doc()["P1_X_DEEP_WAKE_ENABLED"] = false;
    settings.Doc()["P1_X_WAKE_HOLD_MS"] = 5000U;
    firmware::SleepController oldPreference;
    oldPreference.ConfigureHardwareProfile(hardware::BoardHardwareProfileFor(
        hardware::BoardRevision::V3_3S3));
    Check(oldPreference.IsP1XDeepWakeEnabled(),
          "a stored old false cannot disable standard supported-S3 P1-X wake");

    uint8_t stableReleaseSamples = 0U;
    for (uint32_t heldMs = 0U; heldMs < 30000U; ++heldMs) {
        stableReleaseSamples = firmware::p1x_deep_wake::AdvanceStableRelease(
            stableReleaseSamples, false);
    }
    Check(!firmware::p1x_deep_wake::IsReleaseStable(stableReleaseSamples),
          "even a thirty-second P1-X hold cannot permit ROM handoff");
    for (uint8_t releasedSample = 0U;
         releasedSample < firmware::p1x_deep_wake::RELEASE_STABLE_SAMPLES;
         ++releasedSample) {
        stableReleaseSamples = firmware::p1x_deep_wake::AdvanceStableRelease(
            stableReleaseSamples, true);
    }
    Check(firmware::p1x_deep_wake::IsReleaseStable(stableReleaseSamples),
          "stable P1-X release permits application boot");
    stableReleaseSamples = firmware::p1x_deep_wake::AdvanceStableRelease(
        firmware::p1x_deep_wake::RELEASE_STABLE_SAMPLES - 1U, false);
    Check(stableReleaseSamples == 0U &&
              !firmware::p1x_deep_wake::IsReleaseStable(stableReleaseSamples),
          "release bounce resets the complete stability interval");
    const auto v3 = hardware::BoardHardwareProfileFor(
        hardware::BoardRevision::V3S3);
    const auto v2 = hardware::BoardHardwareProfileFor(
        hardware::BoardRevision::V2S3);
    const auto unknown = hardware::BoardHardwareProfileFor(
        hardware::BoardRevision::Unknown);
    const auto ext1WakeSet =
        firmware::BuildDeepSleepWakePinSet(v3, true, false);
    const auto runtimeRestoreSet =
        firmware::BuildDeepSleepWakePinSet(v3, true, true);
    Check(!ext1WakeSet.Contains(v3.p1.x) && ext1WakeSet.count == 12U,
          "EXT1 wake set excludes boot-strap GPIO0");
    Check(runtimeRestoreSet.Contains(v3.p1.x) &&
              runtimeRestoreSet.count == 13U,
          "runtime restore set covers GPIO0 plus safe buttons and motion");
    Check(firmware::BuildDeepSleepWakePinSet(v2, false, true)
              .Contains(v2.p1.x),
          "S3 V2 runtime restoration includes ULP-owned GPIO0");
    Check(unknown.revision == hardware::BoardRevision::V2S3 &&
              firmware::BuildDeepSleepWakePinSet(unknown, false, true)
                  .Contains(unknown.p1.x),
          "unknown S3 revision retains conservative V2 GPIO0 restoration");

    firmware::SleepController sleep;
    sleep.ConfigureHardwareProfile(v2);
    Check(sleep.IsP1XDeepWakeEnabled() &&
              (sleep.GetConfiguredButtonWakeMask() & 1ULL) == 0U,
          "V2 runtime enables P1-X without putting GPIO0 in the EXT1 mask");
    firmware::SleepController c3Sleep;
    const auto c3 = hardware::BoardHardwareProfileFor(
        hardware::BoardRevision::V1C3);
    c3Sleep.ConfigureHardwareProfile(c3);
    Check(!c3Sleep.IsP1XDeepWakeEnabled() &&
              (c3Sleep.GetConfiguredButtonWakeMask() &
               (1ULL << c3.p1.x)) == 0U,
          "C3 ignores persistent GPIO0-wake intent and keeps its legacy mask");

    if (g_failures == 0) {
        std::puts("[P1X_DEEP_WAKE_POLICY] ok");
        return 0;
    }
    std::printf("[P1X_DEEP_WAKE_POLICY] failures=%d\n", g_failures);
    return 1;
}
