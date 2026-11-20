#include <cstdio>

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
    std::puts("[MOVE_ACCEPTANCE] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    runtime::ManualClock clock(0);
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 100;
    cfg.cpuIdleDelayMs = 999999;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    // Configure expected occupancy so we can distinguish capture-first staging
    // from capture-return prompting in this test.
    sm.SetExpectedOccupancyMask((1ULL << 12) | (1ULL << 13));
    sm.SetExpectedMoverAndOpponentOccupancyMasks(1ULL << 12,
                                                 1ULL << 13);

    // Single-piece-in-hand: ignore second removal while PickedUp.
    sm.HandleSensorChange(12, false);
    Check(sm.IsHumanPieceInHand(), "Idle->PickedUp");

    // Removing the opponent after the mover is lifted creates a capture plan
    // directly (allowed; no correction prompt).
    sm.HandleSensorChange(13, false);
    Check(sm.IsHumanPieceInHand(), "second removal keeps PickedUp");
    Check(!sm.HasCaptureReturn(), "capture plan does not latch capture-return");
    const runtime::PhysicalTransitionPlan& capturePlan =
        sm.GetChessInteractionModel().phase.plan;
    Check(capturePlan.removalRequired && capturePlan.removalSquare == 13,
          "second removal enters reducer capture plan");

    // Completing the capture: placing onto that square proceeds to settling.
    sm.HandleSensorChange(13, true);
    Check(sm.IsMoveSettlingActive(), "capture-first place settles");

    // Finalize quickly.
    clock.SetNowMs(200);
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "settle exposes its action");

    // Restart for the remaining tests.
    sm.Reset();
    sm.SetExpectedOccupancyMask((1ULL << 12) | (1ULL << 14));
    sm.SetExpectedMoverAndOpponentOccupancyMasks(
        (1ULL << 12) | (1ULL << 14), 0ULL);
    sm.DebugSetPhysicalOccupancyMask((1ULL << 12) | (1ULL << 14));

    // A second mover-side lift while one piece is already in hand triggers
    // capture-return guidance.
    sm.HandleSensorChange(12, false);
    Check(sm.IsHumanPieceInHand(), "Idle->PickedUp (second phase)");
    sm.HandleSensorChange(14, false);
    Check(sm.HasCaptureReturn(), "second removal latches capture-return");

    // A placement on an unrelated square while capture-return owns input is
    // retained as a local correction rather than silently discarded.
    sm.HandleSensorChange(13, true);
    Check(sm.HasHumanIllegal(),
          "wrong capture-return placement is retained as correction");
    sm.HandleSensorChange(13, false);
    Check(!sm.HasHumanIllegal(),
          "removing wrong capture-return placement clears correction");

    // Returning the prompted piece clears capture-return.
    sm.HandleSensorChange(14, true);
    Check(!sm.HasCaptureReturn(), "capture-return clears when corrected");

    // Cancel move preview: return the piece to the origin.
    sm.HandleSensorChange(12, true);
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "putback cancels to Idle");

    // Placing a piece while idle is illegal and blocks pickup.
    sm.HandleSensorChange(50, true);
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "idle placed keeps Idle");
    Check(sm.HasHumanIllegal(), "idle placed latches illegal");
    sm.HandleSensorChange(51, false);
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "pickup ignored while illegal active");

    // Clear illegal by correcting the illegal square.
    sm.HandleSensorChange(50, false);
    Check(!sm.HasHumanIllegal(), "illegal cleared by removing illegal piece");

    // Restart a normal move and settle. Include a next-side piece so its pickup
    // can be replayed after the first plan is acknowledged.
    sm.Reset();
    sm.HandleInteractionPositionAcknowledged(
        (1ULL << 12) | (1ULL << 30), 0ULL, 0ULL,
        1ULL << 12, 1ULL << 30);
    sm.ApplyPhysicalOccupancySnapshot((1ULL << 12) | (1ULL << 30));
    sm.HandleSensorChange(12, false);
    Check(sm.IsHumanPieceInHand(), "Idle->PickedUp (restart)");

    // Start settling.
    sm.HandleSensorChange(20, true);
    Check(sm.IsMoveSettlingActive(), "PickedUp->Settling");

    // If another interaction happens during settle, finalize early and treat it as
    // a new move start.
    clock.SetNowMs(10);
    sm.HandleSensorChange(30, false);
    Check(sm.IsHumanPieceInHand(),
          "settle early finalize + reprocess removal");

    // Start another move and abort settle by removing its destination square.
    sm.Reset();
    sm.HandleInteractionPositionAcknowledged(1ULL << 12, 0ULL, 0ULL,
                                             1ULL << 12);
    sm.ApplyPhysicalOccupancySnapshot(1ULL << 12);
    sm.HandleSensorChange(12, false);
    sm.HandleSensorChange(40, true);
    Check(sm.IsMoveSettlingActive(),
          "PickedUp->Settling (second)");
    sm.HandleSensorChange(40, false);
    Check(sm.IsHumanPieceInHand(),
          "removing dest aborts settle");

    // CPU correction: unrelated occupancy is immediately invalid. Placement
    // tolerance starts only after the prompted mover is physically lifted.
    sm.Reset();
    clock.SetNowMs(0);
    sm.HandleInteractionPositionAcknowledged(1ULL << 12U, 0ULL, 0ULL,
                                             1ULL << 12U);
    sm.ApplyPhysicalOccupancySnapshot(1ULL << 12U);
    Check(sm.QueueCpuMove(12U, 28U, false, 0U, '\0'),
          "queue cpu prompt for tentative correction");
    sm.HandleSensorChange(5, true);
    Check(sm.HasCpuIllegal() && !sm.HasCpuTentativeCorrection(),
          "CpuMoving unrelated placement is immediately invalid");
    sm.HandleSensorChange(5, false);
    Check(!sm.HasCpuIllegal(),
          "CpuMoving unrelated placement clears on reversal");
    sm.HandleSensorChange(12, false);
    sm.HandleSensorChange(5, true);
    Check(!sm.HasCpuIllegal() && sm.HasCpuTentativeCorrection(),
          "CpuMoving off-target mover placement is tentative");
    clock.SetNowMs(5000);
    sm.Tick();
    Check(!sm.HasCpuIllegal() && sm.HasCpuTentativeCorrection(),
          "CpuMoving mover-placement tolerance has no timeout");
    sm.HandleSensorChange(5, false);
    Check(!sm.HasCpuIllegal() && !sm.HasCpuTentativeCorrection(),
          "CpuMoving mover-placement tolerance clears on reversal");

    // A capture target remains occupied until the captured piece is removed.
    // The lifted CPU mover still owns one tolerant off-target placement.
    runtime::GameStateMachine captureSm(cfg);
    captureSm.Reset();
    const uint64_t captureBefore = (1ULL << 12U) | (1ULL << 28U);
    captureSm.HandleInteractionPositionAcknowledged(
        captureBefore, 0ULL, 0ULL, 1ULL << 12U, 1ULL << 28U);
    captureSm.ApplyPhysicalOccupancySnapshot(captureBefore);
    Check(captureSm.QueueCpuMove(12U, 28U, true, 28U, '\0'),
          "queue CPU capture placement fixture");
    captureSm.HandleSensorChange(12U, false);
    captureSm.HandleSensorChange(5U, true);
    Check(!captureSm.HasCpuIllegal() &&
              captureSm.GetCpuTentativeCorrectionMask() == (1ULL << 5U),
          "off-target CPU capture mover placement remains tentative");

    if (g_failures == 0) {
        std::puts("[MOVE_ACCEPTANCE] ok");
        return 0;
    }

    std::printf("[MOVE_ACCEPTANCE] failures=%d\n", g_failures);
    return 1;
}
