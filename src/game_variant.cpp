#include "modules/game_variant.hpp"

namespace modules {

const char* GameVariantToString(const GameVariant variant) {
    switch (variant) {
        case GameVariant::Undecided: return "Undecided";
        case GameVariant::Classic: return "Classic";
        case GameVariant::Duck: return "Duck";
        default: return "Unknown";
    }
}

const char* DuckTurnPhaseToString(const DuckTurnPhase phase) {
    switch (phase) {
        case DuckTurnPhase::ChessMove: return "ChessMove";
        case DuckTurnPhase::DuckRelocation: return "DuckRelocation";
        default: return "Unknown";
    }
}

const char* GameWinnerToString(const GameWinner winner) {
    switch (winner) {
        case GameWinner::None: return "None";
        case GameWinner::White: return "White";
        case GameWinner::Black: return "Black";
        default: return "Unknown";
    }
}

const char* VariantOutcomeReasonToString(
    const VariantOutcomeReason reason) {
    switch (reason) {
        case VariantOutcomeReason::None: return "None";
        case VariantOutcomeReason::KingCaptured: return "KingCaptured";
        case VariantOutcomeReason::Fowled: return "Fowled";
        case VariantOutcomeReason::Resigned: return "Resigned";
        default: return "Unknown";
    }
}

} // namespace modules
