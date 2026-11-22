#pragma once

#include <array>
#include <cstddef>

#include "modules/ui_types.hpp"

namespace modules {

static constexpr size_t EDGE_LED_COUNT = 32;

// Left-to-right from a player at their home edge; the ring runs in reverse.
constexpr uint8_t PlayerHomeLedIndex(const UiOwner owner, const uint8_t localIndex) {
    return static_cast<uint8_t>((owner == UiOwner::P2 ? 0U : 16U) +
        7U - (localIndex > 7U ? 7U : localIndex));
}

// Side-left, home-left, home-right, side-right, from the owner's perspective.
constexpr std::array<uint8_t, 4U> PlayerCornerLedIndices(const UiOwner owner) {
    return owner == UiOwner::P2 ? std::array<uint8_t, 4U>{8U, 7U, 0U, 31U}
                               : std::array<uint8_t, 4U>{24U, 23U, 16U, 15U};
}

} // namespace modules
