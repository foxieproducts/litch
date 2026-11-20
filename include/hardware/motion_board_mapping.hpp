#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "hardware/board_revision.hpp"
#include "hardware/lis2dw12.hpp"
#include "modules/water_slosh.hpp"

namespace hardware {

// Signed one-based sensor axes, mapping specific force to apparent down-force.
// Zero is deliberately invalid; a hardware revision must have measured signs.
struct MotionBoardMapping {
    int8_t right = 0;
    int8_t towardP1 = 0;
    int8_t normal = 0;

    constexpr bool IsValid() const {
        const int x = right < 0 ? -right : right;
        const int y = towardP1 < 0 ? -towardP1 : towardP1;
        const int z = normal < 0 ? -normal : normal;
        return x >= 1 && x <= 3 && y >= 1 && y <= 3 && z >= 1 && z <= 3 &&
               x != y && x != z && y != z;
    }

    __attribute__((always_inline)) bool Transform(const Lis2dw12Sample& sample,
                   modules::WaterSloshMotion& result) const {
        if (!IsValid()) { return false; }
        const std::array<int32_t, 3> values{
            sample.milliGx, sample.milliGy, sample.milliGz};
        const auto axis = [&](const int8_t selected) __attribute__((always_inline)) {
            const float value = float(values[std::abs(int(selected)) - 1U]) * 0.001F;
            return selected < 0 ? -value : value;
        };
        result = {axis(right), axis(towardP1), std::fabs(axis(normal))};
        return true;
    }
};

constexpr MotionBoardMapping MotionBoardMappingFor(const BoardRevision revision) {
    // V3 bidirectional tilt capture, 2026-09-09: left-up => -Y, right-up => +Y;
    // P2-up => -X, P1-up => +X; face-up => +Z. Apparent gravity is opposite
    // specific force.
    return HasFourLedSquares(revision) ? MotionBoardMapping{-2, -1, 3}
                                          : MotionBoardMapping{};
}

} // namespace hardware
