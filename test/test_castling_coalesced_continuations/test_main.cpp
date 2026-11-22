#include <cstdio>

#include <litch_max/chess_castling_patterns.hpp>
#include "runtime/game_state_machine.hpp"

namespace {

int gFailures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++gFailures;
    }
}

runtime::GameStateMachine MakeMachine(
    const litch_max::CastlingPattern& castle) {
    runtime::GameStateMachine machine;
    runtime::ChessPositionState position;
    position.occupiedMask = (1ULL << castle.kingFrom) |
                            (1ULL << castle.rookFrom);
    position.kingMask = 1ULL << castle.kingFrom;
    position.rookMask = 1ULL << castle.rookFrom;
    position.moverOccupiedMask = position.occupiedMask;
    machine.DebugSetExpectedPositionModel(position);
    machine.DebugSetPhysicalOccupancyMask(position.occupiedMask);
    return machine;
}

void TestCoalescedContinuation(const litch_max::CastlingPattern& castle,
                               const bool placeKingToo) {
    runtime::GameStateMachine machine = MakeMachine(castle);
    const uint64_t before = machine.GetExpectedOccupancyMask();
    const uint64_t kingFrom = 1ULL << castle.kingFrom;
    const uint64_t rookFrom = 1ULL << castle.rookFrom;
    const uint64_t kingTo = 1ULL << castle.kingTo;
    const uint64_t rookTo = 1ULL << castle.rookTo;
    const uint64_t kingLifted = before & ~kingFrom;
    machine.HandleBoardObservation(kingFrom, kingLifted, 1U, 1U);
    machine.SetValidDestMask(kingTo);

    const uint64_t changed = rookFrom | rookTo |
                             (placeKingToo ? kingTo : 0ULL);
    const uint64_t occupied =
        (kingLifted & ~rookFrom) | rookTo |
        (placeKingToo ? kingTo : 0ULL);
    machine.HandleBoardObservation(changed, occupied, 2U, 2U);

    const runtime::TaggedInteractionState& phase =
        machine.GetChessInteractionModel().phase;
    Check(!machine.HasHumanIllegal(),
          "coalesced castle continuation avoids invalid feedback");
    Check(phase.plan.kind == runtime::PhysicalTransitionKind::Castling,
          "coalesced castle continuation builds a castling plan");
    Check(phase.tag == (placeKingToo
                            ? runtime::ChessInteractionPhase::AwaitingAction
                            : runtime::ChessInteractionPhase::ExecutingPlan),
          "coalesced castle continuation reaches the expected phase");
    if (!placeKingToo) {
        Check(phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary,
              "coalesced rook relocation prompts for the king");
    }
}

void TestPreclaimDestinationRelift(
    const litch_max::CastlingPattern& castle,
    const uint8_t reliftSquare) {
    runtime::GameStateMachine machine = MakeMachine(castle);
    const uint64_t sources = (1ULL << castle.kingFrom) |
                             (1ULL << castle.rookFrom);
    const uint64_t destinations = (1ULL << castle.kingTo) |
                                  (1ULL << castle.rookTo);
    const uint64_t lifted = machine.GetExpectedOccupancyMask() & ~sources;
    const uint64_t complete = lifted | destinations;
    machine.HandleBoardObservation(sources, lifted, 1U, 1U);
    machine.HandleBoardObservation(destinations, complete, 2U, 2U);
    Check(machine.HasCommittedMovePending(),
          "completed castle starts with a pending action");
    Check(machine.GetMovedSquaresMask() != 0ULL,
          "completed castle stages moved-square presentation");

    const uint64_t reliftMask = 1ULL << reliftSquare;
    machine.HandleBoardObservation(
        reliftMask, complete & ~reliftMask, 3U, 3U);
    Check(machine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::ExecutingPlan,
          "either castle destination relift reopens the composite plan");
    Check(!machine.HasCommittedMovePending(),
          "castle destination relift invalidates the pending token");
    Check(machine.GetMovedSquaresMask() == 0ULL,
          "castle destination relift restores pre-action moved squares");
    machine.HandleBoardObservation(reliftMask, complete, 4U, 4U);
    runtime::PhysicalTransitionPlan action;
    Check(machine.TryClaimPendingInteractionMove(action),
          "replacement exposes one fresh castle action");
    Check(!machine.TryClaimPendingInteractionMove(action),
          "fresh castle action is exposed exactly once");
}

void TestClaimedCastleReliftDefersToActionResult(
    const litch_max::CastlingPattern& castle) {
    runtime::GameStateMachine machine = MakeMachine(castle);
    const uint64_t sources = (1ULL << castle.kingFrom) |
                             (1ULL << castle.rookFrom);
    const uint64_t destinations = (1ULL << castle.kingTo) |
                                  (1ULL << castle.rookTo);
    const uint64_t lifted = machine.GetExpectedOccupancyMask() & ~sources;
    const uint64_t complete = lifted | destinations;
    machine.HandleBoardObservation(sources, lifted, 1U, 1U);
    machine.HandleBoardObservation(destinations, complete, 2U, 2U);
    runtime::PhysicalTransitionPlan action;
    Check(machine.TryClaimPendingInteractionMove(action),
          "castle action can be claimed before the boundary test");
    const uint32_t token =
        machine.GetChessInteractionModel().phase.pendingActionToken;

    const uint64_t destination = 1ULL << castle.kingTo;
    machine.HandleBoardObservation(
        destination, complete & ~destination, 3U, 3U);
    Check(machine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "claimed castle relift keeps the in-flight action valid");
    const runtime::ChessPositionState projected =
        runtime::ProjectChessPositionAfterPlan(
            machine.GetChessInteractionModel().acknowledged, action);
    machine.HandleInteractionActionResult(token, true);
    machine.HandleInteractionPositionAcknowledged(
        projected.occupiedMask,
        projected.kingMask,
        projected.rookMask,
        projected.moverOccupiedMask,
        projected.opponentOccupiedMask,
        projected.pawnMask,
        projected.enPassantTargetSquare,
        projected.enPassantCaptureSquare,
        projected.sideToMoveValid,
        projected.sideToMoveBlack,
        false,
        0ULL);
    Check(machine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction,
          "claimed castle relift replays only after acknowledgement");
}

void TestKingFirstCastleAbort(const litch_max::CastlingPattern& castle) {
    runtime::GameStateMachine machine = MakeMachine(castle);
    const uint64_t before = machine.GetExpectedOccupancyMask();
    const uint64_t king = 1ULL << castle.kingFrom;
    const uint64_t rook = 1ULL << castle.rookFrom;
    machine.HandleBoardObservation(king, before & ~king, 1U, 1U);
    machine.HandleBoardObservation(rook, 0ULL, 2U, 2U);
    Check(machine.GetChessInteractionModel().phase.plan.kind ==
              runtime::PhysicalTransitionKind::Castling,
          "king-first abort fixture creates a castle plan");
    machine.HandleBoardObservation(king | rook, before, 3U, 3U);
    Check(machine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle,
          "restoring both castle sources clears the composite plan");
}

void TestRookPromptReturnsToBothAirborne(
    const litch_max::CastlingPattern& castle,
    const bool rookPlacedFirst) {
    runtime::GameStateMachine machine = MakeMachine(castle);
    const uint64_t before = machine.GetExpectedOccupancyMask();
    const uint64_t kingFrom = 1ULL << castle.kingFrom;
    const uint64_t kingTo = 1ULL << castle.kingTo;
    const uint64_t rookFrom = 1ULL << castle.rookFrom;
    const uint64_t rookTo = 1ULL << castle.rookTo;
    machine.HandleBoardObservation(
        kingFrom | kingTo, (before & ~kingFrom) | kingTo, 1U, 1U);
    machine.HandleBoardObservation(
        kingTo | rookFrom, 0ULL, 2U, 2U);
    Check(!machine.HasHumanIllegal() &&
              machine.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary,
          "rook-prompt scan can return king and rook to both airborne");

    const uint64_t first = rookPlacedFirst ? rookTo : kingTo;
    const uint64_t second = rookPlacedFirst ? kingTo : rookTo;
    machine.HandleBoardObservation(first, first, 3U, 3U);
    machine.HandleBoardObservation(second, first | second, 4U, 4U);
    Check(!machine.HasHumanIllegal() &&
              machine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::AwaitingAction,
          "both-airborne castle completes in either placement order");
}

void TestBothAirborneCastleRejectsThirdPiece(
    const litch_max::CastlingPattern& castle) {
    runtime::GameStateMachine machine = MakeMachine(castle);
    runtime::ChessPositionState position =
        machine.GetChessInteractionModel().acknowledged;
    constexpr uint8_t THIRD = 40U;
    const uint64_t third = 1ULL << THIRD;
    position.occupiedMask |= third;
    position.moverOccupiedMask |= third;
    machine.DebugSetExpectedPositionModel(position);
    machine.DebugSetPhysicalOccupancyMask(position.occupiedMask);
    const uint64_t sources = (1ULL << castle.kingFrom) |
                             (1ULL << castle.rookFrom);
    machine.HandleBoardObservation(
        sources, position.occupiedMask & ~sources, 1U, 1U);
    machine.HandleBoardObservation(third, 0ULL, 2U, 2U);
    Check(machine.HasHumanIllegal() &&
              machine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan,
          "both-airborne castle rejects a third-piece pickup");
    machine.HandleBoardObservation(third, third, 3U, 3U);
    Check(!machine.HasHumanIllegal() &&
              machine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan,
          "corrected third-piece pickup resumes both-airborne castle");
}

void TestMixedCastleCompletionRecoversAfterThirdPiece(
    const litch_max::CastlingPattern& castle) {
    runtime::GameStateMachine machine = MakeMachine(castle);
    runtime::ChessPositionState position =
        machine.GetChessInteractionModel().acknowledged;
    constexpr uint8_t THIRD = 40U;
    const uint64_t third = 1ULL << THIRD;
    position.occupiedMask |= third;
    position.moverOccupiedMask |= third;
    machine.DebugSetExpectedPositionModel(position);
    machine.DebugSetPhysicalOccupancyMask(position.occupiedMask);

    const uint64_t sources = (1ULL << castle.kingFrom) |
                             (1ULL << castle.rookFrom);
    const uint64_t destinations = (1ULL << castle.kingTo) |
                                  (1ULL << castle.rookTo);
    machine.HandleBoardObservation(
        sources, position.occupiedMask & ~sources, 1U, 1U);
    machine.HandleBoardObservation(
        destinations | third, destinations, 2U, 2U);
    Check(machine.HasHumanIllegal() &&
              !machine.HasCommittedMovePending(),
          "mixed castle completion defers while the third piece is absent");
    machine.HandleBoardObservation(
        third, destinations | third, 3U, 3U);
    Check(!machine.HasHumanIllegal() &&
              machine.HasCommittedMovePending(),
          "correcting the third piece reconciles completed castle progress");
    runtime::PhysicalTransitionPlan action;
    Check(machine.TryClaimPendingInteractionMove(action) &&
              !machine.TryClaimPendingInteractionMove(action),
          "reconciled castle completion exposes exactly one action");
}

void TestMixedPreclaimReliftInvalidatesCastleAction(
    const litch_max::CastlingPattern& castle) {
    runtime::GameStateMachine machine = MakeMachine(castle);
    runtime::ChessPositionState position =
        machine.GetChessInteractionModel().acknowledged;
    constexpr uint8_t THIRD = 40U;
    const uint64_t third = 1ULL << THIRD;
    position.occupiedMask |= third;
    position.moverOccupiedMask |= third;
    machine.DebugSetExpectedPositionModel(position);
    machine.DebugSetPhysicalOccupancyMask(position.occupiedMask);

    const uint64_t sources = (1ULL << castle.kingFrom) |
                             (1ULL << castle.rookFrom);
    const uint64_t destinations = (1ULL << castle.kingTo) |
                                  (1ULL << castle.rookTo);
    const uint64_t complete =
        (position.occupiedMask & ~sources) | destinations;
    machine.HandleBoardObservation(
        sources, position.occupiedMask & ~sources, 1U, 1U);
    machine.HandleBoardObservation(destinations, complete, 2U, 2U);
    Check(machine.HasCommittedMovePending(),
          "completed castle exposes an unclaimed action");

    const uint64_t mixedRelift = (1ULL << castle.kingTo) | third;
    machine.HandleBoardObservation(
        mixedRelift, complete & ~mixedRelift, 3U, 3U);
    runtime::PhysicalTransitionPlan action;
    Check(!machine.TryClaimPendingInteractionMove(action),
          "mixed destination relift invalidates the unclaimed castle action");
    Check(machine.GetMovedSquaresMask() == 0ULL,
          "mixed preclaim disturbance restores pre-action moved squares");
    Check(machine.GetChessInteractionModel().acknowledged.occupiedMask ==
              position.occupiedMask,
          "mixed preclaim disturbance leaves acknowledged chess state unchanged");
}

void TestCorrectionFollowsUnrelatedPieceAcrossSquares(
    const litch_max::CastlingPattern& castle) {
    runtime::GameStateMachine machine = MakeMachine(castle);
    runtime::ChessPositionState position =
        machine.GetChessInteractionModel().acknowledged;
    constexpr uint8_t THIRD = 40U;
    constexpr uint8_t FOURTH = 41U;
    const uint64_t third = 1ULL << THIRD;
    const uint64_t fourth = 1ULL << FOURTH;
    position.occupiedMask |= third | fourth;
    position.moverOccupiedMask |= third | fourth;
    machine.DebugSetExpectedPositionModel(position);
    machine.DebugSetPhysicalOccupancyMask(position.occupiedMask);

    const uint64_t sources = (1ULL << castle.kingFrom) |
                             (1ULL << castle.rookFrom);
    const uint64_t lifted = position.occupiedMask & ~sources;
    machine.HandleBoardObservation(sources, lifted, 1U, 1U);
    machine.HandleBoardObservation(third, lifted & ~third, 2U, 2U);
    Check(machine.HasHumanIllegal(),
          "unrelated castle-phase pickup starts local correction");

    const uint64_t movedUnrelated = (lifted | third) & ~fourth;
    machine.HandleBoardObservation(
        third | fourth, movedUnrelated, 3U, 3U);
    Check(machine.HasHumanIllegal(),
          "correction follows an unrelated piece moved to another bad state");
    Check(!machine.HasCommittedMovePending(),
          "moving unrelated pieces cannot expose a castle action");
}

void TestIllegalCoalescedContinuationDoesNotBypassLegalMask(
    const litch_max::CastlingPattern& castle) {
    runtime::GameStateMachine machine = MakeMachine(castle);
    runtime::ChessPositionState position =
        machine.GetChessInteractionModel().acknowledged;
    position.legalCastlingTargetsValid = true;
    position.legalCastlingKingTargetsMask = 0ULL;
    machine.DebugSetExpectedPositionModel(position);
    machine.DebugSetPhysicalOccupancyMask(position.occupiedMask);

    const uint64_t kingFrom = 1ULL << castle.kingFrom;
    const uint64_t rookFrom = 1ULL << castle.rookFrom;
    const uint64_t rookTo = 1ULL << castle.rookTo;
    const uint64_t kingLifted = position.occupiedMask & ~kingFrom;
    machine.HandleBoardObservation(kingFrom, kingLifted, 1U, 1U);
    machine.SetValidDestMask(0ULL);
    machine.HandleBoardObservation(
        rookFrom | rookTo,
        (kingLifted & ~rookFrom) | rookTo,
        2U,
        2U);

    const runtime::TaggedInteractionState& phase =
        machine.GetChessInteractionModel().phase;
    Check(phase.tag == runtime::ChessInteractionPhase::PieceLifted &&
              phase.selectedSquare == castle.kingFrom &&
              phase.plan.kind == runtime::PhysicalTransitionKind::None,
          "illegal coalesced continuation preserves the original king intent");
    Check(machine.HasHumanIllegal(),
          "illegal coalesced continuation cannot bypass destination feedback");
}

void TestWildCorrectionsPreserveAbortAndCompletion(
    const litch_max::CastlingPattern& castle) {
    runtime::GameStateMachine machine = MakeMachine(castle);
    runtime::ChessPositionState position =
        machine.GetChessInteractionModel().acknowledged;
    constexpr uint8_t THIRD = 40U;
    constexpr uint8_t WRONG_A = 41U;
    constexpr uint8_t WRONG_B = 42U;
    const uint64_t third = 1ULL << THIRD;
    const uint64_t wrongA = 1ULL << WRONG_A;
    const uint64_t wrongB = 1ULL << WRONG_B;
    position.occupiedMask |= third;
    position.moverOccupiedMask |= third;
    machine.DebugSetExpectedPositionModel(position);
    machine.DebugSetPhysicalOccupancyMask(position.occupiedMask);
    const uint64_t kingFrom = 1ULL << castle.kingFrom;
    const uint64_t rookFrom = 1ULL << castle.rookFrom;
    const uint64_t kingTo = 1ULL << castle.kingTo;
    const uint64_t rookTo = 1ULL << castle.rookTo;
    const uint64_t sources = kingFrom | rookFrom;
    uint32_t sequence = 1U;
    const auto Observe = [&](const uint64_t changed,
                             const uint64_t occupied) {
        machine.HandleBoardObservation(
            changed, occupied, sequence, sequence);
        ++sequence;
    };
    uint64_t physical = position.occupiedMask & ~sources;
    Observe(sources, physical);
    Check(machine.GetChessInteractionModel().phase.plan.kind ==
              runtime::PhysicalTransitionKind::Castling,
          "wild correction starts a castle plan");

    // Add an extra, then observably substitute an expected piece elsewhere.
    physical |= wrongA;
    Observe(wrongA, physical);
    Check(machine.HasHumanIllegal() &&
              !machine.HasCommittedMovePending(),
          "wild extra placement pauses castle with local correction");

    physical = (physical & ~third) | wrongB;
    Observe(third | wrongB, physical);
    Check(machine.HasHumanIllegal() &&
              machine.GetChessInteractionModel().acknowledged.occupiedMask ==
                  position.occupiedMask,
          "observable substitution cannot mutate acknowledged chess state");

    // Correct in another order; resume only after every mismatch is gone.
    physical &= ~wrongA;
    Observe(wrongA, physical);
    Check(machine.HasHumanIllegal(),
          "partial arbitrary correction keeps remaining guidance active");
    physical = (physical & ~wrongB) | third;
    Observe(wrongB | third, physical);
    Check(!machine.HasHumanIllegal() &&
              machine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::ExecutingPlan,
          "full arbitrary correction resumes the castle plan");

    // Restore both sources in reverse order to abandon only this attempt.
    physical |= rookFrom;
    Observe(rookFrom, physical);
    physical |= kingFrom;
    Observe(kingFrom, physical);
    Check(machine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle &&
              !machine.HasHumanIllegal() &&
              machine.GetChessInteractionModel().acknowledged.occupiedMask ==
                  position.occupiedMask,
          "reverse source restoration calmly abandons only the castle attempt");

    // Restart rook-first, correct a wrong placement, then finish rook-first.
    physical &= ~rookFrom;
    Observe(rookFrom, physical);
    physical &= ~kingFrom;
    Observe(kingFrom, physical);
    Check(machine.GetChessInteractionModel().phase.plan.kind ==
              runtime::PhysicalTransitionKind::Castling,
          "rook-first restart re-enters the same castle plan");

    physical |= wrongA;
    Observe(wrongA, physical);
    physical &= ~wrongA;
    Observe(wrongA, physical);
    Check(!machine.HasHumanIllegal(),
          "wrong destination correction returns to positive castle guidance");

    physical |= rookTo;
    Observe(rookTo, physical);
    physical |= kingTo;
    Observe(kingTo, physical);
    Check(machine.HasCommittedMovePending(),
          "arbitrary recovery still permits one legal castle completion");

    // Repeated preclaim bounces must revoke each stale action token.
    physical &= ~kingTo;
    Observe(kingTo, physical);
    Check(!machine.HasCommittedMovePending(),
          "king destination relift revokes stale castle action");
    physical |= kingTo;
    Observe(kingTo, physical);
    physical &= ~rookTo;
    Observe(rookTo, physical);
    Check(!machine.HasCommittedMovePending(),
          "rook destination relift also revokes stale castle action");
    physical |= rookTo;
    Observe(rookTo, physical);

    runtime::PhysicalTransitionPlan action;
    Check(machine.TryClaimPendingInteractionMove(action) &&
              action.kind == runtime::PhysicalTransitionKind::Castling,
          "final corrected castle exposes its composite action");
    Check(!machine.TryClaimPendingInteractionMove(action),
          "wild correction trace exposes the final castle exactly once");
    Check(machine.GetChessInteractionModel().acknowledged.occupiedMask ==
              position.occupiedMask,
          "claim boundary still waits for engine acknowledgement");
}

} // namespace

int main() {
    std::puts("[CASTLING_COALESCED_CONTINUATIONS] start");
    for (const litch_max::CastlingPattern& castle :
         litch_max::CHESS_CASTLING_PATTERNS) {
        TestCoalescedContinuation(castle, false);
        TestCoalescedContinuation(castle, true);
        TestPreclaimDestinationRelift(castle, castle.kingTo);
        TestPreclaimDestinationRelift(castle, castle.rookTo);
        TestClaimedCastleReliftDefersToActionResult(castle);
        TestKingFirstCastleAbort(castle);
        TestRookPromptReturnsToBothAirborne(castle, false);
        TestRookPromptReturnsToBothAirborne(castle, true);
        TestBothAirborneCastleRejectsThirdPiece(castle);
        TestMixedCastleCompletionRecoversAfterThirdPiece(castle);
        TestMixedPreclaimReliftInvalidatesCastleAction(castle);
        TestCorrectionFollowsUnrelatedPieceAcrossSquares(castle);
        TestIllegalCoalescedContinuationDoesNotBypassLegalMask(castle);
        TestWildCorrectionsPreserveAbortAndCompletion(castle);
    }
    if (gFailures == 0) {
        std::puts("[CASTLING_COALESCED_CONTINUATIONS] ok");
        return 0;
    }
    std::printf("[CASTLING_COALESCED_CONTINUATIONS] failures=%d\n",
                gFailures);
    return 1;
}
