#pragma once

#include <cstdint>

#include "modules/board_led_frame.hpp"
#include "modules/rgb.hpp"

namespace modules::startup_choreography {

constexpr uint16_t EaseScale256(const uint16_t scale256) {
    const uint32_t t = scale256;
    return static_cast<uint16_t>((t * t * (768U - 2U * t)) >> 16U);
}

constexpr uint16_t StaggeredScale256(const uint16_t progress256,
                                     const uint8_t ordinal,
                                     const uint8_t count) {
    if (progress256 >= 256U) {
        return 256U;
    }
    if (count <= 1U) {
        return EaseScale256(progress256);
    }
    // Starts span 60% of the episode; each item fades over the remaining 40%.
    constexpr uint16_t START_SPAN_256 = 154U;
    const uint16_t start = static_cast<uint16_t>(
        static_cast<uint32_t>(ordinal) * START_SPAN_256 / (count - 1U));
    if (progress256 <= start) {
        return 0U;
    }
    const uint16_t local = static_cast<uint16_t>(
        (static_cast<uint32_t>(progress256 - start) * 256U) /
        (256U - START_SPAN_256));
    return EaseScale256(local > 256U ? 256U : local);
}

constexpr uint8_t LightSquareOrdinal(const uint8_t square) {
    const uint8_t rank = static_cast<uint8_t>(square / 8U);
    const uint8_t file = static_cast<uint8_t>(square % 8U);
    const uint8_t withinRank = static_cast<uint8_t>(file / 2U);
    return static_cast<uint8_t>(rank * 4U +
        ((rank & 1U) == 0U ? withinRank : 3U - withinRank));
}

constexpr uint16_t GridScale256(const uint16_t progress256,
                                const uint8_t square) {
    return StaggeredScale256(
        progress256, LightSquareOrdinal(square), 32U);
}

inline void ApplyGrid(BoardLedFrame& frame, const uint16_t progress256) {
    for (uint8_t square = 0U; square < 64U; ++square) {
        const uint16_t scale = GridScale256(progress256, square);
        auto& leds = frame[square];
        leds.left = ScaleRgbSaturatingBy256(leds.left, scale);
        leds.top = ScaleRgbSaturatingBy256(leds.top, scale);
        leds.right = ScaleRgbSaturatingBy256(leds.right, scale);
        leds.bottom = ScaleRgbSaturatingBy256(leds.bottom, scale);
    }
}

constexpr uint8_t OccupiedOrdinal(const uint64_t occupied,
                                  const uint8_t square) {
    uint8_t ordinal = 0U;
    for (uint8_t index = 0U; index < square; ++index) {
        if ((occupied & (UINT64_C(1) << index)) != 0ULL) {
            ++ordinal;
        }
    }
    return ordinal;
}

constexpr uint8_t OccupiedCount(const uint64_t occupied) {
    uint8_t count = 0U;
    for (uint8_t index = 0U; index < 64U; ++index) {
        if ((occupied & (UINT64_C(1) << index)) != 0ULL) {
            ++count;
        }
    }
    return count;
}

constexpr uint16_t PieceScale256(const uint16_t progress256,
                                 const uint64_t occupied,
                                 const uint8_t square) {
    return StaggeredScale256(progress256, OccupiedOrdinal(occupied, square),
                             OccupiedCount(occupied));
}

} // namespace modules::startup_choreography
