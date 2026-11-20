#pragma once

#include <cstdint>

#include "foxie_embedded/button.hpp"

// Configure a single button with sensible defaults.
void ConfigurePrimaryButton(fxe::Button& button,
                            uint8_t pin,
                            bool pressedState,
                            fxe::FixedFunction<void(fxe::Button::Event), 32> onEvent);
