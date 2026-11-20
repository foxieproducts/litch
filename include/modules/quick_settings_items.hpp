#pragma once

#include <cstdint>

#include "modules/board_owner_pairing_feature.hpp"

namespace modules::quick_settings_items {

inline constexpr uint8_t CPU_DIFFICULTY = 0U;
inline constexpr uint8_t CPU_PLAYER = 1U;
inline constexpr uint8_t FIRST_PLAYER = 2U;
inline constexpr uint8_t BRIGHTNESS = 3U;
inline constexpr uint8_t WHITE_SQUARES = 4U;
inline constexpr uint8_t ROTATE = 5U;
inline constexpr uint8_t TIMER = 6U;
inline constexpr uint8_t OWNER_PAIRING = 7U;
inline constexpr uint8_t TOTAL_COUNT = 8U;
inline constexpr uint8_t COUNT = OWNER_PAIRING_ENABLED ? TOTAL_COUNT : 7U;

} // namespace modules::quick_settings_items
