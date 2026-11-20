#pragma once

#include <cstdint>

#include "modules/ui_types.hpp"
#include "modules/user_input.hpp"
#include "runtime/square_mapping.hpp"

namespace modules {

// Board orientation helpers.
//
// Terminology:
// - boardRotate == BOARD_ROTATE (180-degree physical board rotation semantics)
// - firstPlayerIsP2 == GAME_FIRST_PLAYER (logical P1's selected game end)
// - squareRotate == logical-to-raw mapping at sensor/render boundaries

// Physical rotation compensates the board's pose. Gameplay additionally moves
// logical P1's pieces and controls to the selected end. Artwork has no players.
constexpr bool ComputeSquareRotate(const bool boardRotate, const bool firstPlayerIsP2,
                                   const bool litchcraft = false) {
    return boardRotate != (!litchcraft && firstPlayerIsP2);
}

// Gameplay assigns logical P1/P2 to the selected near/far control ends.
// Litchcraft has no players and follows literal board orientation alone.
constexpr bool ComputeControlBankSwap(const bool boardRotate,
                                      const bool firstPlayerIsP2,
                                      const bool litchcraft) {
    return boardRotate != (!litchcraft && firstPlayerIsP2);
}

constexpr UiOwner SwapP1P2Owner(const UiOwner owner) {
    switch (owner) {
        case UiOwner::P1:
            return UiOwner::P2;
        case UiOwner::P2:
            return UiOwner::P1;
        default:
            return owner;
    }
}

constexpr UserButton SwapP1P2Button(const UserButton button) {
    switch (button) {
        case UserButton::P1X:
            return UserButton::P2X;
        case UserButton::P2X:
            return UserButton::P1X;
        case UserButton::P1O:
            return UserButton::P2O;
        case UserButton::P2O:
            return UserButton::P1O;
        default:
            return button;
    }
}

// Note: squareRotate is a 180-degree mapping, so the same mapping function applies in
// both directions (logical->physical and physical->logical).
constexpr uint8_t MapSquareToPhysical(const uint8_t logicalSq, const bool squareRotate) {
    return runtime::MapSquareIndex(logicalSq, squareRotate);
}

constexpr uint8_t MapSquareToLogical(const uint8_t physicalSq, const bool squareRotate) {
    return runtime::MapSquareIndex(physicalSq, squareRotate);
}

// Map an owner identity to the physical home edge where that owner's UI should
// appear after the composed player-end and literal board rotation.
constexpr UiOwner MapOwnerToPhysicalHomeEdge(const UiOwner owner,
                                             const bool squareRotate) {
    return squareRotate ? SwapP1P2Owner(owner) : owner;
}

} // namespace modules
