#pragma once

#include "modules/ui_types.hpp"
#include "runtime/game_state_machine.hpp"

namespace modules {

struct HintButtonRoutingDecision {
    bool shouldTrackGesture = false;
    bool shouldRequestHintOnRelease = false;
};

// Saved preferences are independent of piece setup and correction. Required
// Circle actions retain the press; they must never become an optional hold.
inline bool CanTrackCoachingGesture(const runtime::GameStateMachine& state) {
    const auto& phase = state.GetChessInteractionModel().phase;
    const bool fixedHistory = phase.plan.source == runtime::PhysicalTransitionSource::Undo ||
        (phase.tag == runtime::ChessInteractionPhase::DuckRelocation &&
         phase.selectedSquare < 64U);
    return !state.IsTextOverlayActive() && !state.IsPromotionActive() &&
           !state.IsKingQueenSwapPromptActive() && !state.IsUndoPromptActive() &&
           !state.IsMoveSettlingActive() && !fixedHistory;
}

constexpr HintButtonRoutingDecision ResolveHintButtonRouting(const bool eligibleContext,
                                                             const bool isOButton,
                                                             const UiOwner buttonOwner,
                                                             const UiOwner activeTurnOwner,
                                                             const bool hintAvailable = true) {
    if (!eligibleContext || !isOButton || buttonOwner == UiOwner::None) {
        return HintButtonRoutingDecision{};
    }

    HintButtonRoutingDecision decision;
    decision.shouldTrackGesture = true;
    decision.shouldRequestHintOnRelease = hintAvailable && buttonOwner == activeTurnOwner;
    return decision;
}

} // namespace modules
