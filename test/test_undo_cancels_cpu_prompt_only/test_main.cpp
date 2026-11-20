#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"

#include "runtime/game_state_machine.hpp"

int main() {
    fxe::test_support::TestResults results("UNDO_DURING_CPU_PROMPT_STARTS_UNDO");
    results.Start();

    {
        fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(1000);

        runtime::GameStateMachineConfig cfg;
        cfg.placementStableRequiredMs = 0;
        cfg.cpuIdleDelayMs = 0;
        cfg.settleDurationMs = 20;

        runtime::GameStateMachine sm(cfg);
        sm.Reset();

        // Seed a simple expected/physical occupancy model:
        // One piece on square 0.
        const uint64_t initialOcc = (1ULL << 0);
        sm.SetExpectedOccupancyMask(initialOcc);
        sm.SetExpectedMoverAndOpponentOccupancyMasks(initialOcc, 0);

#ifndef ARDUINO
        sm.DebugSetPhysicalOccupancyMask(initialOcc);
#endif

        // Finalize a CPU move 0 -> 1 to ensure undo history is non-empty.
        results.Check(sm.QueueCpuMove(0, 1, false, 0, '\0'), "queue cpu move 0->1");
        results.Check(sm.HasQueuedCpuMove(), "cpu move queued");
        results.Check((sm.HasQueuedCpuMove() && !sm.IsMoveSettlingActive()), "entered CpuMoving");

        const uint64_t afterOcc = (1ULL << 1);
        sm.HandleBoardObservation(initialOcc | afterOcc,
                                  afterOcc,
                                  1000U,
                                  1U);

        sm.Tick();
        results.Check(!sm.HasQueuedCpuMove(), "cpu move finalized");
        results.Check(sm.IsMoveSettlingActive(), "cpu move enters Settling");
        fxe::ManualMillis::SetNowMs(1000 + 25);
        sm.Tick();
        results.Check(sm.GetChessInteractionModel().phase.tag ==
                          runtime::ChessInteractionPhase::AwaitingAction,
                      "cpu settle exposes its action");
        const uint32_t actionToken =
            sm.GetChessInteractionModel().phase.pendingActionToken;
        sm.HandleInteractionActionResult(actionToken, true);

        // Queue another CPU move prompt. Pressing Undo should cancel the prompt
        // and immediately start undoing the last committed move.
        sm.SetExpectedOccupancyMask(afterOcc);
#ifndef ARDUINO
        sm.DebugSetPhysicalOccupancyMask(afterOcc);
#endif
        results.Check(sm.QueueCpuMove(1, 2, false, 0, '\0'), "queue cpu move 1->2");
        results.Check(sm.HasQueuedCpuMove(), "cpu move queued (second)");

        // Mimic the firmware policy: in VS-CPU, a tap requests 2 plies, but
        // while the CPU move is still only a prompt, undo should only revert
        // the last committed ply.
        sm.CancelQueuedCpuMove();
        results.Check(sm.BeginUndoPrompt(
                          0U, 1U, modules::PersistedSide::Unknown,
                          '\0', '\0', 0U),
                      "explicit coordinator-style undo starts");
        results.Check(!sm.HasQueuedCpuMove(), "cpu prompt canceled");
        results.Check(sm.IsUndoPromptActive(), "undo prompt started");
        results.Check(sm.GetChessInteractionModel().phase.tag ==
                          runtime::ChessInteractionPhase::AwaitingAction,
                      "undo prompt owns an AwaitingAction phase");
    }

    return results.Finish();
}
