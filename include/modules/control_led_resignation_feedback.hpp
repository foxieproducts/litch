#pragma once

#include "modules/status_led_frame_adapter.hpp"
#include "modules/ui_types.hpp"

namespace modules {
void ComposeControlLedResignationFeedback(UiOwner owner, uint32_t elapsedMs,
    Rgb playerColor, status_led_frame_adapter::FixedPhysicalStatusOverlay& output);
} // namespace modules
