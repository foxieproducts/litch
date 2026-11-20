#pragma once

#include <cstdint>

namespace modules {

constexpr uint8_t INVALID_DUCK_SQUARE = 255U;

enum class GameVariant : uint8_t {
    Undecided = 0,
    Classic = 1,
    Duck = 2,
};

enum class DuckTurnPhase : uint8_t {
    ChessMove = 0,
    DuckRelocation = 1,
};

enum class GameWinner : uint8_t {
    None = 0,
    White = 1,
    Black = 2,
};

enum class VariantOutcomeReason : uint8_t {
    None = 0,
    KingCaptured = 1,
    Fowled = 2,
    Resigned = 3,
};

struct VariantOutcome {
    VariantOutcomeReason reason = VariantOutcomeReason::None;
    GameWinner winner = GameWinner::None;

    constexpr bool IsTerminal() const {
        return reason != VariantOutcomeReason::None &&
               winner != GameWinner::None;
    }
};

constexpr bool IsValidGameVariant(const GameVariant variant) {
    return variant == GameVariant::Undecided ||
           variant == GameVariant::Classic ||
           variant == GameVariant::Duck;
}

constexpr bool IsValidDuckSquare(const uint8_t square) {
    return square < 64U;
}

constexpr bool IsValidVariantOutcome(const VariantOutcome outcome) {
    if (outcome.reason == VariantOutcomeReason::None) {
        return outcome.winner == GameWinner::None;
    }
    return (outcome.reason == VariantOutcomeReason::KingCaptured ||
            outcome.reason == VariantOutcomeReason::Fowled ||
            outcome.reason == VariantOutcomeReason::Resigned) &&
           (outcome.winner == GameWinner::White ||
            outcome.winner == GameWinner::Black);
}

const char* GameVariantToString(GameVariant variant);
const char* DuckTurnPhaseToString(DuckTurnPhase phase);
const char* GameWinnerToString(GameWinner winner);
const char* VariantOutcomeReasonToString(VariantOutcomeReason reason);

} // namespace modules
