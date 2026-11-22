#pragma once

#include <cstdint>

#include "runtime/chess_interaction_model.hpp"

namespace runtime {

// One relocation-sized slice of a larger physical transition. Renderers use
// this to guide exactly one piece at a time while the reducer remains the sole
// authority for legal progress and action ordering.
struct DirectedPhysicalPrompt {
    uint8_t relocationIndex = 0U;
    uint8_t fromSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t toSquare = CHESS_INTERACTION_INVALID_SQUARE;
    RequiredPhysicalAction action = RequiredPhysicalAction::None;
};

inline bool TryGetDirectedPhysicalPrompt(
    const TaggedInteractionState& interaction,
    DirectedPhysicalPrompt& outPrompt) {
    uint8_t relocationIndex = 0U;
    switch (interaction.requiredAction) {
        case RequiredPhysicalAction::LiftPrimary:
        case RequiredPhysicalAction::PlacePrimary:
            relocationIndex = 0U;
            break;
        case RequiredPhysicalAction::LiftSecondary:
        case RequiredPhysicalAction::PlaceSecondary:
            relocationIndex = 1U;
            break;
        default:
            return false;
    }

    const PhysicalTransitionPlan& plan = interaction.plan;
    if (relocationIndex >= plan.relocationCount) {
        return false;
    }
    const PieceRelocation relocation = plan.relocations[relocationIndex];
    if (relocation.from >= 64U || relocation.to >= 64U) {
        return false;
    }

    outPrompt.relocationIndex = relocationIndex;
    outPrompt.fromSquare = relocation.from;
    outPrompt.toSquare = relocation.to;
    outPrompt.action = interaction.requiredAction;
    return true;
}

inline bool IsSecondaryPhysicalPrompt(
    const TaggedInteractionState& interaction) {
    DirectedPhysicalPrompt prompt;
    return TryGetDirectedPhysicalPrompt(interaction, prompt) &&
           prompt.relocationIndex == 1U;
}

} // namespace runtime
