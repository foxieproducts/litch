#pragma once

#include <cstdint>

#include "foxie_embedded/button.hpp"
#include "modules/ui_types.hpp"

namespace modules {

// Logical actions emitted after the physical control-bank orientation mapping.
// P1X/P2X and P1O/P2O identify the current game's owner of the action; pin and
// factory-test code must separately retain the printed physical P1/P2 bank.
enum class UserButton : uint8_t {
    Up = 0,
    Down = 1,
    Left = 2,
    Right = 3,
    Press = 4, // Reserved legacy numeric ID; never an actionable input.
    P1X = 5,
    P2X = 6,
    P1O = 7,
    P2O = 8,
    None = 0xFFU,
};

constexpr bool IsDpadButton(const UserButton button) {
    return button == UserButton::Up || button == UserButton::Down || button == UserButton::Left ||
           button == UserButton::Right;
}

constexpr bool IsXButton(const UserButton button) {
    return button == UserButton::P1X || button == UserButton::P2X;
}

constexpr bool IsOButton(const UserButton button) {
    return button == UserButton::P1O || button == UserButton::P2O;
}

struct ButtonEvent {
    UserButton button = UserButton::None;
    fxe::Button::Event event = fxe::Button::Event::Press;
    // Logical current-game owner after input normalization. Hardware test mode
    // deliberately disables that transform, so only there does this tag name
    // the printed physical control bank.
    UiOwner owner = UiOwner::None;
    uint32_t tMs = 0U;
    bool timestampValid = false;
};

constexpr UiOwner OwnerForPlayerButton(const UserButton button) {
    switch (button) {
        case UserButton::P1X:
        case UserButton::P1O:
            return UiOwner::P1;
        case UserButton::P2X:
        case UserButton::P2O:
            return UiOwner::P2;
        default:
            return UiOwner::None;
    }
}

// Input adapters own physical-to-logical normalization. Prefer their explicit
// owner tag; derive from the legacy P1/P2 button ID only for untagged callers.
constexpr UiOwner ResolveButtonEventOwner(const ButtonEvent& event) {
    return event.owner != UiOwner::None ? event.owner
                                       : OwnerForPlayerButton(event.button);
}

class IUserInput {
  public:
    virtual ~IUserInput() = default;

    // \brief Returns true if a new debounced button edge is available.
    virtual bool TryReadButtonEvent(ButtonEvent& outEvent) = 0;
};

} // namespace modules
