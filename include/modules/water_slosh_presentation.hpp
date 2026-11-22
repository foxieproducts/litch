#pragma once

#include "hardware/board_led_output_mapping.hpp"
#include "modules/edge_led_constants.hpp"
#include "modules/water_slosh_session.hpp"
#include "runtime/interaction_domain_types.hpp"
#include "runtime/runtime_health.hpp"

namespace modules {

struct WaterSloshPresentationContext {
    runtime::RuntimeHealthState healthState = runtime::RuntimeHealthState::Healthy;
    bool boardObserved = false;
    bool higherPriorityActive = false;
    bool awake = false;
    bool minimalAnimations = false;
};

bool CanPresentWaterSlosh(const runtime::InteractionModelSnapshot& snapshot,
                         const WaterSloshPresentationContext& context);
uint8_t WaterSloshRiderSquare(const runtime::InteractionModelSnapshot& snapshot);

// Blend the physically measured surface only after semantic output has crossed
// the single orientation/topology mapping boundary.
void ApplyWaterSloshPhysicalPresentation(
    const WaterSloshSession& water,
    hardware::BoardRevision revision,
    hardware::PhysicalLedFrame& physicalFrame,
    uint8_t physicalRiderSquare = 255U);

} // namespace modules
