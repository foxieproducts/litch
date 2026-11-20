#include "firmware/sleep_countdown_led_frame.hpp"

namespace firmware {
namespace {

constexpr uint8_t INITIAL_GATHERED_LED_COUNT = 3U;
constexpr uint8_t MIN_VALUE = 40U;
constexpr uint8_t PULSE_SPAN = 16U;
constexpr uint32_t PULSE_HALF_PERIOD_MS = 600U;
constexpr uint32_t PULSE_PERIOD_MS = PULSE_HALF_PERIOD_MS * 2U;

uint8_t SemanticIndexForGatherOrder(const uint8_t order) {
    if (order == 0U) {
        return 0U;
    }
    if ((order & 1U) != 0U) {
        return static_cast<uint8_t>((order + 1U) / 2U);
    }
    return static_cast<uint8_t>(
        modules::EDGE_LED_COUNT - (order / 2U));
}

uint8_t CueValue(const uint32_t elapsedMs) {
    const uint32_t phase = elapsedMs % PULSE_PERIOD_MS;
    const uint32_t triangle = phase <= PULSE_HALF_PERIOD_MS
                                  ? phase
                                  : PULSE_PERIOD_MS - phase;
    return static_cast<uint8_t>(
        MIN_VALUE +
        (triangle * PULSE_SPAN) / PULSE_HALF_PERIOD_MS);
}

} // namespace

SleepCountdownLedFrame ComposeSleepCountdownLedFrame(
    const SleepState state,
    const uint32_t remainingCountdownMs,
    const uint32_t countdownDurationMs) {
    SleepCountdownLedFrame frame;
    if (countdownDurationMs == 0U ||
        (state != SleepState::Countdown && state != SleepState::Fading &&
         state != SleepState::Preparing)) {
        return frame;
    }

    frame.active = true;
    const uint32_t durationMs = countdownDurationMs > 1U
        ? countdownDurationMs : 2U;
    const uint32_t boundedRemaining = state == SleepState::Countdown
        ? (remainingCountdownMs > durationMs ? durationMs : remainingCountdownMs)
        : 0U;
    const uint32_t elapsedMs = durationMs - boundedRemaining;
    const uint32_t growingCount =
        INITIAL_GATHERED_LED_COUNT +
        (elapsedMs *
         (modules::EDGE_LED_COUNT - INITIAL_GATHERED_LED_COUNT)) /
            (durationMs - 1U);
    frame.gatheredLedCount = static_cast<uint8_t>(
        growingCount > modules::EDGE_LED_COUNT
            ? modules::EDGE_LED_COUNT
            : growingCount);

    const uint8_t value = CueValue(elapsedMs);
    const modules::Rgb color{
        value,
        static_cast<uint8_t>(value / 3U),
        0U,
    };
    for (uint8_t order = 0U; order < frame.gatheredLedCount; ++order) {
        frame.semanticStatus[SemanticIndexForGatherOrder(order)] = color;
    }
    return frame;
}

} // namespace firmware
