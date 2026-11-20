#include "app_buttons.hpp"

#include <Arduino.h>

namespace {
bool ReadPinPressed(const uint8_t pin, const bool pressedState) {
    const int v = digitalRead(pin);
    const bool high = (v != 0);
    return pressedState ? high : !high;
}
} // namespace

void ConfigurePrimaryButton(fxe::Button& button,
                            const uint8_t pin,
                            const bool pressedState,
                            fxe::FixedFunction<void(fxe::Button::Event), 32> onEvent) {
    pinMode(pin, INPUT_PULLUP);

    fxe::Button::Config cfg;
    cfg.debounceMs = 20;

    cfg.longHoldMs = 1500;
    cfg.canRepeat = false;

    cfg.onEvent = std::move(onEvent);
    button.SetConfig(cfg);

    button.SetDetectionFn([pin, pressedState]() {
        return ReadPinPressed(pin, pressedState);
    });

    // Seed debounce logic with initial state.
    button.SeedRawDown(ReadPinPressed(pin, pressedState));
}
