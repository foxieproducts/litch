#include "runtime/game_state_machine.hpp"

#include "runtime/game_state_machine_castling.hpp"

namespace runtime {
namespace {

bool IsActiveCastlingPlan(const TaggedInteractionState& state) {
    return (state.tag == ChessInteractionPhase::ExecutingPlan ||
            state.tag == ChessInteractionPhase::Settling ||
            state.tag == ChessInteractionPhase::AwaitingAction) &&
           state.plan.kind == PhysicalTransitionKind::Castling &&
           state.plan.relocationCount == 2U;
}

uint8_t LastSquareInMask(const uint64_t mask) {
    uint8_t result = CHESS_INTERACTION_INVALID_SQUARE;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((mask & (1ULL << square)) != 0ULL) {
            result = square;
        }
    }
    return result;
}

} // namespace

const PhysicalTransitionPlan* GameStateMachine::GetActiveHumanCastlingPlan() const {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    if ((phase.tag != ChessInteractionPhase::ExecutingPlan &&
         phase.tag != ChessInteractionPhase::AwaitingAction) ||
        phase.plan.kind != PhysicalTransitionKind::Castling ||
        phase.plan.source != PhysicalTransitionSource::Human ||
        phase.plan.relocationCount != 2U) {
        return nullptr;
    }
    return &phase.plan;
}

void GameStateMachine::UpdateCastlingRelocationFeedback(
    const TaggedInteractionState& before,
    const TaggedInteractionState& after) {
    const bool beforeCastlingPlan = IsActiveCastlingPlan(before);
    const uint64_t changedMask = m_chessInteractionModel.board.changedMask;
    const uint64_t occupiedMask = m_chessInteractionModel.board.occupiedMask;

    if (beforeCastlingPlan && m_renderState.settleAfterglowActive) {
        for (uint8_t i = 0U; i < before.plan.relocationCount; ++i) {
            const uint8_t destination = before.plan.relocations[i].to;
            if (destination < 64U &&
                destination == m_renderState.settleAfterglowSquare &&
                (changedMask & (1ULL << destination)) != 0ULL &&
                (occupiedMask & (1ULL << destination)) == 0ULL) {
                // Relift/cancel removes only the presentation. The reducer
                // continues to own plan, correction, and cancellation state.
                StartSettleAfterglow(m_renderState,
                                     INVALID_SQUARE,
                                     false,
                                     INVALID_SQUARE);
                break;
            }
        }
    }

    if (!IsActiveCastlingPlan(after)) {
        return;
    }

    const uint8_t beforePlaced =
        beforeCastlingPlan
            ? before.plan.progress.relocationsPlacedMask
            : 0U;
    const uint8_t afterPlaced =
        after.plan.progress.relocationsPlacedMask;
    const uint8_t newlyPlaced =
        static_cast<uint8_t>(afterPlaced & ~beforePlaced & 0x03U);
    const bool compositeComplete = (afterPlaced & 0x03U) == 0x03U;
    uint8_t feedbackSquare = INVALID_SQUARE;
    if (newlyPlaced != 0U && !compositeComplete) {
        for (uint8_t i = 0U; i < after.plan.relocationCount; ++i) {
            if ((newlyPlaced & (1U << i)) != 0U) {
                feedbackSquare = after.plan.relocations[i].to;
                break;
            }
        }
    } else if (newlyPlaced == 0x03U) {
        const uint64_t destinationMask =
            (1ULL << after.plan.relocations[0].to) |
            (1ULL << after.plan.relocations[1].to);
        const uint8_t finalSettleSquare =
            LastSquareInMask(changedMask & occupiedMask & destinationMask);
        feedbackSquare =
            after.plan.relocations[0].to == finalSettleSquare
                ? after.plan.relocations[1].to
                : after.plan.relocations[0].to;
    }
    if (feedbackSquare < 64U) {
        // The first completed relocation gets positive render-only feedback.
        // No timer or action authority is introduced, so the next relocation
        // prompt remains immediate and both presentations may overlap.
        StartSettleAfterglow(m_renderState,
                             feedbackSquare,
                             false,
                             INVALID_SQUARE);
    }
}

void GameStateMachine::CommitCastlingComposite() {
    const PhysicalTransitionPlan* active = GetActiveHumanCastlingPlan();
    if (active != nullptr) {
        (void)TryAcknowledgeInteractionPlan(
            PhysicalTransitionKind::Castling,
            active->moveFrom,
            active->moveTo);
    }
}

} // namespace runtime
