#pragma once

#include <cstdint>

namespace firmware::p1x_deep_wake {
inline constexpr uint32_t POLL_INTERVAL_US = 40000U;
inline constexpr uint8_t RELEASE_STABLE_SAMPLES = 5U;
inline constexpr uint8_t WAKE_STATE = 1U + RELEASE_STABLE_SAMPLES;

constexpr uint8_t AdvanceStableRelease(
    const uint8_t releasedSamples,
    const bool gpio0Released) {
    if (!gpio0Released) {
        return 0U;
    }
    return releasedSamples < RELEASE_STABLE_SAMPLES
               ? static_cast<uint8_t>(releasedSamples + 1U)
               : releasedSamples;
}

constexpr bool IsReleaseStable(const uint8_t releasedSamples) {
    return releasedSamples >= RELEASE_STABLE_SAMPLES;
}

bool ConfigureWakeStub(bool isS3);

} // namespace firmware::p1x_deep_wake
