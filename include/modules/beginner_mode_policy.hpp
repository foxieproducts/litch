#pragma once

#include <cstdint>

#include "modules/engine.hpp"
#include "modules/game_variant.hpp"
#include "modules/ui_types.hpp"
#include "runtime/chess_interaction_model.hpp"

namespace modules {

class BeginnerModeController;

struct BeginnerModePolicyInputs {
    GameVariant variant = GameVariant::Classic;
    bool quickSettingsOverlayActive = false;
    bool setupCompleteActive = false;
    bool gameResetHoldActive = false;
    bool textOverlayActive = false;
    bool placementModeActive = false;
    bool kingQueenSwapPromptActive = false;
    bool undoPromptActive = false;
    bool promotionActive = false;
    bool moveSettlingActive = false;
    bool queuedCpuMove = false;
    uint8_t hintFromSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t hintToSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    SideToMove sideToMove = SideToMove::White;
    uint32_t vsCpuLevel = 0U;
    bool firstPlayerIsP2 = false;
    bool cpuPlayerIsP2 = true;
};

UiOwner ActiveBeginnerModeOwnerForTurn(
    const BeginnerModeController& controller,
    const BeginnerModePolicyInputs& inputs,
    const runtime::TaggedInteractionState& interaction);

// The enabled human owner is independent of temporary prompt suppression.
UiOwner BeginnerModeOwnerForHumanTurn(
    const BeginnerModeController& controller,
    const BeginnerModePolicyInputs& inputs);

// Hint eligibility requires a selectable destination. Fixed Undo/CancelRedo
// plans retain the Human source tag but own their own physical guidance.
bool IsHumanDuckRelocation(
    const runtime::TaggedInteractionState& interaction);

// A global suggestion can guide its own held piece, never another source.
bool IsMatchingHumanHintPickup(
    const runtime::TaggedInteractionState& interaction,
    uint8_t hintFromSquare);

// A revealed compound Duck suggestion stays useful while its matching chess
// choreography is held, placed or awaiting confirmation.
bool IsMatchingDuckHintChessProgress(
    const runtime::TaggedInteractionState& interaction,
    uint8_t hintFromSquare, uint8_t hintToSquare);

} // namespace modules

namespace runtime {

class GameStateMachine;

void SyncBeginnerModeHintDisplay(
    GameStateMachine& stateMachine,
    modules::UiOwner activeOwner,
    bool& forcedHintDisplay);

} // namespace runtime
