#include <cstdint>
#include <cstdio>

#include "firmware/sleep_wake_pin_policy.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

bool AllS3PinsRtcCapable(const firmware::SleepWakePinSet& pins) {
    for (uint8_t index = 0U; index < pins.count; ++index) {
        if (pins.pins[index] > 21U) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    std::puts("[SLEEP_WAKE_PIN_POLICY] start");

    const auto v1 = hardware::BoardHardwareProfileFor(
        hardware::BoardRevision::V1C3);
    const auto v2 = hardware::BoardHardwareProfileFor(
        hardware::BoardRevision::V2S3);
    const auto v3 = hardware::BoardHardwareProfileFor(
        hardware::BoardRevision::V3S3);

    const auto v1Wake = firmware::BuildDeepSleepWakePinSet(v1, true);
    Check(v1Wake.count == 4U && v1Wake.Contains(v1.p1.up) &&
              !v1Wake.Contains(v1.p1.x) && !v1Wake.Contains(v1.p1.o),
          "V1 keeps its legacy four-direction wake set without motion");

    const auto v2Wake = firmware::BuildDeepSleepWakePinSet(v2, true);
    Check(v2Wake.count == 8U && v2Wake.Contains(v2.p1.up) &&
              v2Wake.Contains(v2.p2.right) &&
              !v2Wake.Contains(v2.p1.x) && !v2Wake.Contains(v2.p1.o) &&
              !v2Wake.Contains(v2.p2.x) && !v2Wake.Contains(v2.p2.o),
          "V2 preserves its exact eight directional wake buttons");
    Check(AllS3PinsRtcCapable(v2Wake),
          "V2 deep-sleep wake set stays within ESP32-S3 RTC GPIO range");

    const auto v3Buttons = firmware::BuildDeepSleepWakePinSet(v3, false);
    Check(v3Buttons.count == 11U,
          "V3 enables every face control except unsafe P1-X");
    Check(!v3Buttons.Contains(v3.p1.x) && v3.p1.x == 0U,
          "V3 excludes active-low GPIO0 boot-strap button");
    Check(v3Buttons.Contains(v3.p1.o) && v3Buttons.Contains(v3.p2.x) &&
              v3Buttons.Contains(v3.p2.o),
          "V3 includes the three safe face buttons omitted on V2");
    Check(!v3Buttons.Contains(v3.accelerometerInterrupt),
          "V3 button-only plan omits disabled motion source");

    const auto v3Motion = firmware::BuildDeepSleepWakePinSet(v3, true);
    Check(v3Motion.count == 12U &&
              v3Motion.Contains(v3.accelerometerInterrupt) &&
              v3.accelerometerInterrupt == 21U,
          "V3 adds GPIO21 motion to the same active-low wake set");
    Check(AllS3PinsRtcCapable(v3Motion),
          "every V3 button and motion wake source is RTC-capable");
    Check(v3Motion.Mask() == (v3Motion.Mask() | (1ULL << 21U)),
          "V3 wake mask includes accelerometer bit 21");

    const auto allV3Buttons = firmware::BuildRuntimeButtonPinSet(v3);
    Check(allV3Buttons.count == 12U && allV3Buttons.Contains(v3.p1.x),
          "runtime input/leakage set still contains all twelve V3 controls");

    if (g_failures == 0) {
        std::puts("[SLEEP_WAKE_PIN_POLICY] ok");
        return 0;
    }
    std::printf("[SLEEP_WAKE_PIN_POLICY] failures=%d\n", g_failures);
    return 1;
}
