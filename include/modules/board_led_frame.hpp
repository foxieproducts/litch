#pragma once

#include <array>
#include <cstdint>

#include "hardware/led_layout.hpp"
#include "modules/animation_timing.hpp"

namespace modules {

enum class BoardLedSlots : uint8_t {
    None = 0,
    Left = 1U << 0,
    Top = 1U << 1,
    Bottom = 1U << 3,
    PiecePair = (1U << 1) | (1U << 3),
    Right = 1U << 2,
    SquarePair = (1U << 0) | (1U << 2),
    All = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3),
};

constexpr BoardLedSlots operator|(const BoardLedSlots lhs,
                                  const BoardLedSlots rhs) {
    return static_cast<BoardLedSlots>(static_cast<uint8_t>(lhs) |
                                      static_cast<uint8_t>(rhs));
}

inline bool HasBoardLedSlot(const BoardLedSlots mask,
                            const BoardLedSlots slot) {
    return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(slot)) != 0U;
}

// Board-coordinate directions. A 180-degree output transform swaps both pairs.
struct BoardSquareLeds {
    modules::Rgb left = modules::RGB_BLACK;
    modules::Rgb top = modules::RGB_BLACK;
    modules::Rgb right = modules::RGB_BLACK;
    modules::Rgb bottom = modules::RGB_BLACK;
};

using BoardLedFrame = std::array<BoardSquareLeds, 64>;

inline void ApplyToBoardSquare(BoardSquareLeds& square,
                               const BoardLedSlots slots,
                               const modules::Rgb color,
                               const uint8_t alpha = 255U,
                               const modules::animation::BlendMode mode = modules::animation::BlendMode::Replace) {
    if (HasBoardLedSlot(slots, BoardLedSlots::Left)) {
        modules::animation::ApplyBlend(square.left, color, alpha, mode);
    }

    if (HasBoardLedSlot(slots, BoardLedSlots::Top)) {
        modules::animation::ApplyBlend(square.top, color, alpha, mode);
    }

    if (HasBoardLedSlot(slots, BoardLedSlots::Bottom)) {
        modules::animation::ApplyBlend(square.bottom, color, alpha, mode);
    }

    if (HasBoardLedSlot(slots, BoardLedSlots::Right)) {
        modules::animation::ApplyBlend(square.right, color, alpha, mode);
    }
}

inline modules::Rgb RepresentativeSquareColor(const BoardSquareLeds& square) {
    return modules::Rgb{
        (square.left.r > square.right.r) ? square.left.r : square.right.r,
        (square.left.g > square.right.g) ? square.left.g : square.right.g,
        (square.left.b > square.right.b) ? square.left.b : square.right.b,
    };
}

// Legacy projection happens only at compatibility boundaries, before RGBW.
inline Rgb RoundedPieceAverage(const BoardSquareLeds& square) {
    return Rgb{static_cast<uint8_t>((uint16_t(square.top.r) + square.bottom.r + 1U) / 2U),
               static_cast<uint8_t>((uint16_t(square.top.g) + square.bottom.g + 1U) / 2U),
               static_cast<uint8_t>((uint16_t(square.top.b) + square.bottom.b + 1U) / 2U)};
}

inline void SetPiecePair(BoardSquareLeds& square, const Rgb color) {
    square.top = color;
    square.bottom = color;
}

inline std::array<Rgb, hardware::TOTAL_LED_COUNT> ProjectLegacyBoardFrame(
    const BoardLedFrame& frame) {
    std::array<Rgb, hardware::TOTAL_LED_COUNT> out{};
    for (uint8_t square = 0U; square < 64U; ++square) {
        out[square * 3U] = frame[square].left;
        out[square * 3U + 1U] = RoundedPieceAverage(frame[square]);
        out[square * 3U + 2U] = frame[square].right;
    }
    return out;
}

inline BoardLedFrame ExpandLegacyBoardFrame(
    const std::array<Rgb, hardware::TOTAL_LED_COUNT>& legacy) {
    BoardLedFrame frame{};
    for (uint8_t square = 0U; square < 64U; ++square) {
        frame[square] = {legacy[square * 3U], legacy[square * 3U + 1U],
                         legacy[square * 3U + 2U], legacy[square * 3U + 1U]};
    }
    return frame;
}

} // namespace modules
