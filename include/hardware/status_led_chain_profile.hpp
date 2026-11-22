#pragma once

#include <cstdint>

namespace hardware {

constexpr uint8_t SEMANTIC_STATUS_LED_COUNT = 32U;
constexpr uint8_t V3_STATUS_CHAIN_LED_COUNT = 32U;
constexpr uint8_t V3_3_CONTROL_LED_COUNT = 12U;
constexpr uint8_t V3_3_STATUS_CHAIN_LED_COUNT = 44U;

struct PhysicalStatusLedRange {
    uint8_t first = 0U;
    uint8_t count = 0U;
};

// V3.3 physical chain: LED257..LED300. Directions stay local to each user.
// Both banks are O, Up, Right, Down, Left, X in DOUT-to-DIN chain order.
// PCB switch nets place P2 O beside LED261 and P2 X beside LED266;
// board/game orientation swaps banks, never these local button roles.
constexpr PhysicalStatusLedRange V3_3_P2_CONTROL_LED_RANGE{4U, 6U};
constexpr PhysicalStatusLedRange V3_3_P1_CONTROL_LED_RANGE{26U, 6U};

enum class ControlLedRole : uint8_t {
    O = 0U, Up, Right, Down, Left, X,
};

constexpr uint8_t V3_3PhysicalIndexForControl(
    const bool physicalP2Bank, const ControlLedRole role) {
    const auto range = physicalP2Bank ? V3_3_P2_CONTROL_LED_RANGE
                                      : V3_3_P1_CONTROL_LED_RANGE;
    return static_cast<uint8_t>(range.first + static_cast<uint8_t>(role));
}

// The caller supplies ComputeControlBankSwap, shared with physical button input.
// Directions stay local to the selected bank; only the owner bank changes.
constexpr uint8_t V3_3PhysicalIndexForLogicalControl(
    const bool logicalP2Owner, const ControlLedRole role,
    const bool controlBanksSwapped) {
    return V3_3PhysicalIndexForControl(
        logicalP2Owner != controlBanksSwapped, role);
}

constexpr uint8_t RotateSemanticStatusIndex180(
    const uint8_t semanticIndex,
    const bool rotate180) {
    if (!rotate180 || semanticIndex >= SEMANTIC_STATUS_LED_COUNT) {
        return semanticIndex;
    }

    return static_cast<uint8_t>((semanticIndex + 16U) %
                                SEMANTIC_STATUS_LED_COUNT);
}

constexpr uint8_t V3_3PhysicalIndexForSemanticStatus(
    const uint8_t semanticIndex,
    const bool rotate180 = false) {
    if (semanticIndex >= SEMANTIC_STATUS_LED_COUNT) {
        return V3_3_STATUS_CHAIN_LED_COUNT;
    }

    const uint8_t rotated =
        RotateSemanticStatusIndex180(semanticIndex, rotate180);
    if (rotated >= 20U) {
        return static_cast<uint8_t>(rotated + 12U);
    }
    if (rotated >= 4U) {
        return static_cast<uint8_t>(rotated + 6U);
    }
    return rotated;
}

} // namespace hardware
