#pragma once

#include "modules/edge_led_renderer.hpp"

namespace modules {

void ApplyHintCornerFeedback(const EdgeLedState& state,
                             std::array<Rgb, EDGE_LED_COUNT>& frame);

} // namespace modules
