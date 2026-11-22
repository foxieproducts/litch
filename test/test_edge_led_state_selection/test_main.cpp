#include <cstdio>

#include "modules/edge_led_state_selection.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckMode(const modules::EdgeLedSelectionInputs& in,
               const modules::EdgeUiMode expected,
               const char* msg) {
    const modules::EdgeLedState st = modules::SelectEdgeLedState(in);
    Check(st.mode == expected, msg);
}

} // namespace

int main() {
    std::puts("[EDGE_LED_SELECT] start");

    modules::EdgeLedSelectionInputs in;
    in.activeSide = modules::UiOwner::P2;
    in.interactionActive = false;
    in.cpuThinkingActive = false;
    in.hintThinkingActive = false;

    // Default idle: turn indicator.
    {
        const modules::EdgeLedState st = modules::SelectEdgeLedState(in);
        Check(st.mode == modules::EdgeUiMode::IdleTurn, "idle -> IdleTurn");
        Check(st.activeSide == modules::UiOwner::P2, "idle keeps activeSide");
    }

    // CPU moving: prompt-style placement indicator (distinct from CPU thinking).
    {
        in.interactionActive = true;
        const modules::EdgeLedState st = modules::SelectEdgeLedState(in);
        Check(st.mode == modules::EdgeUiMode::Placement, "CpuMoving -> Placement");
    }

    // Human move states reuse placement intent and keep the home-edge prompt disabled.
    {
        in.interactionActive = true;
        const modules::EdgeLedState pickedUp = modules::SelectEdgeLedState(in);
        Check(pickedUp.mode == modules::EdgeUiMode::Placement, "PickedUp -> Placement");

        const modules::EdgeLedState placing = modules::SelectEdgeLedState(in);
        Check(placing.mode == modules::EdgeUiMode::Placement, "Placing -> Placement");

        const modules::EdgeLedState settling = modules::SelectEdgeLedState(in);
        Check(settling.mode == modules::EdgeUiMode::Placement, "Settling -> Placement");
    }

    // CPU thinking: explicit thinking mode.
    {
        in.interactionActive = false;
        in.cpuThinkingActive = true;
        const modules::EdgeLedState st = modules::SelectEdgeLedState(in);
        Check(st.mode == modules::EdgeUiMode::IdleTurn, "CPU analysis preserves ordinary Turn on the perimeter");
        in.cpuThinkingActive = false;
    }

    // Hint thinking retains ordinary guidance and never displays the CPU scanner.
    {
        in.interactionActive = false;
        in.hintThinkingActive = true;
        const modules::EdgeLedState st = modules::SelectEdgeLedState(in);
        Check(st.mode == modules::EdgeUiMode::IdleTurn, "hintThinkingActive -> ordinary Turn");
        in.hintThinkingActive = false;
    }

    // Promotion maps to placement.
    {
        in.interactionActive = true;
        const modules::EdgeLedState st = modules::SelectEdgeLedState(in);
        Check(st.mode == modules::EdgeUiMode::Placement, "Promotion -> Placement");
    }

    // Placement mode overrides game state.
    {
        in.interactionActive = false;
        in.placementModeActive = true;
        const modules::EdgeLedState st = modules::SelectEdgeLedState(in);
        Check(st.mode == modules::EdgeUiMode::Placement, "placementModeActive -> Placement");
        in.placementModeActive = false;
    }

    // Text overlay suppresses idle indicator.
    {
        in.textOverlayActive = true;
        const modules::EdgeLedState st = modules::SelectEdgeLedState(in);
        Check(st.mode == modules::EdgeUiMode::Menu, "textOverlayActive -> Menu(off)");
        Check(st.owner == modules::UiOwner::None, "textOverlayActive owner None");
        in.textOverlayActive = false;
    }

    // Illegal overrides idle.
    {
        in.illegalActive = true;
        const modules::EdgeLedState st = modules::SelectEdgeLedState(in);
        Check(st.mode == modules::EdgeUiMode::Error, "illegalActive -> Error");
        Check(st.activeSide == modules::UiOwner::P1, "illegalActive forces P1 side");
        in.illegalActive = false;
    }

    // Paused runtime health outranks correction and ordinary overlays while
    // remaining below settings/fatal safety ownership.
    {
        modules::EdgeLedSelectionInputs healthIn{};
        healthIn.runtimeHealthPaused = true;
        healthIn.illegalActive = true;
        healthIn.textOverlayActive = true;
        healthIn.placementModeActive = true;
        healthIn.cpuThinkingActive = true;
        CheckMode(healthIn, modules::EdgeUiMode::HealthPaused,
                  "paused health wins over correction and ordinary overlays");

        healthIn.settingsErrorActive = true;
        CheckMode(healthIn, modules::EdgeUiMode::Error,
                  "settings safety error remains above paused health");
    }

    // Settings error overrides idle.
    {
        in.settingsErrorActive = true;
        const modules::EdgeLedState st = modules::SelectEdgeLedState(in);
        Check(st.mode == modules::EdgeUiMode::Error, "settingsErrorActive -> Error");
        in.settingsErrorActive = false;
    }

    // Draw beats all non-checkmate states.
    {
        modules::EdgeLedSelectionInputs drawIn = in;
        drawIn.inDraw = true;
        drawIn.settingsErrorActive = true;
        drawIn.illegalActive = true;
        drawIn.textOverlayActive = true;
        drawIn.placementModeActive = true;
        drawIn.cpuThinkingActive = true;
        drawIn.hintThinkingActive = true;
        drawIn.interactionActive = true;

        const modules::EdgeLedState st = modules::SelectEdgeLedState(drawIn);
        Check(st.mode == modules::EdgeUiMode::FatalDraw, "draw -> FatalDraw");
    }

    // Priority order under contention: Error > text overlay > placement > thinking > state mapping.
    {
        modules::EdgeLedSelectionInputs priorityIn{};
        priorityIn.activeSide = modules::UiOwner::P2;
        priorityIn.interactionActive = true;
        priorityIn.cpuThinkingActive = true;
        priorityIn.hintThinkingActive = true;
        priorityIn.placementModeActive = true;
        priorityIn.textOverlayActive = true;
        priorityIn.illegalActive = true;
        CheckMode(priorityIn, modules::EdgeUiMode::Error,
                  "illegal wins over text overlay, placement, thinking, and game state");

        priorityIn.illegalActive = false;
        CheckMode(priorityIn, modules::EdgeUiMode::Menu,
                  "text overlay wins over placement, thinking, and game state");

        priorityIn.textOverlayActive = false;
        CheckMode(priorityIn, modules::EdgeUiMode::Placement,
                  "placement mode wins over thinking and game state");

        priorityIn.placementModeActive = false;
        CheckMode(priorityIn, modules::EdgeUiMode::Placement,
                  "required physical action retains perimeter priority during search");

        priorityIn.cpuThinkingActive = false;
        priorityIn.hintThinkingActive = false;
        CheckMode(priorityIn, modules::EdgeUiMode::Placement,
                  "CpuMoving falls back to placement intent when no higher priority state is active");
    }

    // Checkmate overrides all other flags.
    {
        in.inCheckmate = true;
        in.inDraw = true;
        in.illegalActive = true;
        in.settingsErrorActive = true;
        in.textOverlayActive = true;
        in.placementModeActive = true;
        in.cpuThinkingActive = true;
        in.hintThinkingActive = true;
        in.interactionActive = true;

        const modules::EdgeLedState st = modules::SelectEdgeLedState(in);
        Check(st.mode == modules::EdgeUiMode::FatalCheckmate, "checkmate -> FatalCheckmate");
    }

    if (g_failures == 0) {
        std::puts("[EDGE_LED_SELECT] ok");
        return 0;
    }

    std::printf("[EDGE_LED_SELECT] failures=%d\n", g_failures);
    return 1;
}
