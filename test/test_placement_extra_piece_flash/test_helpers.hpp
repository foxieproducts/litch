#pragma once

#include <array>
#include <cstdint>

#include "hardware/led_layout.hpp"
#include "modules/edge_led_constants.hpp"
#include "modules/rgb.hpp"

namespace placement_led_test_helpers {

inline modules::Rgb PieceLedAt(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
    const uint8_t physicalSquare) {
    return ledFrame[hardware::LedIndexForSquare(
        physicalSquare, hardware::SquareLedSlot::Piece)];
}

inline modules::Rgb SquareLedAt(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
    const uint8_t physicalSquare) {
    return ledFrame[hardware::LedIndexForSquare(
        physicalSquare, hardware::SquareLedSlot::Square0)];
}

inline modules::Rgb SquareLed2At(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
    const uint8_t physicalSquare) {
    return ledFrame[hardware::LedIndexForSquare(
        physicalSquare, hardware::SquareLedSlot::Square2)];
}

inline bool RgbEq(const modules::Rgb lhs, const modules::Rgb rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

inline bool RgbNe(const modules::Rgb lhs, const modules::Rgb rhs) {
    return !RgbEq(lhs, rhs);
}

inline bool AnyEdgeLit(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& edgeFrame) {
    for (const modules::Rgb rgb : edgeFrame) {
        if (rgb.r != 0U || rgb.g != 0U || rgb.b != 0U) {
            return true;
        }
    }
    return false;
}

inline bool AllEdgeLit(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& edgeFrame) {
    for (const modules::Rgb rgb : edgeFrame) {
        if (rgb.r == 0U && rgb.g == 0U && rgb.b == 0U) {
            return false;
        }
    }
    return true;
}

inline bool AllEdgeRedDominant(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& edgeFrame) {
    for (const modules::Rgb rgb : edgeFrame) {
        if ((rgb.r == 0U && rgb.g == 0U && rgb.b == 0U) ||
            rgb.r < rgb.g || rgb.r < rgb.b) {
            return false;
        }
    }
    return true;
}

} // namespace placement_led_test_helpers
