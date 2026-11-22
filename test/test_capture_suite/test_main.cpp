#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[CAPTURE_SUITE] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    runtime::ManualClock clock(0);
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 100;
    cfg.cpuIdleDelayMs = 999999;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    // Target-first capture: remove the captured piece while Idle, then lift the
    // mover piece, then place onto the target square.
    sm.Reset();
    sm.SetExpectedOccupancyMask((1ULL << 12) | (1ULL << 13));
    sm.SetExpectedMoverAndOpponentOccupancyMasks((1ULL << 12),
                                                 (1ULL << 13));
    sm.SetLegalCaptureTargetsMask(1ULL << 13, true);
    sm.DebugSetPhysicalOccupancyMask((1ULL << 12) | (1ULL << 13));

    sm.HandleSensorChange(13, false);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::CaptureStaged,
          "target-first: target removal enters CaptureStaged");
    Check(sm.HasCaptureFirstStaging(), "target-first: stages capture-first while idle");

    // Cancellation: returning the staged target while idle should not mark illegal.
    sm.HandleSensorChange(13, true);
    Check(!sm.HasHumanIllegal(), "target-first: returning target while idle is not illegal");
    Check(!sm.HasCaptureFirstStaging(), "target-first: staging clears when target returned");

    // Stage again and complete the capture.
    sm.HandleSensorChange(13, false);
    sm.HandleSensorChange(12, false);
    Check(sm.IsHumanPieceInHand(), "target-first: mover pickup enters PickedUp");
    Check(sm.GetChessInteractionModel().phase.plan.moveFrom == 12,
          "target-first: plan source is mover square");

    sm.SetValidDestMask(1ULL << 13);
    sm.HandleSensorChange(13, true);
    Check(sm.IsMoveSettlingActive(), "target-first: placement settles");

    clock.SetNowMs(200);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "target-first: settle exposes its action");

    runtime::PhysicalTransitionPlan committed;
    const bool got = sm.TryClaimPendingInteractionMove(committed);
    Check(got, "target-first: reducer action exposed");
    if (got) {
        Check(committed.moveFrom == 12, "target-first: committed from");
        Check(committed.moveTo == 13, "target-first: committed to");
    }
    Check(sm.GetExpectedOccupancyMask() == ((1ULL << 12) | (1ULL << 13)),
          "human finalize leaves acknowledged expected occupancy unchanged until commit is consumed upstream");

    // Configure a complete position so the reducer can classify mover and
    // opponent pieces without relying on legacy occupancy heuristics.
    sm.Reset();
    sm.SetExpectedOccupancyMask((1ULL << 12) | (1ULL << 13));
    sm.SetExpectedMoverAndOpponentOccupancyMasks((1ULL << 12),
                                                 (1ULL << 13));
    sm.DebugSetPhysicalOccupancyMask((1ULL << 12) | (1ULL << 13));

    // Start pickup.
    sm.HandleSensorChange(12, false);
    Check(sm.IsHumanPieceInHand(), "Idle->PickedUp");

    // Removing the opponent after the mover is lifted creates the capture plan
    // directly; target-first staging is reserved for the opposite order.
    sm.HandleSensorChange(13, false);
    const runtime::PhysicalTransitionPlan& capturePlan =
        sm.GetChessInteractionModel().phase.plan;
    Check(!sm.HasCaptureFirstStaging(),
          "mover-first capture does not create target-first staging");
    Check(capturePlan.removalRequired && capturePlan.removalSquare == 13,
          "mover-first capture records removal in reducer plan");
    Check(!sm.HasCaptureReturn(), "capture plan does not set capture-return");

    // Completing the capture by placing on that square proceeds to settle.
    sm.SetValidDestMask(1ULL << 13);
    sm.HandleSensorChange(13, true);
    Check(sm.IsMoveSettlingActive(), "capture-first placement settles");

    // Reset and test capture-return + corrective placement invariant.
    sm.Reset();
    sm.SetExpectedOccupancyMask((1ULL << 12) | (1ULL << 14));
    sm.SetExpectedMoverAndOpponentOccupancyMasks(
        (1ULL << 12) | (1ULL << 14), 0ULL);
    sm.DebugSetPhysicalOccupancyMask((1ULL << 12) | (1ULL << 14));

    sm.HandleSensorChange(12, false);
    Check(sm.IsHumanPieceInHand(), "Idle->PickedUp (phase 2)");

    // Capture-return: remove a second mover-side piece while one is in hand.
    sm.HandleSensorChange(14, false);
    Check(sm.HasCaptureReturn(), "capture-return latched on unexpected removal");

    // While capture-return is active, placing on a valid destination must NOT proceed.
    sm.SetValidDestMask((1ULL << 14) | (1ULL << 20));
    sm.HandleSensorChange(20, true);
    Check(sm.IsHumanPieceInHand(),
          "capture-return blocks completing a move");
    Check(sm.HasHumanIllegal(),
          "capture-return latches a destination placed during correction");

    // Returning the removed piece clears that prompt, but the premature
    // destination placement remains a local correction. Hall sensors cannot
    // replay a placement that happened while the modal owned the board.
    sm.HandleSensorChange(14, true);
    Check(!sm.HasCaptureReturn(), "capture-return clears on correction");
    Check(sm.IsHumanPieceInHand(),
          "corrective placement does not start settling");
    Check(sm.HasHumanIllegal(),
          "premature destination remains correction until physically removed");

    sm.HandleSensorChange(20, false);
    Check(!sm.HasHumanIllegal(),
          "removing premature destination clears local correction");

    // Now completing the move should settle.
    sm.HandleSensorChange(20, true);
    Check(sm.IsMoveSettlingActive(),
          "move completes after correction");

    clock.SetNowMs(400);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "settle exposes its action");

    // Target-first en-passant shape: captured pawn removed first (capture square != destination).
    // This should not block settling because the capture square is already clear.
    sm.Reset();
    runtime::ChessPositionState enPassantPosition;
    enPassantPosition.occupiedMask = (1ULL << 28) | (1ULL << 27);
    enPassantPosition.pawnMask = enPassantPosition.occupiedMask;
    enPassantPosition.moverOccupiedMask = 1ULL << 28;
    enPassantPosition.opponentOccupiedMask = 1ULL << 27;
    enPassantPosition.enPassantTargetSquare = 19U;
    enPassantPosition.enPassantCaptureSquare = 27U;
    enPassantPosition.sideToMoveValid = true;
    enPassantPosition.sideToMoveBlack = false;
    sm.DebugSetExpectedPositionModel(enPassantPosition);
    sm.DebugSetPhysicalOccupancyMask(enPassantPosition.occupiedMask);

    sm.HandleSensorChange(27, false); // remove captured d5 pawn first
    sm.HandleSensorChange(28, false); // lift White e5 pawn
    sm.SetValidDestMask(1ULL << 19);  // diagonal d6 destination
    sm.HandleSensorChange(19, true);
    Check(sm.IsMoveSettlingActive(), "target-first ep: settles");

    clock.SetNowMs(600);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "target-first ep: settle exposes its action without blocking");

    if (g_failures == 0) {
        std::puts("[CAPTURE_SUITE] ok");
        return 0;
    }

    std::printf("[CAPTURE_SUITE] failures=%d\n", g_failures);
    return 1;
}
