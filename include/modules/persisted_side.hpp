#pragma once

#include <cstdint>

#include "modules/engine.hpp"

namespace modules {

enum class PersistedSide : uint8_t {
    White = 0,
    Black = 1,
    Unknown = 255,
};

constexpr uint8_t PersistedSideToU8(const PersistedSide side) {
    return static_cast<uint8_t>(side);
}

constexpr PersistedSide PersistedSideFromU8(const uint8_t value) {
    return (value == 0U) ? PersistedSide::White : ((value == 1U) ? PersistedSide::Black : PersistedSide::Unknown);
}

constexpr PersistedSide PersistedSideFromSideToMove(const SideToMove side) {
    return (side == SideToMove::Black) ? PersistedSide::Black : PersistedSide::White;
}

} // namespace modules
