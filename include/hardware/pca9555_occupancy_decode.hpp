#pragma once

#include <cstdint>

#include "hardware/board_revision.hpp"

namespace hardware {

// Legacy-accurate PCA9555 expander bit/pin -> square occupancy decode.
//
// Mapping contract:
// - Returns occupancy bits for square indices 0..63 (0=a8 .. 63=h1)
// - Does NOT apply flip-sides. Higher layers handle that.
//
// Notes:
// - Legacy firmware reads pins as: occupied = (expValue & (1U << (15 - pin)))
//   which is preserved here for compatibility.

constexpr uint8_t PCA9555_EXPANDER_COUNT = 4;
constexpr uint8_t PCA9555_PINS_PER_EXPANDER = 16;

// Mapping extracted from legacy litch firmware chessboard_hardware.cpp (BuildMapping).
// Indexing is [expander][pin] -> squareIndex (0=a8 .. 63=h1).
constexpr uint8_t EXPANDER_PIN_TO_SQUARE_C3[PCA9555_EXPANDER_COUNT][PCA9555_PINS_PER_EXPANDER] = {
    // Expander 0
    {19, 18, 17, 16, 27, 26, 25, 24, 0, 1, 2, 3, 8, 9, 10, 11},
    // Expander 1
    {31, 30, 29, 28, 23, 22, 21, 20, 12, 13, 14, 15, 4, 5, 6, 7},
    // Expander 2
    {51, 50, 49, 48, 59, 58, 57, 56, 32, 33, 34, 35, 40, 41, 42, 43},
    // Expander 3
    {63, 62, 61, 60, 55, 54, 53, 52, 44, 45, 46, 47, 36, 37, 38, 39},
};

// ESP32-S3 prototype mapping captured from the measured hall sensor wiring.
// Update this table and its paired native tests together when hardware
// revisions change the expander routing.
constexpr uint8_t EXPANDER_PIN_TO_SQUARE_S3[PCA9555_EXPANDER_COUNT][PCA9555_PINS_PER_EXPANDER] = {
    // Expander 0
    {0, 1, 2, 3, 11, 10, 9, 8, 16, 17, 18, 19, 27, 26, 25, 24},
    // Expander 1
    {31, 30, 29, 28, 20, 21, 22, 23, 15, 14, 13, 12, 4, 5, 6, 7},
    // Expander 2
    {32, 33, 34, 35, 43, 42, 41, 40, 48, 49, 50, 51, 59, 58, 57, 56},
    // Expander 3
    {63, 62, 61, 60, 52, 53, 54, 55, 47, 46, 45, 44, 36, 37, 38, 39},
};

inline const uint8_t (*GetExpanderPinToSquareMap()) [PCA9555_PINS_PER_EXPANDER] {
#if defined(LITCH_HW_VARIANT_S3)
    return EXPANDER_PIN_TO_SQUARE_S3;
#else
    return EXPANDER_PIN_TO_SQUARE_C3;
#endif
}

inline const uint8_t (*GetExpanderPinToSquareMap(
    const BoardRevision revision))[PCA9555_PINS_PER_EXPANDER] {
    if (ResolveBoardRevision(revision) == BoardRevision::V1C3) {
        return EXPANDER_PIN_TO_SQUARE_C3;
    }

    // Page 3 of the V3 schematic preserves the measured V2 S3 routing.
    return EXPANDER_PIN_TO_SQUARE_S3;
}

// Overload that allows future hardware revisions to provide a different mapping table
// without rewriting the occupancy decode logic.
inline uint64_t DecodePca9555Occupancy64(const uint16_t exp0,
                                         const uint16_t exp1,
                                         const uint16_t exp2,
                                         const uint16_t exp3,
                                         const uint8_t mapping[PCA9555_EXPANDER_COUNT][PCA9555_PINS_PER_EXPANDER]) {
    const uint16_t expValues[PCA9555_EXPANDER_COUNT] = {exp0, exp1, exp2, exp3};

    uint64_t occ64 = 0;
    for (uint8_t exp = 0; exp < PCA9555_EXPANDER_COUNT; ++exp) {
        const uint16_t expValue = expValues[exp];
        for (uint8_t pin = 0; pin < PCA9555_PINS_PER_EXPANDER; ++pin) {
            const uint8_t square = mapping[exp][pin];
            const uint8_t bit = static_cast<uint8_t>(15U - pin);
            const bool occupied = (expValue & (static_cast<uint16_t>(1U) << bit)) != 0;
            if (occupied) {
                occ64 |= (1ULL << square);
            }
        }
    }

    return occ64;
}

inline uint64_t DecodePca9555Occupancy64(const uint16_t exp0, const uint16_t exp1, const uint16_t exp2, const uint16_t exp3) {
    return DecodePca9555Occupancy64(exp0, exp1, exp2, exp3, GetExpanderPinToSquareMap());
}

} // namespace hardware
