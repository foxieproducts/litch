#pragma once

#include <litch_max/chess_castling_patterns.hpp>
#include "runtime/chess_interaction_model.hpp"

namespace runtime {

struct CastlingPickupIntent {
    uint64_t kingDestinationMask = 0ULL;
    uint64_t rookSourceMask = 0ULL;

    bool IsActive() const { return kingDestinationMask != 0ULL; }
};

inline bool IsCastlingPatternEligible(
    const ChessPositionState& position,
    const litch_max::CastlingPattern& pattern) {
    const uint64_t kingSource = 1ULL << pattern.kingFrom;
    const uint64_t rookSource = 1ULL << pattern.rookFrom;
    const uint64_t sourcePair = kingSource | rookSource;
    const bool sideModelAvailable =
        (position.moverOccupiedMask |
         position.opponentOccupiedMask) != 0ULL;
    const bool pairBelongsToMover =
        !sideModelAvailable ||
        (position.moverOccupiedMask & sourcePair) == sourcePair;
    const bool engineAllowsCastle =
        !position.legalCastlingTargetsValid ||
        (position.legalCastlingKingTargetsMask &
         (1ULL << pattern.kingTo)) != 0ULL;
    return (position.occupiedMask & sourcePair) == sourcePair &&
           (position.kingMask & kingSource) != 0ULL &&
           (position.rookMask & rookSource) != 0ULL &&
           pairBelongsToMover && engineAllowsCastle;
}

inline CastlingPickupIntent BuildCastlingPickupIntent(
    const ChessPositionState& position,
    const uint8_t selectedSquare,
    const uint64_t validDestinationMask) {
    CastlingPickupIntent out;
    for (const litch_max::CastlingPattern& pattern : litch_max::CHESS_CASTLING_PATTERNS) {
        const uint64_t kingDestination = 1ULL << pattern.kingTo;
        const bool destinationConfirmed = position.legalCastlingTargetsValid
                                              ? (position.legalCastlingKingTargetsMask &
                                                 kingDestination) != 0ULL
                                              : (validDestinationMask &
                                                 kingDestination) != 0ULL;
        if (selectedSquare != pattern.kingFrom ||
            !destinationConfirmed ||
            !IsCastlingPatternEligible(position, pattern)) {
            continue;
        }
        out.kingDestinationMask |= kingDestination;
        out.rookSourceMask |= 1ULL << pattern.rookFrom;
    }
    return out;
}

} // namespace runtime
