#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/edge_led_constants.hpp"
#include "modules/promotion_ux.hpp"
#include "modules/rgb.hpp"
#include "modules/status_led_frame_adapter.hpp"
#include "hardware/status_led_chain_profile.hpp"

namespace modules::promotion_piece_cue {

inline constexpr uint8_t ICON_WIDTH = 6U;
inline constexpr uint8_t ICON_HEIGHT = 6U;
inline constexpr uint8_t ICON_ORIGIN_X = 1U;
inline constexpr uint8_t ICON_ORIGIN_Y = 1U;
inline constexpr uint32_t ARRIVAL_MS = 260U;

constexpr uint16_t ArrivalScale256(const uint32_t elapsed, const bool minimal) {
    constexpr uint32_t PEAK_MS = 110U;
    if (minimal) return 256U;
    if (elapsed < PEAK_MS) return 160U + 96U * elapsed / PEAK_MS;
    if (elapsed >= ARRIVAL_MS) return 192U;
    return 256U - 64U * (elapsed - PEAK_MS) / (ARRIVAL_MS - PEAK_MS);
}

// Semantic perimeter positions: two quiet pixels in the middle of each
// physical side edge. The hardware output adapter applies BOARD_ROTATE after
// composition, so this cue follows the complete control surface.
inline constexpr uint32_t HUMAN_LEFT_RIGHT_EDGE_MASK =
    (UINT32_C(1) << 11U) | (UINT32_C(1) << 12U) |
    (UINT32_C(1) << 27U) | (UINT32_C(1) << 28U);

struct Icon {
    std::array<uint8_t, ICON_HEIGHT> rows{};
};

// Six-by-six, code-native chess-piece silhouettes. Each row uses the low six
// bits with bit 5 at the visual left. They are deliberately broader than the
// readable text font and do not reuse any letter glyph.
inline constexpr Icon QUEEN_ICON{{
    0b101101U,
    0b011110U,
    0b001100U,
    0b011110U,
    0b011110U,
    0b111111U,
}};

inline constexpr Icon ROOK_ICON{{
    0b110011U,
    0b111111U,
    0b011110U,
    0b011110U,
    0b011110U,
    0b111111U,
}};

inline constexpr Icon BISHOP_ICON{{
    0b001100U,
    0b011110U,
    0b000100U,
    0b001100U,
    0b011110U,
    0b111111U,
}};

inline constexpr Icon KNIGHT_ICON{{
    0b001110U,
    0b011111U,
    0b011000U,
    0b011100U,
    0b001110U,
    0b011111U,
}};

constexpr const Icon& GetIcon(const modules::PromotionPiece piece) {
    switch (piece) {
        case modules::PromotionPiece::Queen:
            return QUEEN_ICON;
        case modules::PromotionPiece::Rook:
            return ROOK_ICON;
        case modules::PromotionPiece::Bishop:
            return BISHOP_ICON;
        case modules::PromotionPiece::Knight:
            return KNIGHT_ICON;
    }

    return QUEEN_ICON;
}

constexpr bool TryPieceFromSuffix(const char suffix,
                                  modules::PromotionPiece& outPiece) {
    switch (suffix) {
        case 'q':
        case 'Q':
            outPiece = modules::PromotionPiece::Queen;
            return true;
        case 'r':
        case 'R':
            outPiece = modules::PromotionPiece::Rook;
            return true;
        case 'b':
        case 'B':
            outPiece = modules::PromotionPiece::Bishop;
            return true;
        case 'n':
        case 'N':
            outPiece = modules::PromotionPiece::Knight;
            return true;
        default:
            return false;
    }
}

constexpr bool IsIconPixelOn(const Icon& icon,
                             const uint8_t row,
                             const uint8_t column) {
    if (row >= ICON_HEIGHT || column >= ICON_WIDTH) {
        return false;
    }
    const uint8_t bit = static_cast<uint8_t>(ICON_WIDTH - 1U - column);
    return ((icon.rows[row] >> bit) & 1U) != 0U;
}

constexpr uint64_t BoardMask(const modules::PromotionPiece piece) {
    const Icon& icon = GetIcon(piece);
    uint64_t mask = 0ULL;
    for (uint8_t row = 0U; row < ICON_HEIGHT; ++row) {
        for (uint8_t column = 0U; column < ICON_WIDTH; ++column) {
            if (!IsIconPixelOn(icon, row, column)) {
                continue;
            }
            const uint8_t square = static_cast<uint8_t>(
                (ICON_ORIGIN_Y + row) * 8U + ICON_ORIGIN_X + column);
            mask |= UINT64_C(1) << square;
        }
    }
    return mask;
}

inline void DrawIcon(std::array<modules::Rgb, 64U>& board,
                     const modules::PromotionPiece piece,
                     const modules::Rgb color,
                     const uint8_t alpha255 = 255U) {
    const Icon& icon = GetIcon(piece);
    for (uint8_t row = 0U; row < ICON_HEIGHT; ++row) {
        for (uint8_t column = 0U; column < ICON_WIDTH; ++column) {
            if (!IsIconPixelOn(icon, row, column)) {
                continue;
            }
            const size_t square = static_cast<size_t>(
                (ICON_ORIGIN_Y + row) * 8U + ICON_ORIGIN_X + column);
            board[square] = modules::AlphaBlendRgb(
                board[square], color, alpha255);
        }
    }
}

// Start at the crest; required controls remain visible at the half-intensity
// trough. A selection input resets the existing promotion clock.
inline uint16_t GuidanceScale256(const uint32_t elapsedMs) {
    constexpr uint32_t HALF_PERIOD_MS = 800U;
    const uint32_t phase = elapsedMs % (2U * HALF_PERIOD_MS);
    const uint32_t distance = phase <= HALF_PERIOD_MS
        ? phase : 2U * HALF_PERIOD_MS - phase;
    return static_cast<uint16_t>(256U - distance * 128U / HALF_PERIOD_MS);
}

inline void ApplyHumanLeftRightGuidance(
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>& edgeFrame,
    const modules::Rgb color,
    const uint32_t elapsedMs = 0U) {
    const auto restrained = modules::ScaleRgbSaturatingBy256(
        color, GuidanceScale256(elapsedMs) * 96U / 256U);
    for (uint8_t index = 0U; index < modules::EDGE_LED_COUNT; ++index) {
        if ((HUMAN_LEFT_RIGHT_EDGE_MASK & (UINT32_C(1) << index)) != 0U) {
            edgeFrame[index] = restrained;
        }
    }
}

inline void ComposeHumanControls(
    status_led_frame_adapter::FixedPhysicalStatusOverlay& output,
    const modules::Rgb color,
    const uint32_t elapsedMs) {
    const auto pulsed = ScaleRgbSaturatingBy256(color, GuidanceScale256(elapsedMs));
    // Either bank's O and the normalized Left/Right inputs are accepted by
    // PromotionUx. V3's four emulated positions are X, Left, Right, O.
    for (uint8_t bank = 0U; bank < 2U; ++bank) {
        for (const auto role : {hardware::ControlLedRole::Left,
                                hardware::ControlLedRole::Right,
                                hardware::ControlLedRole::O}) {
            const uint8_t index = bank * 6U + static_cast<uint8_t>(role);
            output.controlPixels[index] = pulsed;
            output.controlMask |= static_cast<uint16_t>(1U << index);
        }
        output.controlAlpha255[bank] = 255U;
        output.emulatedControlMask |= static_cast<uint8_t>(0x0FU << (bank * 4U));
        for (uint8_t position = 1U; position < 4U; ++position) {
            output.emulatedControlPixels[bank * 4U + position] = pulsed;
        }
    }
}

} // namespace modules::promotion_piece_cue
