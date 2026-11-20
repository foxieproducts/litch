#pragma once

#include <cstdint>

#include "modules/engine.hpp"
#include "modules/ui_types.hpp"
#include "runtime/game_clock.hpp"

namespace modules {

constexpr Side OppositeSide(const Side side) {
    return (side == Side::P2) ? Side::P1 : Side::P2;
}

// Note: these helpers intentionally operate on chess sides (White/Black) only.

constexpr runtime::ClockSide ClockSideFromSideToMove(const SideToMove side) {
    return (side == SideToMove::Black) ? runtime::ClockSide::Black : runtime::ClockSide::White;
}

inline bool TrySideFromUiOwner(const UiOwner owner, Side& outSide) {
    if (owner == UiOwner::None) {
        return false;
    }
    outSide = (owner == UiOwner::P2) ? Side::P2 : Side::P1;
    return true;
}

} // namespace modules
