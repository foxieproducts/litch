#include "modules/beginner_mode_policy.hpp"

#include "modules/beginner_mode_controller.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/vs_cpu_turn.hpp"
#include "runtime/game_state_machine.hpp"

namespace modules {

bool IsMatchingHumanHintPickup(
    const runtime::TaggedInteractionState& interaction,
    const uint8_t hintFromSquare) {
    return hintFromSquare < 64U &&
           interaction.tag == runtime::ChessInteractionPhase::PieceLifted &&
           (interaction.requiredAction == runtime::RequiredPhysicalAction::None ||
            interaction.requiredAction == runtime::RequiredPhysicalAction::PlacePrimary) &&
           interaction.selectedSquare == hintFromSquare;
}

bool IsHumanDuckRelocation(
    const runtime::TaggedInteractionState& interaction) {
    return interaction.tag == runtime::ChessInteractionPhase::DuckRelocation &&
           interaction.plan.kind == runtime::PhysicalTransitionKind::DuckRelocation &&
           interaction.plan.source == runtime::PhysicalTransitionSource::Human &&
           interaction.selectedSquare == runtime::CHESS_INTERACTION_INVALID_SQUARE &&
           interaction.requiredAction != runtime::RequiredPhysicalAction::CorrectBoard;
}

bool IsMatchingDuckHintChessProgress(
    const runtime::TaggedInteractionState& interaction,
    const uint8_t hintFromSquare, const uint8_t hintToSquare) {
    if (IsMatchingHumanHintPickup(interaction, hintFromSquare)) return true;
    const auto& plan = interaction.plan;
    return hintFromSquare < 64U && hintToSquare < 64U &&
        plan.moveFrom == hintFromSquare && plan.moveTo == hintToSquare &&
        plan.source == runtime::PhysicalTransitionSource::Human &&
        plan.kind != runtime::PhysicalTransitionKind::None &&
        plan.kind != runtime::PhysicalTransitionKind::DuckRelocation &&
        plan.kind != runtime::PhysicalTransitionKind::Undo &&
        interaction.requiredAction != runtime::RequiredPhysicalAction::CorrectBoard &&
        (interaction.tag == runtime::ChessInteractionPhase::ExecutingPlan ||
         interaction.tag == runtime::ChessInteractionPhase::Settling ||
         interaction.tag == runtime::ChessInteractionPhase::AwaitingAction);
}

UiOwner ActiveBeginnerModeOwnerForTurn(
    const BeginnerModeController& controller,
    const BeginnerModePolicyInputs& inputs,
    const runtime::TaggedInteractionState& interaction) {
    const bool compoundChessProgress = inputs.variant == GameVariant::Duck &&
        IsMatchingDuckHintChessProgress(interaction, inputs.hintFromSquare, inputs.hintToSquare);
    const bool eligiblePhase =
        interaction.tag == runtime::ChessInteractionPhase::Idle ||
        IsMatchingHumanHintPickup(interaction, inputs.hintFromSquare) || compoundChessProgress ||
        (inputs.variant == GameVariant::Duck && IsHumanDuckRelocation(interaction));
    if (inputs.quickSettingsOverlayActive || inputs.setupCompleteActive ||
        inputs.gameResetHoldActive || inputs.textOverlayActive ||
        inputs.placementModeActive || inputs.kingQueenSwapPromptActive ||
        inputs.undoPromptActive || inputs.promotionActive ||
        (inputs.moveSettlingActive && !compoundChessProgress) || inputs.queuedCpuMove ||
        !eligiblePhase) {
        return UiOwner::None;
    }
    return BeginnerModeOwnerForHumanTurn(controller, inputs);
}

UiOwner BeginnerModeOwnerForHumanTurn(
    const BeginnerModeController& controller,
    const BeginnerModePolicyInputs& inputs) {
    const bool isHumanTurn = !IsVsCpuTurn(
        inputs.sideToMove,
        inputs.vsCpuLevel,
        inputs.firstPlayerIsP2,
        inputs.cpuPlayerIsP2);
    const UiOwner turnOwner =
        led_frame_composer::OwnerForSideToMove(
            inputs.sideToMove, inputs.firstPlayerIsP2);
    return controller.ActiveOwnerForTurn(turnOwner, isHumanTurn);
}

} // namespace modules

namespace runtime {

void SyncBeginnerModeHintDisplay(
    GameStateMachine& stateMachine,
    const modules::UiOwner activeOwner,
    bool& forcedHintDisplay) {
    if (activeOwner != modules::UiOwner::None) {
        stateMachine.SetHintDisplayRequested(true);
        forcedHintDisplay = true;
        return;
    }
    if (!forcedHintDisplay) {
        return;
    }
    stateMachine.SetHintDisplayRequested(false);
    forcedHintDisplay = false;
}

} // namespace runtime
