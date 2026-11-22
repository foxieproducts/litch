#pragma once

#include <cstdint>

#include "hardware/led_layout.hpp"
#include "runtime/square_mapping.hpp"

namespace hardware {

constexpr SquareLedSlot RotateSquareLedSlot180(const SquareLedSlot slot) {
    switch (slot) {
        case SquareLedSlot::Square0:
            return SquareLedSlot::Square2;
        case SquareLedSlot::Square2:
            return SquareLedSlot::Square0;
        default:
            return slot;
    }
}

// Maps a logical LED index (composed in canonical orientation) to the physical LED
// index for the current hardware orientation.
//
// BOARD_ROTATE semantics:
// - Board squares rotate 180 degrees: square index uses the stable involution 63-square,
//   and the per-square LED slots are rotated (Square0 <-> Square2).
// - Edge/status LEDs rotate 180 degrees: the edge ring is split into two 16-LED halves
//   and rotation swaps the halves (offset by EDGE_LED_COUNT/2).
constexpr uint16_t MapLedIndexForBoardRotate(const uint16_t logicalLedIndex, const bool rotate180) {
    if (!rotate180) {
        return logicalLedIndex;
    }

    if (logicalLedIndex < BOARD_LED_COUNT) {
        const uint16_t square = static_cast<uint16_t>(logicalLedIndex / LEDS_PER_SQUARE);
        const uint16_t slot = static_cast<uint16_t>(logicalLedIndex % LEDS_PER_SQUARE);
        const uint8_t mappedSquare = runtime::MapSquareIndex(static_cast<uint8_t>(square), true);
        const auto mappedSlot = RotateSquareLedSlot180(static_cast<SquareLedSlot>(slot));
        return static_cast<uint16_t>(LedIndexForSquare(mappedSquare, mappedSlot));
    }

    if constexpr (EDGE_LED_COUNT == 0) {
        return logicalLedIndex;
    } else {
        if (logicalLedIndex >= TOTAL_LED_COUNT) {
            return logicalLedIndex;
        }

        const uint16_t edgeIndex = static_cast<uint16_t>(logicalLedIndex - BOARD_LED_COUNT);
        // A 180-degree rotation swaps the two halves.
        const uint16_t half = static_cast<uint16_t>(EDGE_LED_COUNT / 2U);
        if ((EDGE_LED_COUNT % 2U) != 0U || half == 0U) {
            return logicalLedIndex;
        }

        const uint16_t mappedEdge = static_cast<uint16_t>((edgeIndex + half) % EDGE_LED_COUNT);
        return static_cast<uint16_t>(FirstEdgeLedIndex() + mappedEdge);
    }
}

// Like MapLedIndexForBoardRotate, but allows square LEDs and edge LEDs to be rotated
// independently.
//
// This is useful because GAME_FIRST_PLAYER can change the square mapping (who plays
// from which physical side) without physically rotating the hardware edge LED ring.
constexpr uint16_t MapLedIndexForSquareAndEdgeRotate(const uint16_t logicalLedIndex,
                                                     const bool squareRotate180,
                                                     const bool edgeRotate180) {
    if (logicalLedIndex < BOARD_LED_COUNT) {
        return MapLedIndexForBoardRotate(logicalLedIndex, squareRotate180);
    }
    return MapLedIndexForBoardRotate(logicalLedIndex, edgeRotate180);
}

} // namespace hardware
