#pragma once

#include <cstdint>

namespace modules {

namespace menu_option_indices {
// Prototype menu ordering (no wrap):
// 0) CPU difficulty
// 1) Brightness
// 2) Player-side placement (legacy GAME_FIRST_PLAYER storage)
// 3) Flip board
// 4) Sleep
// 5) Timer
// 6) CPU player (Quick Setup identity only)
static constexpr uint8_t VS_CPU = 0;
static constexpr uint8_t BRIGHTNESS = 1;
static constexpr uint8_t GAME_FIRST_PLAYER = 2;
static constexpr uint8_t FLIP_SIDES = 3;
static constexpr uint8_t SLEEP = 4;
static constexpr uint8_t TIMER = 5;
static constexpr uint8_t CPU_PLAYER = 6;
} // namespace menu_option_indices

} // namespace modules
