#include <array>
#include <cstdint>
#include <cstdio>

#include "modules/litchcraft_shake_detector.hpp"

namespace {
int failures = 0;
void Check(const bool condition, const char* const message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++failures;
    }
}

bool DeliberateShake(modules::LitchcraftShakeDetector& detector,
                     uint32_t& nowMs, const bool eligible = true) {
    detector.Observe(0, 0, 1000, nowMs, eligible);
    bool fired = false;
    for (uint8_t sample = 0U; sample < 27U; ++sample) {
        nowMs += modules::LitchcraftShakeDetector::SAMPLE_PERIOD_MS;
        const int32_t direction = sample % 2U == 0U ? 700 : -700;
        fired = detector.Observe(direction, -direction / 2, 1000, nowMs,
                                 eligible) || fired;
    }
    return fired;
}
struct BlockShakeResult {
    uint32_t triggerAtMs = 0U;
    uint8_t triggers = 0U;
};

BlockShakeResult BlockShake(
    modules::LitchcraftShakeDetector& detector, const int32_t deltaX,
    const std::array<uint8_t, 4U>& blocks) {
    // Acceleration ramps stay centered within the sensor's +/-2 g axis range.
    // Nonempty blocks alternate delta direction; four give three reversals.
    int32_t x = -deltaX * blocks[0U] / 2;
    int32_t y = x / 2;
    uint32_t nowMs = 0U;
    detector.Observe(x, y, 1000, nowMs, true);
    BlockShakeResult result;
    bool positive = true;
    for (const uint8_t samples : blocks) {
        const int32_t direction = positive ? deltaX : -deltaX;
        for (uint8_t sample = 0U; sample < samples; ++sample) {
            nowMs += modules::LitchcraftShakeDetector::SAMPLE_PERIOD_MS;
            x += direction;
            y += direction / 2;
            if (detector.Observe(x, y, 1000, nowMs, true)) {
                result.triggerAtMs = nowMs;
                ++result.triggers;
            }
        }
        positive = !positive;
    }
    return result;
}
} // namespace

int main() {
    using modules::LitchcraftShakeDetector;
    std::puts("[LITCHCRAFT_SHAKE_DETECTOR] start");

    LitchcraftShakeDetector deliberate;
    uint32_t nowMs = 0U;
    Check(DeliberateShake(deliberate, nowMs),
          "one second of alternating deliberate motion fires once");
    Check(deliberate.IsCoolingDown(),
          "accepted gesture enters quiet rearm cooldown");
    Check(!DeliberateShake(deliberate, nowMs),
          "continued shaking cannot retrigger during cooldown");
    for (uint8_t sample = 0U; sample < 17U; ++sample) {
        nowMs += LitchcraftShakeDetector::SAMPLE_PERIOD_MS;
        deliberate.Observe(0, 0, 1000, nowMs, true);
    }
    Check(!deliberate.IsCoolingDown(),
          "sustained quiet rearms the next deliberate gesture");

    for (const int32_t deltaX : {300, 310, 400}) {
        LitchcraftShakeDetector threeReversals;
        const auto result = BlockShake(threeReversals, deltaX, {7U, 7U, 7U, 5U});
        std::printf("[SHAKE_THREE_REVERSALS] dx=%d dy=%d triggers=%u at=%u\n",
                    static_cast<int>(deltaX), static_cast<int>(deltaX / 2),
                    result.triggers, result.triggerAtMs);
        Check(result.triggers == 1U && result.triggerAtMs == 1040U,
              "exactly three reversals qualify only after one full energetic second");
    }
    LitchcraftShakeDetector twoReversals;
    Check(BlockShake(twoReversals, 400, {9U, 9U, 8U, 0U}).triggers == 0U,
          "two energetic reversals over one second remain rejected");
    LitchcraftShakeDetector shortShake;
    Check(BlockShake(shortShake, 400, {6U, 6U, 6U, 5U}).triggers == 0U,
          "three energetic reversals shorter than one second remain rejected");

    LitchcraftShakeDetector rotation;
    nowMs = 0U;
    rotation.Observe(0, 0, 1000, nowMs, true);
    bool rotated = false;
    for (uint8_t sample = 1U; sample <= 30U; ++sample) {
        nowMs += 40U;
        rotated = rotation.Observe(sample * 33, 0, 1000 - sample * 20,
                                   nowMs, true) || rotated;
    }
    Check(!rotated, "slow board rotation cannot satisfy shake energy");

    LitchcraftShakeDetector carry;
    nowMs = 0U;
    carry.Observe(0, 0, 1000, nowMs, true);
    bool carried = false;
    for (uint8_t sample = 0U; sample < 40U; ++sample) {
        nowMs += 40U;
        const int32_t step = sample % 2U == 0U ? 140 : -140;
        carried = carry.Observe(step, step / 2, 1000, nowMs, true) || carried;
    }
    Check(!carried, "ordinary alternating carry motion stays below threshold");

    LitchcraftShakeDetector tap;
    nowMs = 0U;
    tap.Observe(0, 0, 1000, nowMs, true);
    nowMs += 40U;
    Check(!tap.Observe(1200, -500, 1000, nowMs, true),
          "one piece-like impact does not trigger");
    for (uint8_t sample = 0U; sample < 30U; ++sample) {
        nowMs += 40U;
        Check(!tap.Observe(0, 0, 1000, nowMs, true),
              "quiet after an isolated impact remains rejected");
    }

    LitchcraftShakeDetector interrupted;
    nowMs = 0U;
    interrupted.Observe(0, 0, 1000, nowMs, true);
    for (uint8_t sample = 0U; sample < 10U; ++sample) {
        nowMs += 40U;
        const int32_t direction = sample % 2U == 0U ? 700 : -700;
        interrupted.Observe(direction, -direction / 2, 1000, nowMs, true);
    }
    nowMs += LitchcraftShakeDetector::MAX_SAMPLE_GAP_MS;
    Check(!interrupted.Observe(700, -350, 1000, nowMs, true),
          "sample gap resets partial gesture progress");
    Check(!DeliberateShake(interrupted, nowMs, false),
          "ineligible occupancy or lifecycle state rejects and resets shaking");
    Check(DeliberateShake(interrupted, nowMs, true),
          "fresh eligible sustained motion works after reset");

    std::printf("[LITCHCRAFT_SHAKE_DETECTOR] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
