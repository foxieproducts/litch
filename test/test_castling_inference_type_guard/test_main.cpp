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
    std::puts("[CASTLING_INFERENCE_TYPE_GUARD] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    runtime::ManualClock clock(0);
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 100;
    cfg.cpuIdleDelayMs = 999999;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    // Regression: a non-rook moving along a rook castling path (e.g., promoted
    // queen a8->d8) must NOT be treated as a partial castling submove.
    //
    // Repro shape (target-first capture):
    //  1) remove target square while Idle
    //  2) lift mover
    //  3) place mover on target
    //  4) settle finalizes -> committed move must be exported

    // Expected occupancy includes:
    //  - mover piece at a8 (0)
    //  - opponent piece at d8 (3)
    //  - a king at e8 (4) so the type model is considered available
    const uint64_t a8 = (1ULL << 0);
    const uint64_t d8 = (1ULL << 3);
    const uint64_t e8 = (1ULL << 4);

    sm.SetExpectedOccupancyMask(a8 | d8 | e8);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(a8, d8 | e8);
    sm.SetLegalCaptureTargetsMask(d8, true);

    // Provide a piece-type model that explicitly says there is a king on e8,
    // but no rook on a8.
    sm.SetExpectedKingAndRookOccupancyMasks(e8, 0);
    sm.DebugSetPhysicalOccupancyMask(a8 | d8 | e8);

    // Target-first capture staging: remove opponent piece at d8.
    sm.HandleSensorChange(3, false);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::CaptureStaged,
          "target-first: target removal enters CaptureStaged");
    Check(sm.HasCaptureFirstStaging(), "target-first: stages capture-first while idle");

    // Lift mover from a8.
    sm.HandleSensorChange(0, false);
    Check(sm.IsHumanPieceInHand(), "target-first: mover pickup enters PickedUp");
    Check(sm.GetChessInteractionModel().phase.plan.moveFrom == 0,
          "target-first: plan source is a8");

    // Place on d8 and settle.
    sm.SetValidDestMask(d8);
    sm.HandleSensorChange(3, true);
    Check(sm.IsMoveSettlingActive(), "target-first: placement settles");

    clock.SetNowMs(200);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "target-first: settle exposes its action");

    runtime::PhysicalTransitionPlan committed;
    const bool got = sm.TryClaimPendingInteractionMove(committed);
    Check(got, "non-rook a8->d8 exposes reducer action (not castling)");
    if (got) {
        Check(committed.moveFrom == 0, "committed from a8");
        Check(committed.moveTo == 3, "committed to d8");
    }

    // A rook that has already been placed as an ordinary move is not
    // retroactively reinterpreted as castling when the king is lifted next.
    runtime::GameStateMachine rookMove(cfg);
    rookMove.Reset();
    runtime::ChessPositionState castleReady;
    castleReady.occupiedMask = (1ULL << 60U) | (1ULL << 63U);
    castleReady.kingMask = 1ULL << 60U;
    castleReady.rookMask = 1ULL << 63U;
    castleReady.moverOccupiedMask = castleReady.occupiedMask;
    castleReady.legalCastlingTargetsValid = true;
    castleReady.legalCastlingKingTargetsMask = 1ULL << 62U;
    rookMove.DebugSetExpectedPositionModel(castleReady);
    rookMove.DebugSetPhysicalOccupancyMask(castleReady.occupiedMask);
    rookMove.HandleSensorChange(63U, false);
    rookMove.SetValidDestMask(1ULL << 61U);
    rookMove.HandleSensorChange(61U, true);
    Check(rookMove.GetChessInteractionModel().phase.plan.kind ==
              runtime::PhysicalTransitionKind::QuietMove,
          "placed rook is accepted as an ordinary move");
    rookMove.HandleSensorChange(60U, false);
    Check(rookMove.GetChessInteractionModel().phase.plan.kind !=
              runtime::PhysicalTransitionKind::Castling,
          "later king lift does not reinterpret the placed rook move");

    if (g_failures == 0) {
        std::puts("[CASTLING_INFERENCE_TYPE_GUARD] ok");
        return 0;
    }

    std::printf("[CASTLING_INFERENCE_TYPE_GUARD] failures=%d\n", g_failures);
    return 1;
}
