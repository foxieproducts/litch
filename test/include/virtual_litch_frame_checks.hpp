#pragma once

#include "hardware/led_layout.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/ui_types.hpp"
#include "runtime/virtual_litch_session.h"

namespace virtual_litch_test {

inline bool HasPhysicalCoachingBadge(
    const virtual_litch_physical_frame& frame,
    const modules::UiOwner physicalOwner,
    const hardware::BoardRevision revision = hardware::BoardRevision::V2S3) {
    const uint8_t homeStart =
        physicalOwner == modules::UiOwner::P2 ? 0U : 16U;
    const uint8_t sideLeft =
        physicalOwner == modules::UiOwner::P2 ? 8U : 24U;
    const uint8_t sideRight =
        physicalOwner == modules::UiOwner::P2 ? 31U : 15U;
    const uint16_t edgeStart = hardware::BoardLedTopologyFor(revision).physicalBoardLedCount;
    return frame.pixels[edgeStart + homeStart].w != 0U &&
           frame.pixels[edgeStart + homeStart + 7U].w != 0U &&
           frame.pixels[edgeStart + sideLeft].w != 0U &&
           frame.pixels[edgeStart + sideRight].w != 0U;
}

} // namespace virtual_litch_test
