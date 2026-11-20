#pragma once

#include <cstdint>

#include "modules/edge_led_renderer.hpp"

namespace modules {

struct EdgeLedSelectionInputs {
    bool inCheckmate = false;
    bool inDraw = false;

    // Settings store disabled or error state.
    bool settingsErrorActive = false;
    bool runtimeHealthPaused = false;

    bool illegalActive = false;
    bool textOverlayActive = false;
    bool placementModeActive = false;

    // CPU is actively thinking/searching (engine running). This is distinct from
    // CpuMoving (physical move prompt) and should have a distinct edge indicator.
    bool cpuThinkingActive = false;
    bool hintThinkingActive = false;

    // True while a physical interaction, prompt, settle, or modal owns the board.
    bool interactionActive = false;

    // Side-to-move (or CPU side when thinking); used for IdleTurn/Thinking.
    UiOwner activeSide = UiOwner::P1;
};

inline EdgeLedState SelectEdgeLedState(const EdgeLedSelectionInputs& in) {
    EdgeLedState out;

    // Default to fully off for an explicit suppressing overlay.
    out.mode = EdgeUiMode::Menu;
    out.owner = UiOwner::None;
    out.value01_8 = 0;

    out.activeSide = in.activeSide;
    if (in.inCheckmate) {
        out.mode = EdgeUiMode::FatalCheckmate;
        return out;
    }

    if (in.inDraw) {
        out.mode = EdgeUiMode::FatalDraw;
        return out;
    }

    if (in.settingsErrorActive) {
        out.mode = EdgeUiMode::Error;
        out.activeSide = UiOwner::P1;
        return out;
    }

    if (in.runtimeHealthPaused) {
        out.mode = EdgeUiMode::HealthPaused;
        return out;
    }

    if (in.illegalActive) {
        out.mode = EdgeUiMode::Error;
        out.activeSide = UiOwner::P1;
        return out;
    }

    if (in.textOverlayActive) {
        out.mode = EdgeUiMode::Menu;
        out.owner = UiOwner::None;
        return out;
    }

    if (in.placementModeActive) {
        out.mode = EdgeUiMode::Placement;
        return out;
    }

    // Search feedback is composed on control roles, not the perimeter.
    if (in.interactionActive) {
        out.mode = EdgeUiMode::Placement;
        return out;
    }

    out.mode = EdgeUiMode::IdleTurn;
    return out;
}

} // namespace modules
