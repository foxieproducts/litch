#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"

#include "runtime/game_state_machine.hpp"

int main() {
    fxe::test_support::TestResults results("CPU_CASTLING_INFERENCE_TYPE_GUARD");
    results.Start();

    {
        fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(1000);

        runtime::GameStateMachineConfig cfg;
        cfg.placementStableRequiredMs = 0;
        cfg.cpuIdleDelayMs = 0;
        cfg.settleDurationMs = 50;

        runtime::GameStateMachine sm(cfg);
        sm.Reset();

        // Scenario: a rook is on e8 and moves to g8.
        // If castling inference were based only on (from,to) pattern, this would
        // be misinterpreted as black king-side castling.
        // Guard: when the piece-type model says the king is NOT on e8, the CPU
        // move must be treated as a normal single-piece move.
        const uint64_t e8 = (1ULL << 4);
        const uint64_t h8 = (1ULL << 7);
        const uint64_t initialOcc = e8 | h8;

        sm.SetExpectedOccupancyMask(initialOcc);
        sm.SetExpectedMoverAndOpponentOccupancyMasks(initialOcc, 0);

#ifndef ARDUINO
        sm.DebugSetPhysicalOccupancyMask(initialOcc);
#endif

        // Type model: king is not on e8 (it has moved earlier).
        const uint64_t kingOcc = (1ULL << 2); // c8
        const uint64_t rookOcc = initialOcc;  // rooks on e8 and h8
        sm.SetExpectedKingAndRookOccupancyMasks(kingOcc, rookOcc);

        // Queue a CPU move matching the castling king move pattern (e8->g8).
        results.Check(sm.QueueCpuMove(4, 6, false, 0, '\0'), "queue cpu move e8->g8");
        results.Check((sm.HasQueuedCpuMove() && !sm.IsMoveSettlingActive()), "entered CpuMoving");

        // Physical board performs the simple rook move e8->g8 (no rook h8->f8).
        const uint64_t g8 = (1ULL << 6);
        const uint64_t afterOcc = g8 | h8;
        sm.HandleBoardObservation(e8 | g8, afterOcc, 1000U, 1U);

        sm.Tick();
        results.Check(sm.IsMoveSettlingActive(), "cpu move enters Settling after finalize");

        // Let settle elapse.
        fxe::ManualMillis::SetNowMs(1100);
        sm.Tick();
        results.Check(sm.GetChessInteractionModel().phase.tag ==
                          runtime::ChessInteractionPhase::AwaitingAction,
                      "cpu settle exposes its action");

        runtime::PhysicalTransitionPlan mv;
        results.Check(sm.TryClaimPendingInteractionMove(mv), "reducer action exposed");
        results.Check(mv.moveFrom == 4 && mv.moveTo == 6, "action squares e8->g8");
    }

    return results.Finish();
}
