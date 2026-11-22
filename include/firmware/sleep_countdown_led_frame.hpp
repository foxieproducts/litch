#pragma once

#include <array>
#include <cstdint>

#include "firmware/sleep_transition.hpp"
#include "modules/edge_led_constants.hpp"
#include "modules/rgb.hpp"

namespace firmware {

struct SleepCountdownLedFrame {
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> semanticStatus{};
    uint8_t gatheredLedCount = 0U;
    bool active = false;
};

/**
 * \brief Compose a deterministic, calm perimeter-gathering sleep cue.
 *
 * The cue is required state feedback: a contiguous warm arc grows in both
 * directions from semantic status index zero. Its persistent spatial boundary
 * is the non-color channel; a slow bounded pulse is secondary. This function
 * expresses semantic intent only. Orientation, virtual/dedicated status
 * mapping, brightness, RGBW conversion, and power limiting remain downstream.
 */
SleepCountdownLedFrame ComposeSleepCountdownLedFrame(
    SleepState state,
    uint32_t remainingCountdownMs,
    uint32_t countdownDurationMs = SleepTransition::DEFAULT_COUNTDOWN_MS);

} // namespace firmware
