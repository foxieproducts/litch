#pragma once

#include <array>
#include <cstdint>

#include "foxie_embedded/button.hpp"
#include "modules/draw_rules.hpp"
#include "modules/engine.hpp"
#include "modules/game_variant.hpp"
#include "modules/persisted_side.hpp"
#include "modules/promotion_ux.hpp"
#include "modules/ui_types.hpp"
#include "modules/user_input.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/game_clock.hpp"
#include "runtime/game_state_machine_render_state.hpp"

namespace runtime {

enum class SetupLifecycle : uint8_t {
    Gameplay = 0,
    FreshSetup,
};

constexpr SetupLifecycle ClassifySetupLifecycle(
    const modules::GameVariant variant,
    const modules::DuckTurnPhase duckTurnPhase,
    const uint64_t movedSquaresMask) {
    if (movedSquaresMask != 0ULL ||
        duckTurnPhase != modules::DuckTurnPhase::ChessMove) {
        return SetupLifecycle::Gameplay;
    }

    return variant == modules::GameVariant::Undecided ||
                   variant == modules::GameVariant::Duck
               ? SetupLifecycle::FreshSetup
               : SetupLifecycle::Gameplay;
}

// Foundation vocabulary for the firmware "middle" refactor.
//
// The current code still dispatches directly from hardware/button polling into
// GameStateMachine and builds render frames from scattered state queries. These
// types define the explicit seams we want to preserve while we migrate that
// flow incrementally toward:
//   input event -> interaction transition -> acknowledged adapter effect -> render

struct SensorChangeEvent {
    uint32_t tMs = 0;
    uint8_t rawSquare = 0;
    uint8_t logicalSquare = 0;
    bool nowOccupied = false;
};

struct BoardObservationEvent {
    uint32_t tMs = 0;
    uint32_t sequence = 0;
    uint64_t changedMask = 0;
    uint64_t occupiedMask = 0;
};

struct ButtonInputEvent {
    uint32_t tMs = 0;
    modules::UserButton button = modules::UserButton::None;
    fxe::Button::Event event = fxe::Button::Event::Press;
    modules::UiOwner owner = modules::UiOwner::None;
    bool litchcraftEntryArbitrated = false;
    bool timestampValid = false;
};

struct PhysicalBoardState {
    uint64_t physicalOccupiedMask = 0;
    uint64_t expectedOccupiedMask = 0;
    uint64_t moverOccupiedMask = 0;
    uint64_t opponentOccupiedMask = 0;
    std::array<char, 64> enginePieces{};
    bool enginePiecesValid = false;
};

struct InteractionStateView {
    uint64_t validDestinationMask = 0;
    uint64_t captureFirstCapturerMask = 0ULL;
    uint8_t captureFirstEnPassantLanding = CHESS_INTERACTION_INVALID_SQUARE;
    // Legal castling is a coordinated two-piece action. These masks let the
    // renderer distinguish it from an ordinary king destination without
    // re-reading engine state or inferring intent from distance.
    uint64_t castlingKingDestinationMask = 0;
    uint64_t castlingRookSourceMask = 0;
    uint64_t humanIllegalMask = 0;
    uint64_t cpuIllegalMask = 0;
    uint64_t cpuTentativeCorrectionMask = 0;
    bool undoPromptActive = false;
    bool kingQueenSwapPromptActive = false;
    bool placementModeActive = false;
    uint64_t placementMissingMask = 0;
    uint64_t placementExtraMask = 0;
    bool interactionStartedFromMismatch = false;

    // One next-side pickup may arrive while the completed move still awaits
    // its adapter acknowledgement. It is legal progress owned by the replay
    // bridge, not an invalid-board mismatch.
    uint64_t deferredFollowupMask = 0;

    TaggedInteractionState taggedState{};

    bool visualActiveSideOverrideValid = false;
    modules::PersistedSide visualActiveSideOverride = modules::PersistedSide::Unknown;
    bool statusEdgeActiveSideOverrideValid = false;
    modules::PersistedSide statusEdgeActiveSideOverride = modules::PersistedSide::Unknown;

    uint32_t undoPromptElapsedMs = 0;
    uint8_t undoFromSquare = 255;
    uint8_t undoToSquare = 255;
    bool undoMoverSideKnown = false;
    modules::PersistedSide undoMoverSide = modules::PersistedSide::Unknown;
    bool undoRestoreValid = false;
    uint8_t undoRestoreSquare = 255;
    char undoRestorePieceFen = '\0';
    bool undoMoverPieceFenKnown = false;
    char undoMoverPieceFen = '\0';
    bool undoCastlingRookMoveValid = false;
    uint8_t undoCastlingRookFrom = 255;
    uint8_t undoCastlingRookTo = 255;

    uint64_t kingQueenSwapPromptMask = 0;
    uint64_t kingQueenSwapPromptFlashMask = 0;

    modules::PromotionPiece promotionSelection = modules::PromotionPiece::Queen;
    uint32_t promotionElapsedMs = 0U;

    bool cpuPromotionValid = false;
    char cpuPromotionSuffixLower = '\0';
    uint32_t cpuMovePromptElapsedMs = 0;
    bool illegalOn = false;
    uint32_t illegalElapsedMs = 0;
    uint32_t illegalPeriodMs = 1;
    bool kingSafetyPromptActive = false;
    uint8_t kingSafetyKingSquare = 255;
    uint8_t kingSafetyAttackerSquare = 255;
    uint64_t captureReturnMask = 0;
    bool captureReturnOn = false;
    uint32_t captureReturnElapsedMs = 0;
    uint32_t lastInteractionElapsedMs = 0;
};

struct GameStateView {
    modules::SideToMove sideToMove = modules::SideToMove::White;
    bool cpuThinking = false;
    bool hintThinking = false;
    bool inCheck = false;
    bool inCheckmate = false;
    bool inDraw = false;
    bool inStalemate = false;
    bool threatenedKingSquareValid = false;
    uint8_t threatenedKingSquare = 255;
    uint64_t checkingPiecesMask = 0;
    bool checkStartValid = false;
    uint32_t checkStartMs = 0;
    bool checkmateStartValid = false;
    uint32_t checkmateStartMs = 0;
    bool drawStartValid = false;
    uint32_t drawStartMs = 0;
    modules::DrawReason drawReason = modules::DrawReason::None;

    bool clockEnabled = false;
    bool clockRunning = false;
    bool clockPaused = false;
    ClockSide clockActiveSide = ClockSide::White;
    uint32_t clockWhiteRemainingMs = 0;
    uint32_t clockBlackRemainingMs = 0;
    modules::GameWinner clockWinner = modules::GameWinner::None;
    uint32_t clockTerminalStartMs = 0U;

    modules::GameVariant variant = modules::GameVariant::Classic;
    uint8_t duckSquare = modules::INVALID_DUCK_SQUARE;
    uint8_t initialDuckCandidateSquare = modules::INVALID_DUCK_SQUARE;
    bool initialVariantValidationActive = false;
    SetupLifecycle setupLifecycle = SetupLifecycle::Gameplay;
    modules::DuckTurnPhase duckTurnPhase = modules::DuckTurnPhase::ChessMove;
    modules::VariantOutcome variantOutcome{};
    uint32_t variantTerminalStartMs = 0U;
    modules::UiOwner kingResignationOwner = modules::UiOwner::None;
    uint8_t kingResignationSquare = 255U;
    uint32_t kingResignationElapsedMs = 0U;
    bool kingResignationArmed = false;
};

struct RenderStateView {
    modules::UiOwner thinkingControlOwner = modules::UiOwner::None;
    uint16_t thinkingControlScale256 = 0U;
    uint32_t thinkingControlElapsedMs = 0U;
    bool thinkingControlCpu = false;
    modules::UiOwner hintBrewingOwner = modules::UiOwner::None;
    uint16_t hintBrewingScale256 = 0U;
    uint32_t hintBrewingElapsedMs = 0U;
    bool manualHintAcknowledgmentValid = false;
    bool turnPresentationValid = false;
    uint16_t turnScale256 = 0U;
    uint32_t turnElapsedMs = 0U;
    bool hintDisplayRequested = false;
    bool hintFeedbackPending = false;
    bool hintFeedbackTimingValid = false;
    uint32_t hintFeedbackElapsedMs = 0U;
    bool hintMoveValid = false;
    uint8_t hintMoveFromSquare = 255;
    uint8_t hintMoveToSquare = 255;
    uint8_t hintCaptureSquare = CHESS_INTERACTION_INVALID_SQUARE;
    bool hintRefinementActive = false;
    uint32_t hintRefinementElapsedMs = 0U;
    uint32_t hintRefinementDurationMs = 0U;
    uint8_t incomingHintFromSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t incomingHintToSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t incomingHintCaptureSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t incomingHintDuckFromSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t incomingHintDuckToSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t previousHintFromSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t previousHintToSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t previousHintCaptureSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t previousHintDuckFromSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t previousHintDuckToSquare = CHESS_INTERACTION_INVALID_SQUARE;
    bool hintDuckMoveValid = false;
    uint8_t hintDuckMoveFromSquare = 255;
    uint8_t hintDuckMoveToSquare = 255;
    bool allPiecesFlashActive = false;
    bool allPiecesFlashOn = false;
    bool initialKingPlacementPromptActive = false;
    uint64_t initialKingPlacementPromptMask = 0ULL;
    uint32_t initialKingPlacementPromptElapsedMs = 0U;
    fxe::ElapsedTime legalDestinationReveal;
    bool settleAfterglowActive = false;
    uint8_t settleAfterglowSquare = 255;
    uint32_t settleAfterglowElapsedMs = 0;
    bool settleAfterglowAbort = false;
    uint8_t settleAnimationVariant = 0;
    uint8_t settleSquare = 255;
    uint8_t promotionArrivalSquare = 255U;
    bool settleOn = false;
    bool settleTimingValid = false;
    uint32_t settleElapsedMs = 0;
    uint32_t settleDurationMs = 0;
    uint32_t settleVisualElapsedMs = 0;
    uint32_t settlePresentationDurationMs = 0;
    bool humanMoveSettlingActive = false;
    bool cpuMoveSettlingActive = false;
    bool settleMoverSideKnown = false;
    bool settleMoverIsBlack = false;
    MoveRenderTransition moveTransition{};
    bool whiteLastMoveValid = false;
    uint8_t whiteLastMoveFromSquare = 255;
    uint8_t whiteLastMoveToSquare = 255;
    bool blackLastMoveValid = false;
    uint8_t blackLastMoveFromSquare = 255;
    uint8_t blackLastMoveToSquare = 255;
    bool lastMoveMostRecentKnown = false;
    modules::PersistedSide lastMoveMostRecentSide = modules::PersistedSide::Unknown;
    PromotionConfirmation promotionConfirmation;
    bool textOverlayActive = false;
    bool menuOverlayActive = false;
    bool quickSettingsOverlayActive = false;
    bool bootFadeActive = false;
    bool startupPresentationActive = false;
};

struct SystemStateView {
    uint32_t nowMs = 0;
    bool safeModeActive = false;
    bool hardwareTestActive = false;
    bool amptestActive = false;
    bool otaInProgress = false;
};

struct InteractionModelSnapshot {
    PhysicalBoardState board;
    InteractionStateView interaction;
    GameStateView game;
    RenderStateView render;
    SystemStateView system;
};

inline bool IsHumanPromotionActive(const InteractionStateView& interaction) {
    const PhysicalTransitionPlan& plan = interaction.taggedState.plan;
    return interaction.taggedState.tag == ChessInteractionPhase::AwaitingAction &&
           plan.kind == PhysicalTransitionKind::Promotion &&
           plan.source == PhysicalTransitionSource::Human;
}

inline bool HasActivePhysicalPlan(const InteractionStateView& interaction) {
    const ChessInteractionPhase phase = interaction.taggedState.tag;
    const bool planPhase = phase == ChessInteractionPhase::ExecutingPlan ||
                           phase == ChessInteractionPhase::AwaitingAction ||
                           phase == ChessInteractionPhase::Settling;
    return planPhase &&
           interaction.taggedState.plan.kind != PhysicalTransitionKind::None &&
           interaction.taggedState.plan.relocationCount != 0U;
}

inline bool IsCpuMovePromptActive(const InteractionStateView& interaction) {
    return interaction.taggedState.tag == ChessInteractionPhase::ExecutingPlan &&
           IsCpuDirectedTransition(interaction.taggedState.plan);
}

inline bool IsHumanPickupGuidanceActive(const InteractionStateView& interaction) {
    if (interaction.taggedState.tag == ChessInteractionPhase::PieceLifted) {
        return true;
    }
    return interaction.taggedState.tag == ChessInteractionPhase::ExecutingPlan &&
           interaction.taggedState.plan.source == PhysicalTransitionSource::Human &&
           interaction.taggedState.requiredAction ==
               RequiredPhysicalAction::PlacePrimary;
}

inline bool IsPendingHumanEnPassantRemoval(
    const InteractionStateView& interaction) {
    const TaggedInteractionState& state = interaction.taggedState;
    const PhysicalTransitionPlan& plan = state.plan;
    return state.tag == ChessInteractionPhase::ExecutingPlan &&
           plan.source == PhysicalTransitionSource::Human &&
           plan.kind == PhysicalTransitionKind::EnPassant &&
           plan.removalRequired && !plan.progress.removalComplete &&
           (plan.progress.relocationsPlacedMask & 0x01U) != 0U;
}

inline bool IsSettlingActive(const InteractionModelSnapshot& snapshot) {
    const TaggedInteractionState& state = snapshot.interaction.taggedState;
    return state.tag == ChessInteractionPhase::Settling ||
           snapshot.render.humanMoveSettlingActive ||
           snapshot.render.cpuMoveSettlingActive ||
           IsPendingHumanEnPassantRemoval(snapshot.interaction);
}

inline bool IsIdleInteraction(const InteractionModelSnapshot& snapshot) {
    return snapshot.interaction.taggedState.tag == ChessInteractionPhase::Idle &&
           !snapshot.render.cpuMoveSettlingActive;
}

inline bool IsCancelUndoTransitionActive(const InteractionModelSnapshot& snapshot) {
    return snapshot.render.moveTransition.kind ==
           MoveRenderTransitionKind::CancelUndo;
}

} // namespace runtime
