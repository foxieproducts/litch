#pragma once

#include <array>
#include <cstdint>

namespace hardware {

static constexpr bool HW_IS_S3 = true;
static constexpr bool HW_HAS_P2_CONTROLS = true;
static constexpr bool HW_HAS_EXPANDER_INTERRUPTS = true;
static constexpr bool HW_HAS_5V_EN = true;
static constexpr bool HW_HAS_LIGHT_SENSORS = true;
static constexpr bool HW_HAS_CUSTOM_I2C_PINS = true;
static constexpr uint8_t HW_REVISION_ID = 2U;
static constexpr bool HW_SUPPORTS_BLE_OTA = true;

// NeoPixel outputs (separate board + status LED chains).
static constexpr uint8_t PIN_BOARD_LEDS = 47;
static constexpr uint8_t PIN_EDGE_LEDS = 33;

// Legacy alias used in older code paths.
static constexpr uint8_t PIN_LEDS = PIN_BOARD_LEDS;

// Power rail enable (S3 only).
static constexpr uint8_t PIN_5V_EN = 13;

// P1 d-pad pins.
// Mapping: DPAD1=UP, DPAD2=LEFT, DPAD4=DOWN, DPAD5=RIGHT.
static constexpr uint8_t PIN_P1_DPAD_UP = 7;    // P1_DPAD1
static constexpr uint8_t PIN_P1_DPAD_LEFT = 5;  // P1_DPAD2
static constexpr uint8_t PIN_P1_DPAD_DOWN = 6;  // P1_DPAD4
static constexpr uint8_t PIN_P1_DPAD_RIGHT = 9; // P1_DPAD5

// P1 buttons (note: P1_LEFT is a boot strap).
static constexpr uint8_t PIN_P1X = 0;  // P1_LEFT
static constexpr uint8_t PIN_P1O = 10; // P1_RIGHT

// P2 d-pad pins.
// Mapping: DPAD1=UP, DPAD2=LEFT, DPAD4=DOWN, DPAD5=RIGHT.
static constexpr uint8_t PIN_P2_DPAD_UP = 17;    // P2_DPAD1
static constexpr uint8_t PIN_P2_DPAD_LEFT = 15;  // P2_DPAD2
static constexpr uint8_t PIN_P2_DPAD_DOWN = 16;  // P2_DPAD4
static constexpr uint8_t PIN_P2_DPAD_RIGHT = 21; // P2_DPAD5

// P2 buttons.
static constexpr uint8_t PIN_P2X = 14; // P2_LEFT
static constexpr uint8_t PIN_P2O = 26; // P2_RIGHT

// I2C pins.
static constexpr uint8_t PIN_I2C_SDA = 3;
static constexpr uint8_t PIN_I2C_SCL = 4;

// Expander interrupt pins (per quadrant).
static constexpr uint8_t PIN_PCA9555_INT0 = 34; // INT_EXP_QUAD_1
static constexpr uint8_t PIN_PCA9555_INT1 = 12; // INT_EXP_QUAD_2
static constexpr uint8_t PIN_PCA9555_INT2 = 48; // INT_EXP_QUAD_3
static constexpr uint8_t PIN_PCA9555_INT3 = 11; // INT_EXP_QUAD_4

// USB-C CC sense (ADC).
static constexpr uint8_t PIN_USB_CC1 = 1;
static constexpr uint8_t PIN_USB_CC2 = 2;

// Button GPIOs used for wake-from-deep-sleep.
static constexpr std::array<uint8_t, 12> WAKE_BUTTON_PINS = {
    PIN_P1_DPAD_UP,
    PIN_P1_DPAD_DOWN,
    PIN_P1_DPAD_LEFT,
    PIN_P1_DPAD_RIGHT,
    PIN_P1X,
    PIN_P1O,
    PIN_P2_DPAD_UP,
    PIN_P2_DPAD_DOWN,
    PIN_P2_DPAD_LEFT,
    PIN_P2_DPAD_RIGHT,
    PIN_P2X,
    PIN_P2O,
};

// Subset used when configuring deep sleep wake sources.
// Use d-pad buttons for wake (and avoid P1X/P2X strap/button pins).
static constexpr std::array<uint8_t, 8> DEEP_SLEEP_WAKE_BUTTON_PINS = {
    PIN_P1_DPAD_UP,
    PIN_P1_DPAD_DOWN,
    PIN_P1_DPAD_LEFT,
    PIN_P1_DPAD_RIGHT,
    PIN_P2_DPAD_UP,
    PIN_P2_DPAD_DOWN,
    PIN_P2_DPAD_LEFT,
    PIN_P2_DPAD_RIGHT,
};

// PCA9555 expander addresses.
static constexpr uint8_t PCA9555_ADDR_0 = 0x20;
static constexpr uint8_t PCA9555_ADDR_1 = 0x21;
static constexpr uint8_t PCA9555_ADDR_2 = 0x22;
static constexpr uint8_t PCA9555_ADDR_3 = 0x23;

} // namespace hardware
