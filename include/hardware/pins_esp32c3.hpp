#pragma once

#include <array>
#include <cstdint>

namespace hardware {

static constexpr bool HW_IS_S3 = false;
static constexpr bool HW_HAS_P2_CONTROLS = false;
static constexpr bool HW_HAS_EXPANDER_INTERRUPTS = false;
static constexpr bool HW_HAS_5V_EN = false;
static constexpr bool HW_HAS_LIGHT_SENSORS = false;
static constexpr bool HW_HAS_CUSTOM_I2C_PINS = false;
static constexpr uint8_t HW_REVISION_ID = 1U;
static constexpr bool HW_SUPPORTS_BLE_OTA = false;

// NeoPixel output (board-only; edge/status are virtual).
static constexpr uint8_t PIN_BOARD_LEDS = 2;
static constexpr uint8_t PIN_EDGE_LEDS = 2;

// Legacy alias used in older code paths.
static constexpr uint8_t PIN_LEDS = PIN_BOARD_LEDS;

// Power rail enable (not present on ESP32-C3 prototype).
static constexpr uint8_t PIN_5V_EN = 13;

// P1 d-pad pins (four active directions on C3 prototype).
static constexpr uint8_t PIN_P1_DPAD_UP = 10;
static constexpr uint8_t PIN_P1_DPAD_LEFT = 4;
static constexpr uint8_t PIN_P1_DPAD_DOWN = 7;
static constexpr uint8_t PIN_P1_DPAD_RIGHT = 9;

// Player buttons.
static constexpr uint8_t PIN_P1X = 21;
static constexpr uint8_t PIN_P1O = 20;
static constexpr uint8_t PIN_P2X = 0;
static constexpr uint8_t PIN_P2O = 0;

// P2 d-pad placeholders (unused on C3).
static constexpr uint8_t PIN_P2_DPAD_UP = 0;
static constexpr uint8_t PIN_P2_DPAD_LEFT = 0;
static constexpr uint8_t PIN_P2_DPAD_DOWN = 0;
static constexpr uint8_t PIN_P2_DPAD_RIGHT = 0;

// I2C pins (use defaults on C3).
static constexpr uint8_t PIN_I2C_SDA = 0;
static constexpr uint8_t PIN_I2C_SCL = 0;

// Expander interrupt pins (not present on C3 prototype).
static constexpr uint8_t PIN_PCA9555_INT0 = 0;
static constexpr uint8_t PIN_PCA9555_INT1 = 0;
static constexpr uint8_t PIN_PCA9555_INT2 = 0;
static constexpr uint8_t PIN_PCA9555_INT3 = 0;

// USB-C CC sense (not present on C3 prototype).
static constexpr uint8_t PIN_USB_CC1 = 0;
static constexpr uint8_t PIN_USB_CC2 = 0;

// Button GPIOs used for wake-from-deep-sleep.
static constexpr std::array<uint8_t, 6> WAKE_BUTTON_PINS = {
    PIN_P1_DPAD_UP,
    PIN_P1_DPAD_DOWN,
    PIN_P1_DPAD_LEFT,
    PIN_P1_DPAD_RIGHT,
    PIN_P1X,
    PIN_P1O,
};

// Subset used when configuring deep sleep wake sources.
// Prefer the d-pad as a wake source on this single-player prototype.
static constexpr std::array<uint8_t, 4> DEEP_SLEEP_WAKE_BUTTON_PINS = {
    PIN_P1_DPAD_UP,
    PIN_P1_DPAD_DOWN,
    PIN_P1_DPAD_LEFT,
    PIN_P1_DPAD_RIGHT,
};

// PCA9555 expander addresses.
static constexpr uint8_t PCA9555_ADDR_0 = 0x20;
static constexpr uint8_t PCA9555_ADDR_1 = 0x21;
static constexpr uint8_t PCA9555_ADDR_2 = 0x22;
static constexpr uint8_t PCA9555_ADDR_3 = 0x23;

} // namespace hardware
