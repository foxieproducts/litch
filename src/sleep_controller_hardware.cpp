#include "firmware/sleep_controller_hardware_internal.hpp"

#ifdef ARDUINO

#include <Arduino.h>
#include <Wire.h>

#include <driver/gpio.h>
#include <soc/soc_caps.h>

#if SOC_USB_SERIAL_JTAG_SUPPORTED
#if __has_include(<driver/usb_serial_jtag.h>)
#include <driver/usb_serial_jtag.h>
#endif
#endif

#if defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
#include <tusb.h>
#endif

#include "logger.hpp"

namespace firmware::sleep_controller_internal {
namespace {

// Keep the native USB pull-up disconnected long enough for desktop hosts to
// retire the existing CDC interface before deep sleep resets the S3 and it
// enumerates again with the same identity. In particular, macOS can otherwise
// leave the BSD serial node present with stale, silent endpoints across rapid
// sleep/wake cycles.
constexpr uint32_t NATIVE_USB_HOST_DETACH_DWELL_MS = 250U;

struct I2cLineLevels {
    bool sdaHigh = true;
    bool sclHigh = true;
};

uint8_t GetI2cSdaPin(const hardware::BoardHardwareProfile& profile) {
    return profile.hasCustomI2cPins
               ? profile.i2cSda
               : static_cast<uint8_t>(SDA);
}

uint8_t GetI2cSclPin(const hardware::BoardHardwareProfile& profile) {
    return profile.hasCustomI2cPins
               ? profile.i2cScl
               : static_cast<uint8_t>(SCL);
}

void DisablePullsAndFloatPin(const uint8_t pin) {
    pinMode(pin, INPUT);
    const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
    (void)gpio_pullup_dis(gpio);
    (void)gpio_pulldown_dis(gpio);
    (void)gpio_hold_dis(gpio);
}

void DriveLowWithNoPulls(const uint8_t pin) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
    (void)gpio_pullup_dis(gpio);
    (void)gpio_pulldown_dis(gpio);
    (void)gpio_hold_dis(gpio);
}

I2cLineLevels ReadI2cLineLevels(
    const hardware::BoardHardwareProfile& profile) {
    return I2cLineLevels{
        digitalRead(GetI2cSdaPin(profile)) != LOW,
        digitalRead(GetI2cSclPin(profile)) != LOW,
    };
}

bool TryRecoverI2cBus(const hardware::BoardHardwareProfile& profile) {
    const uint8_t sdaPin = GetI2cSdaPin(profile);
    const uint8_t sclPin = GetI2cSclPin(profile);
    const I2cLineLevels initial = ReadI2cLineLevels(profile);
    if (initial.sdaHigh && initial.sclHigh) {
        return true;
    }

    DisablePullsAndFloatPin(sdaPin);
    pinMode(sclPin, OUTPUT_OPEN_DRAIN);
    digitalWrite(sclPin, HIGH);
    delayMicroseconds(5);
    for (uint8_t pulse = 0U; pulse < 9U; ++pulse) {
        digitalWrite(sclPin, LOW);
        delayMicroseconds(5);
        digitalWrite(sclPin, HIGH);
        delayMicroseconds(5);
        const I2cLineLevels levels = ReadI2cLineLevels(profile);
        if (levels.sdaHigh && levels.sclHigh) {
            DisablePullsAndFloatPin(sclPin);
            return true;
        }
    }
    DisablePullsAndFloatPin(sclPin);
    return false;
}

} // namespace

bool StopI2cForSleep(const hardware::BoardHardwareProfile& profile) {
    const I2cLineLevels before = ReadI2cLineLevels(profile);
    logger::Trace("sleep: i2c lines before end (sda=%u scl=%u)",
                 static_cast<unsigned>(before.sdaHigh ? 1U : 0U),
                 static_cast<unsigned>(before.sclHigh ? 1U : 0U));

    Wire.end();
    DisablePullsAndFloatPin(GetI2cSdaPin(profile));
    DisablePullsAndFloatPin(GetI2cSclPin(profile));

    I2cLineLevels after = ReadI2cLineLevels(profile);
    if (!after.sdaHigh || !after.sclHigh) {
        logger::Warn("sleep: i2c bus not released; attempting recovery");
        (void)TryRecoverI2cBus(profile);
        after = ReadI2cLineLevels(profile);
    }
    logger::Trace("sleep: i2c lines final (sda=%u scl=%u)",
                 static_cast<unsigned>(after.sdaHigh ? 1U : 0U),
                 static_cast<unsigned>(after.sclHigh ? 1U : 0U));
    return after.sdaHigh && after.sclHigh;
}

void ApplyGpioLeakagePolicyForSleep(
    const hardware::BoardHardwareProfile& profile,
    const SleepWakePinSet& runtimeButtonPins,
    const SleepWakePinSet& preservedInputPins) {
    for (uint8_t index = 0U; index < runtimeButtonPins.count; ++index) {
        const uint8_t pin = runtimeButtonPins.pins[index];
        if (!preservedInputPins.Contains(pin)) {
            DisablePullsAndFloatPin(pin);
        }
    }

    DriveLowWithNoPulls(profile.boardLedPin);
    if (profile.statusLedPin != profile.boardLedPin) {
        DriveLowWithNoPulls(profile.statusLedPin);
    }
    if (profile.hasExpanderInterrupts) {
        for (const uint8_t pin : profile.expanderInterrupts) {
            DisablePullsAndFloatPin(pin);
        }
    }
    if (profile.hasFiveVoltEnable) {
        DriveLowWithNoPulls(profile.fiveVoltEnable);
    }
    if (profile.hasLightSensors) {
        DisablePullsAndFloatPin(profile.usbCc1);
        DisablePullsAndFloatPin(profile.usbCc2);
    }
}

bool TryDetachUsbForSleep() {
#if defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
    Serial.flush();
    (void)tud_disconnect();
    // Leave USBCDC registered while TinyUSB and the host process the detach.
    // Serial.end() first would remove the CDC endpoint object immediately.
    delay(NATIVE_USB_HOST_DETACH_DWELL_MS);
    Serial.end();
    return true;
#elif SOC_USB_SERIAL_JTAG_SUPPORTED
    Serial.flush();
    Serial.end();
    delay(20);
#if __has_include(<driver/usb_serial_jtag.h>)
    (void)usb_serial_jtag_driver_uninstall();
    delay(20);
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

} // namespace firmware::sleep_controller_internal

#endif
