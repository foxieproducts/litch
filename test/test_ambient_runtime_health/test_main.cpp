#include <cstdio>

#include "firmware/ambient_runtime_health.hpp"

namespace {

int failures = 0;

void Check(const bool value, const char* message) {
    if (!value) {
        std::printf("[FAIL] %s\n", message);
        ++failures;
    }
}

hardware::AmbientLightUpdateResult Completed(const uint8_t count) {
    return hardware::AmbientLightUpdateResult{
        true,
        count != 0U,
        count,
    };
}

} // namespace

int main() {
    std::puts("[AMBIENT_RUNTIME_HEALTH] start");

    runtime::RuntimeHealthReducer reducer;
    const hardware::AmbientLightUpdateResult notDue{};
    Check(!firmware::ObserveAmbientRuntimeHealth(reducer, true, 0U, notDue),
          "startup or not-due update does not publish a zero-sensor fact");
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy &&
              reducer.Snapshot().revision == 0U,
          "not-due leaves runtime health unknown rather than degraded");

    Check(!firmware::ObserveAmbientRuntimeHealth(reducer, false, 1U,
                                                 Completed(0U)),
          "V1/C3 virtual profile does not publish an ambient fact");
    Check(reducer.Snapshot().revision == 0U,
          "V1/C3 ambient absence remains non-degrading");

    Check(!firmware::ObserveAmbientRuntimeHealth(reducer, true, 2U,
                                                 Completed(2U)),
          "two fitted sensors retain healthy state");
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::Healthy,
          "two fitted sensors are healthy");

    Check(firmware::ObserveAmbientRuntimeHealth(reducer, true, 3U,
                                                Completed(1U)),
          "one fitted sensor is published as a completed attempt");
    Check(reducer.Snapshot().state == runtime::RuntimeHealthState::DegradedUsable &&
              runtime::HasRuntimeHealthCause(
                  reducer.Snapshot().causes,
                  runtime::RuntimeHealthCause::OneAmbientSensor),
          "one sensor remains usable but degraded");

    Check(firmware::ObserveAmbientRuntimeHealth(reducer, true, 4U,
                                                Completed(0U)),
          "zero fitted sensors is published only after a completed attempt");
    Check(runtime::HasRuntimeHealthCause(
              reducer.Snapshot().causes,
              runtime::RuntimeHealthCause::NoAmbientSensors),
          "zero fitted sensors select the deterministic fallback cause");

    Check(!firmware::ShouldUseAmbientBrightness(true, false),
          "startup without a validated sample bypasses ambient interpolation");
    Check(!firmware::ShouldUseAmbientBrightness(true, Completed(0U).sampleCaptured),
          "validated zero sensors bypass ambient interpolation");
    Check(firmware::ShouldUseAmbientBrightness(true, Completed(1U).sampleCaptured),
          "one valid sensor keeps its ambient sample usable");
    Check(!firmware::ShouldUseAmbientBrightness(false, true),
          "V1/C3 does not use an ambient path without fitted hardware");

    std::printf("[AMBIENT_RUNTIME_HEALTH] %s\n",
                failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
