#pragma once

#include <cstdint>

#include "runtime/board_constants.hpp"

namespace hardware {

static constexpr uint16_t SQUARE_COUNT = runtime::BOARD_SQUARE_COUNT;
static constexpr uint16_t LEDS_PER_SQUARE = 3;
static constexpr uint16_t BOARD_LED_COUNT = static_cast<uint16_t>(SQUARE_COUNT * LEDS_PER_SQUARE);

// Edge/status LEDs are after the 64*3 square LEDs.
//
// Notes:
// - Edge/status LEDs: 32 total (8 per side) (224 total LEDs)
//
// The build can override this with -D LITCH_EDGE_LED_COUNT=<n>.
#ifndef LITCH_EDGE_LED_COUNT
#define LITCH_EDGE_LED_COUNT 32
#endif

static constexpr uint16_t EDGE_LED_COUNT = static_cast<uint16_t>(LITCH_EDGE_LED_COUNT);
static constexpr uint16_t TOTAL_LED_COUNT = static_cast<uint16_t>(BOARD_LED_COUNT + EDGE_LED_COUNT);

enum class SquareLedSlot : uint8_t {
    Square0 = 0,
    Piece = 1,
    Square2 = 2,
};

static constexpr uint16_t LedIndexForSquare(const uint8_t square, const SquareLedSlot slot) {
    return static_cast<uint16_t>(static_cast<uint16_t>(square) * LEDS_PER_SQUARE + static_cast<uint8_t>(slot));
}

static constexpr uint16_t FirstEdgeLedIndex() {
    return BOARD_LED_COUNT;
}

} // namespace hardware
