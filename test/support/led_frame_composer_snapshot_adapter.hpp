#pragma once

#include <array>

#include "hardware/led_layout.hpp"
#include "modules/edge_led_renderer.hpp"
#include "modules/engine.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "runtime/game_clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"

namespace test_support {

inline runtime::InteractionModelSnapshot BuildInteractionSnapshot(
    const runtime::GameStateMachine& stateMachine,
    const modules::EngineDerivedState& derived,
    const modules::IEngine* engine = nullptr,
    const runtime::GameClock* gameClock = nullptr) {
    // Isolated renderer fixtures have a quiescent engine rather than a
    // coordinator. Publish its fixed FEN before invoking the shared builder.
    char fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    if (engine != nullptr) {
        (void)engine->GetFen(fen, sizeof(fen));
    }
    return runtime::BuildInteractionModelSnapshot({
        .stateMachine = &stateMachine,
        .derived = &derived,
        .authoritativeFen = fen,
        .gameClock = gameClock,
    });
}

inline void ComposeWithSnapshot(
    modules::led_frame_composer::ComposeInputs inputs,
    const runtime::GameStateMachine& stateMachine,
    const modules::EngineDerivedState& derived,
    const modules::IEngine* engine,
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& outLedFrame,
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>& outEdgeFrame,
    std::array<modules::Rgb, 64>& outGridFrame,
    const runtime::GameClock* gameClock = nullptr) {
    runtime::InteractionModelSnapshot snapshot =
        BuildInteractionSnapshot(stateMachine, derived, engine, gameClock);
    snapshot.render.menuOverlayActive =
        inputs.menuOverlay != nullptr && inputs.menuOverlay->IsActive();
    inputs.interactionSnapshot = &snapshot;
    modules::led_frame_composer::Compose(
        inputs,
        outLedFrame,
        outEdgeFrame,
        outGridFrame);
}

inline bool BuildComposeContextWithSnapshot(
    modules::led_frame_composer::ComposeInputs& inputs,
    const runtime::GameStateMachine& stateMachine,
    const modules::EngineDerivedState& derived,
    const modules::IEngine* engine,
    modules::led_frame_composer::internal::ComposeContext& outContext,
    runtime::InteractionModelSnapshot& snapshot,
    const runtime::GameClock* gameClock = nullptr) {
    snapshot = BuildInteractionSnapshot(stateMachine, derived, engine, gameClock);
    snapshot.render.menuOverlayActive =
        inputs.menuOverlay != nullptr && inputs.menuOverlay->IsActive();
    inputs.interactionSnapshot = &snapshot;
    return modules::led_frame_composer::internal::BuildComposeContext(
        inputs,
        outContext);
}

} // namespace test_support
