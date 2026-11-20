#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "foxie_embedded/byte_order.hpp"
#include "modules/button_state_wire_codec.hpp"

namespace modules::physical_input_state_wire {

// One replaceable physical-truth key: all Hall occupancy and all twelve
// switches actually sampled by the current V2/V3 hardware profiles.
constexpr uint8_t STATE_CODE = 23U;
constexpr size_t BODY_BYTES = 10U;
constexpr uint8_t SWITCH_COUNT = 12U;
constexpr uint16_t SWITCH_MASK = (UINT16_C(1) << SWITCH_COUNT) - 1U;

constexpr int8_t PackedSwitchIndex(const ButtonEvent& event) {
    const int8_t logical = button_state_wire_codec::SwitchIndex(event);
    if (logical >= 0 && logical <= 3) return logical;
    if (logical >= 5 && logical <= 8) return logical - 1;
    if (logical >= 10 && logical <= 13) return logical - 2;
    return -1;
}

inline std::array<uint8_t, BODY_BYTES> Encode(const uint64_t occupancy,
                                               const uint16_t switches) {
    std::array<uint8_t, BODY_BYTES> body{};
    fxe::WriteU64LE(occupancy, body.data());
    fxe::WriteU16LE(static_cast<uint16_t>(switches & SWITCH_MASK),
                    body.data() + 8U);
    return body;
}

} // namespace modules::physical_input_state_wire
