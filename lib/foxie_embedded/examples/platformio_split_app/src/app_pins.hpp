#pragma once

#include <cstdint>

// Adjust these for your board.
static constexpr uint8_t PIN_PRIMARY_BUTTON = 0;

// Many dev boards wire buttons as active-low (pressed pulls pin to GND).
static constexpr bool PRIMARY_BUTTON_PRESSED_STATE = false;
