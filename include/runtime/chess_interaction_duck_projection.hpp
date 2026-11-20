#pragma once

#include "runtime/chess_interaction_model.hpp"

namespace runtime {

// A latched Duck is the sole expected occupied square outside the typed chess
// position. Incomplete or untyped positions cannot establish that identity.
inline uint64_t GetKnownNeutralDuckMask(const ChessPositionState& position) {
    if (!position.sideToMoveValid ||
        (position.moverOccupiedMask | position.opponentOccupiedMask) == 0ULL) {
        return 0ULL;
    }
    const uint64_t neutral = position.occupiedMask &
        ~(position.moverOccupiedMask | position.opponentOccupiedMask |
          position.kingMask | position.rookMask | position.pawnMask);
    return neutral != 0ULL && (neutral & (neutral - 1ULL)) == 0ULL
               ? neutral : 0ULL;
}

inline bool IsEarlyDuckLiftAllowed(const TaggedInteractionState& phase) {
    if (phase.tag == ChessInteractionPhase::PieceLifted) {
        return phase.plan.kind == PhysicalTransitionKind::None &&
               phase.selectedSquare < 64U;
    }
    return (phase.tag == ChessInteractionPhase::ExecutingPlan ||
            phase.tag == ChessInteractionPhase::Settling ||
            phase.tag == ChessInteractionPhase::AwaitingAction) &&
           phase.plan.source == PhysicalTransitionSource::Human &&
           phase.plan.kind != PhysicalTransitionKind::None &&
           phase.plan.kind != PhysicalTransitionKind::DuckRelocation;
}

inline uint64_t GetDuckLandingRemovalMask(
    const uint64_t duckMask,
    const TaggedInteractionState& phase,
    const uint64_t expected,
    const uint64_t physical,
    const uint64_t illegal) {
    if (phase.tag != ChessInteractionPhase::PieceLifted ||
        phase.selectedSquare >= 64U || duckMask == 0ULL ||
        (physical & duckMask) != 0ULL) return 0ULL;
    const uint64_t remaining = expected & ~(duckMask | (1ULL << phase.selectedSquare));
    const uint64_t landings = physical & ~remaining;
    const uint64_t additional = landings & (landings - 1ULL);
    return (remaining & ~physical) == 0ULL && landings == illegal &&
                   (additional & (additional - 1ULL)) == 0ULL
               ? landings : 0ULL;
}

inline uint64_t ProjectHumanChessOccupancy(
    const uint64_t duckMask,
    const TaggedInteractionState& phase,
    const uint64_t physical) {
    if (!IsEarlyDuckLiftAllowed(phase)) return physical;
    const uint64_t lifted = physical | duckMask;
    if (duckMask == 0ULL || (physical & duckMask) != 0ULL ||
        phase.plan.kind == PhysicalTransitionKind::None) return lifted;
    const uint64_t missingChess = phase.plan.afterOccupiedMask & ~lifted;
    const uint64_t extra = lifted & ~phase.plan.afterOccupiedMask;
    if (phase.plan.removalRequired && phase.plan.removalSquare < 64U &&
        (physical & (1ULL << phase.plan.removalSquare)) != 0ULL &&
        (phase.plan.removalSquare != phase.plan.moveTo ||
         !phase.plan.progress.removalComplete)) {
        return lifted;
    }
    // Once all chess choreography is complete, one extra square has only the
    // remaining Duck role. Incomplete capture/castling work never uses it.
    return missingChess == 0ULL && extra != 0ULL &&
                   (extra & (extra - 1ULL)) == 0ULL
               ? lifted & ~extra : lifted;
}

inline uint64_t ProjectHumanChessOccupancy(
    const ChessPositionState& position,
    const TaggedInteractionState& phase,
    const uint64_t physical) {
    return ProjectHumanChessOccupancy(GetKnownNeutralDuckMask(position), phase, physical);
}

inline uint64_t GetCoalescedDuckLandings(
    const ChessInteractionModel& model,
    const ChessInteractionEvent& event) {
    if (model.phase.tag != ChessInteractionPhase::PieceLifted ||
        model.phase.selectedSquare >= 64U) return 0ULL;
    const uint64_t duckMask = GetKnownNeutralDuckMask(model.acknowledged);
    const uint64_t sourceMask = 1ULL << model.phase.selectedSquare;
    if (duckMask == 0ULL ||
        (sourceMask & model.acknowledged.moverOccupiedMask) == 0ULL ||
        (event.occupiedMask & duckMask) != 0ULL) return 0ULL;
    const uint64_t remainingChess = model.acknowledged.occupiedMask &
        ~(duckMask | sourceMask);
    const uint64_t landings = event.occupiedMask & ~remainingChess;
    const uint64_t secondLanding = landings & (landings - 1ULL);
    if ((remainingChess & ~event.occupiedMask) != 0ULL ||
        secondLanding == 0ULL ||
        (secondLanding & (secondLanding - 1ULL)) != 0ULL ||
        (event.changedMask & event.occupiedMask) != landings ||
        (event.changedMask & ~event.occupiedMask &
         ~(duckMask | sourceMask)) != 0ULL) return 0ULL;
    return landings;
}

// Only the chess choreography sees this projection. model.board always keeps
// the real atomic Hall observation, including the already-held neutral piece.
inline ChessInteractionEvent ProjectHumanChessObservation(
    const ChessInteractionModel& model,
    const ChessInteractionEvent& event) {
    ChessInteractionEvent projected = event;
    if (IsEarlyDuckLiftAllowed(model.phase)) {
        const uint64_t duckMask = GetKnownNeutralDuckMask(model.acknowledged);
        projected.occupiedMask = ProjectHumanChessOccupancy(
            model.acknowledged, model.phase, event.occupiedMask);
        projected.changedMask &= ~duckMask;
        if (event.deferredDuckDestinationSquare < 64U &&
            (GetCoalescedDuckLandings(model, event) &
             (1ULL << event.deferredDuckDestinationSquare)) != 0ULL) {
            projected.occupiedMask &=
                ~(1ULL << event.deferredDuckDestinationSquare);
        }
        projected.changedMask &=
            ~(event.occupiedMask & ~projected.occupiedMask);
    }
    return projected;
}

} // namespace runtime
