#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/home_state_notification.hpp"

namespace modules::home_state_wire {

inline constexpr uint8_t VERSION = 1U;
inline constexpr uint8_t PROMOTION_VERSION = 2U;
inline constexpr uint8_t PROMOTION_SELECTION_VERSION = 3U;
inline constexpr size_t PAYLOAD_BYTES = 100U;
inline constexpr size_t FRAGMENT_BYTES = 10U;
inline constexpr uint8_t FIRST_STATE_CODE = 4U;
inline constexpr uint8_t FRAGMENT_COUNT = 10U;
inline constexpr uint8_t LAST_STATE_CODE =
    FIRST_STATE_CODE + FRAGMENT_COUNT - 1U;

// Byte 6 flags: rotation, White owner, placement, Fresh Setup, Vex, Undo,
// clock validity, authoritative king/queen swap prompt (bits 0..7).
// With byte 8 bit 6 clear (CPU idle), byte 71 is HomeClassicResult. Earlier
// boards wrote zero and earlier apps ignore this slot. With the bit set,
// bytes 71..74 remain the complete little-endian CPU elapsed half-seconds.
using Payload = std::array<uint8_t, PAYLOAD_BYTES>;

bool Encode(const HomeStateNotification& snapshot, Payload& out);

} // namespace modules::home_state_wire
