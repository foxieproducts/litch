#include "runtime/chess_interaction_model.hpp"

namespace runtime {

const char* ChessInteractionPhaseToString(const ChessInteractionPhase phase) {
    switch (phase) {
        case ChessInteractionPhase::Idle: return "Idle";
        case ChessInteractionPhase::PieceLifted: return "PieceLifted";
        case ChessInteractionPhase::CaptureStaged: return "CaptureStaged";
        case ChessInteractionPhase::ExecutingPlan: return "ExecutingPlan";
        case ChessInteractionPhase::AwaitingAction: return "AwaitingAction";
        case ChessInteractionPhase::Settling: return "Settling";
        case ChessInteractionPhase::DuckRelocation: return "DuckRelocation";
        case ChessInteractionPhase::Correction: return "Correction";
    }
    return "?";
}

const char* PhysicalTransitionKindToString(const PhysicalTransitionKind kind) {
    switch (kind) {
        case PhysicalTransitionKind::None: return "None";
        case PhysicalTransitionKind::QuietMove: return "QuietMove";
        case PhysicalTransitionKind::Capture: return "Capture";
        case PhysicalTransitionKind::Castling: return "Castling";
        case PhysicalTransitionKind::EnPassant: return "EnPassant";
        case PhysicalTransitionKind::Promotion: return "Promotion";
        case PhysicalTransitionKind::Undo: return "Undo";
        case PhysicalTransitionKind::DuckRelocation: return "DuckRelocation";
    }
    return "?";
}

const char* RequiredPhysicalActionToString(const RequiredPhysicalAction action) {
    switch (action) {
        case RequiredPhysicalAction::None: return "None";
        case RequiredPhysicalAction::LiftPrimary: return "LiftPrimary";
        case RequiredPhysicalAction::LiftSecondary: return "LiftSecondary";
        case RequiredPhysicalAction::RemoveCapture: return "RemoveCapture";
        case RequiredPhysicalAction::PlacePrimary: return "PlacePrimary";
        case RequiredPhysicalAction::PlaceSecondary: return "PlaceSecondary";
        case RequiredPhysicalAction::RestoreCapture: return "RestoreCapture";
        case RequiredPhysicalAction::AwaitAcknowledgement: return "AwaitAcknowledgement";
        case RequiredPhysicalAction::CorrectBoard: return "CorrectBoard";
    }
    return "?";
}

const char* InteractionInvariantStatusCodeToString(
    const InteractionInvariantStatusCode code) {
    switch (code) {
        case InteractionInvariantStatusCode::None: return "None";
        case InteractionInvariantStatusCode::InvariantViolation: return "InvariantViolation";
    }
    return "?";
}

} // namespace runtime
