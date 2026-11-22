#include "hardware/gpio_diagnostics.hpp"

#ifdef ARDUINO

#include <Arduino.h>

namespace hardware {

int ReadDigitalLevel(const uint8_t pin) {
    return digitalRead(pin);
}

} // namespace hardware

#else

namespace hardware {

int ReadDigitalLevel(const uint8_t) {
    return 1;
}

} // namespace hardware

#endif
