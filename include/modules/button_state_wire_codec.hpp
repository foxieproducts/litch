#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "foxie_embedded/byte_order.hpp"
#include "modules/user_input.hpp"

namespace modules::button_state_wire_codec {

constexpr uint8_t VERSION = 1U;
constexpr uint8_t FIRST_STATE_CODE = 23U;
constexpr uint8_t SWITCH_COUNT = 14U;
constexpr uint8_t LAST_STATE_CODE = FIRST_STATE_CODE + SWITCH_COUNT - 1U;
constexpr size_t BODY_BYTES = 8U;

constexpr int8_t SwitchIndex(const ButtonEvent& event) {
    const UiOwner owner = ResolveButtonEventOwner(event);
    if (event.button <= UserButton::Right) {
        if (owner != UiOwner::P1 && owner != UiOwner::P2) return -1;
        const uint8_t ownerOffset = owner == UiOwner::P1 ? 0U : 5U;
        return static_cast<int8_t>(ownerOffset + static_cast<uint8_t>(event.button));
    }
    switch (event.button) {
    case UserButton::P1X: return owner == UiOwner::P1 ? 10 : -1;
    case UserButton::P1O: return owner == UiOwner::P1 ? 11 : -1;
    case UserButton::P2X: return owner == UiOwner::P2 ? 12 : -1;
    case UserButton::P2O: return owner == UiOwner::P2 ? 13 : -1;
    default: return -1;
    }
}

inline std::array<uint8_t, BODY_BYTES> Encode(const ButtonEvent& event,
                                               const bool down,
                                               const uint32_t observedAtMs) {
    std::array<uint8_t, BODY_BYTES> body{};
    body[0U] = VERSION;
    body[1U] = static_cast<uint8_t>(event.button);
    body[2U] = static_cast<uint8_t>(ResolveButtonEventOwner(event));
    body[3U] = down ? 1U : 0U;
    fxe::WriteU32LE(observedAtMs, body.data() + 4U);
    return body;
}

} // namespace modules::button_state_wire_codec
