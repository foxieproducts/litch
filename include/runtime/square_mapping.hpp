#pragma once

#include <cstdint>

#include "runtime/board_constants.hpp"

namespace runtime {

constexpr bool IsValidSquareIndex(const uint8_t square) {
    return square < BOARD_SQUARE_COUNT;
}

// Maps logical 0..63 (0=a8,63=h1) to physical index. When flipSides=true,
// mapping is the stable involution `63-index`.
constexpr uint8_t MapSquareIndex(const uint8_t logicalSquare,
                                 const bool flipSides) {
    return flipSides ? static_cast<uint8_t>((BOARD_SQUARE_COUNT - 1U) - logicalSquare) : logicalSquare;
}

} // namespace runtime
