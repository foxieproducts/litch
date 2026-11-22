#pragma once

#include "modules/orientation.hpp"

namespace modules {

// Apply a full 180-degree "rotate" semantics to a button event:
// - P1 buttons become P2, and vice-versa.
// - D-pad events keep their button (Up/Down/Left/Right) but swap owners.
inline void ApplyRotateToButtonEvent(ButtonEvent& event, const bool rotate) {
    if (!rotate) {
        return;
    }

    event.owner = SwapP1P2Owner(event.owner);
    event.button = SwapP1P2Button(event.button);
}

} // namespace modules
