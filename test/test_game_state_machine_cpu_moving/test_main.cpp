#include <cstdio>

#include "foxie_embedded/millis.hpp"

#include "runtime/app_event.hpp"
#include "runtime/app_event_dispatch.hpp"
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

void Tick(runtime::ManualClock& clock, runtime::GameStateMachine& sm) {
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    runtime::DispatchAppEventToStateMachine(sm, runtime::AppEvent::MakeTick(clock.NowMs()));
}

void Sensor(runtime::ManualClock& clock, runtime::GameStateMachine& sm, const uint8_t square, const bool occ) {
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    runtime::DispatchAppEventToStateMachine(sm, runtime::AppEvent::MakeSensorDelta(clock.NowMs(), square, occ));
}

} // namespace

int main() {
    std::puts("[GAME_STATE_MACHINE_CPU_MOVING] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    runtime::ManualClock clock;

    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 20;
    cfg.settleFlashMs = 10;
    cfg.cpuIdleDelayMs = 100000;
    cfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    // Engine/expected setup:
    // - mover-side pieces at 12 (prompted) and 7 (other)
    // - opponent-side piece at 55
    const uint64_t moverOcc = (1ULL << 12) | (1ULL << 7);
    const uint64_t oppOcc = (1ULL << 55);
    const uint64_t occ = moverOcc | oppOcc;
    sm.SetExpectedOccupancyMask(occ);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(moverOcc, oppOcc);
    sm.DebugSetPhysicalOccupancyMask(occ);

    Check(sm.QueueCpuMove(12, 28, false, 0, '\0'), "queue cpu move succeeds");
    Check((sm.HasQueuedCpuMove() && !sm.IsMoveSettlingActive()), "queued cpu move enters CpuMoving");
    Check(sm.GetChessInteractionModel().phase.plan.source ==
              runtime::PhysicalTransitionSource::Cpu,
          "queued CPU prompt is owned by common plan");

    // Any unrelated lift is immediately invalid during a CPU prompt.
    Sensor(clock, sm, 7, false);
    Check(sm.GetCpuIllegalMask() == (1ULL << 7U) &&
              !sm.HasCpuTentativeCorrection(),
          "wrong mover-piece lift is immediately invalid");
    Sensor(clock, sm, 7, true);
    Check(!sm.HasCpuIllegal(),
          "reversing wrong mover-piece lift clears invalid state");

    // The same immediate rejection applies to an opponent piece.
    Sensor(clock, sm, 55, false);
    Check(sm.GetCpuIllegalMask() == (1ULL << 55U) &&
              !sm.HasCpuTentativeCorrection(),
          "wrong opponent-piece lift is immediately invalid");
    Sensor(clock, sm, 55, true);
    Check(!sm.HasCpuIllegal(),
          "restoring wrong opponent-piece lift clears invalid state");

    // Perform the physical move.
    Sensor(clock, sm, 12, false);
    Sensor(clock, sm, 28, true);

    // CpuMoving finalizes only after Tick observes stable match to target.
    Tick(clock, sm);
    Check(sm.IsMoveSettlingActive(), "cpu move enters Settling when board matches target");

    // CPU settling happens after physical completion but before the queued
    // engine action is necessarily acknowledged. A destination-only remove /
    // restore pair is treated as transient sensor bounce and does not invent a
    // human placement-cancel transition for the already completed CPU move.
    const uint64_t cpuTargetOcc = (occ & ~(1ULL << 12)) | (1ULL << 28);
    uint32_t sequence = sm.GetLastBoardObservationSequence() + 1U;
    sm.HandleBoardObservation(1ULL << 28,
                              cpuTargetOcc & ~(1ULL << 28),
                              clock.NowMs(),
                              sequence++);
    Check(sm.IsMoveSettlingActive(),
          "cpu destination bounce remains in settle");
    Check(sm.IsCpuMoveSettlingActive(),
          "cpu destination bounce cannot become human placement");
    Check(!sm.HasCpuIllegal(),
          "cpu destination bounce does not create invalid feedback");
    sm.HandleBoardObservation(1ULL << 28,
                              cpuTargetOcc,
                              clock.NowMs(),
                              sequence++);
    Check(sm.IsMoveSettlingActive(),
          "restored cpu destination continues settle animation");

    // Settle completion exposes the reducer action.
    clock.AdvanceMs(50);
    Tick(clock, sm);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "cpu settle exposes its action");

    runtime::PhysicalTransitionPlan mv;
    Check(sm.TryClaimPendingInteractionMove(mv), "cpu move exposes reducer action");
    Check(mv.moveFrom == 12 && mv.moveTo == 28, "reducer action has correct squares");
    Check(sm.GetExpectedOccupancyMask() == occ,
          "cpu finalize leaves acknowledged expected occupancy unchanged until commit is consumed upstream");
    const uint32_t actionToken =
        sm.GetChessInteractionModel().phase.pendingActionToken;
    runtime::DispatchAppEventToStateMachine(
        sm,
        runtime::AppEvent::MakeActionResult(clock.NowMs(), actionToken, true));

    // CPU-illegal markers during CpuMoving are based on the CPU target.
    sm.SetExpectedOccupancyMask(occ);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(moverOcc, oppOcc);
    sm.DebugSetPhysicalOccupancyMask(occ);
    runtime::DispatchAppEventToStateMachine(
        sm,
        runtime::AppEvent::MakePositionAcknowledged(
            clock.NowMs(), occ, 0, 0, moverOcc, oppOcc));
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle,
          "cpu acknowledgement returns reducer to idle");
    Check(sm.QueueCpuMove(12, 28, false, 0, '\0'), "queue cpu move again");

    // An extra placement before the prompted mover is lifted is immediately
    // invalid because it cannot be the prompted CPU piece.
    Sensor(clock, sm, 0, true);
    Check(sm.GetCpuIllegalMask() == (1ULL << 0U) &&
              !sm.HasCpuTentativeCorrection(),
          "extra placement before CPU lift is immediately invalid");
    Sensor(clock, sm, 0, false);
    Check(!sm.HasCpuIllegal(),
          "removing extra placement clears invalid state");

    // Leniency applies only after the prompted mover is in hand. A single
    // off-target placement remains tentative until it is corrected or another
    // contradictory observation occurs.
    Sensor(clock, sm, 12, false);
    Sensor(clock, sm, 0, true);
    Check(!sm.HasCpuIllegal() &&
              sm.GetCpuTentativeCorrectionMask() == (1ULL << 0U),
          "off-target CPU mover placement starts tentative correction");
    sm.HandleBoardObservation(
        0ULL, sm.GetPhysicalOccupancyMask(),
        clock.NowMs(), sm.GetLastBoardObservationSequence() + 1U);
    Check(!sm.HasCpuIllegal() && sm.HasCpuTentativeCorrection(),
          "unchanged board observation cannot escalate mover placement");

    clock.AdvanceMs(5000);
    Tick(clock, sm);
    Check(!sm.HasCpuIllegal() && sm.HasCpuTentativeCorrection(),
          "waiting cannot escalate mover placement");

    Sensor(clock, sm, 0, false);
    Check(!sm.HasCpuIllegal() && !sm.HasCpuTentativeCorrection(),
          "removing off-target CPU mover clears tentative correction");
    Sensor(clock, sm, 28, true);
    Tick(clock, sm);
    Check(sm.IsCpuMoveSettlingActive(),
          "correct CPU mover placement still reaches settle");

    // A fast lift plus off-target placement may arrive in one atomic scan. It
    // is still placement of the prompted mover, not an unrelated pickup.
    {
        runtime::GameStateMachine smAtomic(cfg);
        smAtomic.Reset();
        smAtomic.SetExpectedOccupancyMask(occ);
        smAtomic.SetExpectedMoverAndOpponentOccupancyMasks(moverOcc, oppOcc);
        smAtomic.DebugSetPhysicalOccupancyMask(occ);
        Check(smAtomic.QueueCpuMove(12, 28, false, 0, '\0'),
              "queue atomic CPU placement fixture");
        const uint64_t atomicPhysical = (occ & ~(1ULL << 12U)) | (1ULL << 0U);
        smAtomic.HandleBoardObservation((1ULL << 12U) | (1ULL << 0U),
                                        atomicPhysical, clock.NowMs(), 1U);
        Check(!smAtomic.HasCpuIllegal() &&
                  smAtomic.GetCpuTentativeCorrectionMask() == (1ULL << 0U),
              "atomic off-target CPU mover placement remains tentative");
    }

    // Capture prompt: lifting the capture square (opponent piece) must be allowed.
    {
        runtime::GameStateMachine smCap(cfg);
        smCap.Reset();

        // Expected: mover at 12, opponent at 28 (capture target).
        const uint64_t moverOccCap = (1ULL << 12);
        const uint64_t oppOccCap = (1ULL << 28);
        const uint64_t occCap = moverOccCap | oppOccCap;
        smCap.SetExpectedOccupancyMask(occCap);
        smCap.SetExpectedMoverAndOpponentOccupancyMasks(moverOccCap, oppOccCap);
        smCap.DebugSetPhysicalOccupancyMask(occCap);

        Check(smCap.QueueCpuMove(12, 28, true, 28, '\0'), "queue capture cpu move succeeds");
        Check((smCap.HasQueuedCpuMove() && !smCap.IsMoveSettlingActive()), "capture cpu move enters CpuMoving");

        Sensor(clock, smCap, 28, false);
        Check(!smCap.HasCpuIllegal(), "lifting capture square is allowed");

        // Continue the move so the state machine remains consistent.
        Sensor(clock, smCap, 12, false);
        Sensor(clock, smCap, 28, true);
        Tick(clock, smCap);
        Check(smCap.IsMoveSettlingActive(),
              "capture cpu move enters Settling when board matches target");
        clock.AdvanceMs(50);
        Tick(clock, smCap);
        Check(smCap.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::AwaitingAction,
              "capture cpu settle exposes its action");
    }

    // A different pickup is not bounce: it ends the CPU animation immediately
    // and is replayed as the next side's interaction once the CPU action is
    // acknowledged. The derived piece-in-hand query and reducer must converge.
    {
        runtime::GameStateMachine smNext(cfg);
        smNext.Reset();
        smNext.SetExpectedOccupancyMask(occ);
        smNext.SetExpectedMoverAndOpponentOccupancyMasks(moverOcc, oppOcc);
        smNext.DebugSetPhysicalOccupancyMask(occ);
        Check(smNext.QueueCpuMove(12, 28, false, 0, '\0'),
              "queue cpu move for settle interruption");

        uint64_t physical = occ & ~(1ULL << 12);
        smNext.HandleBoardObservation(1ULL << 12, physical, 1000U, 1U);
        physical |= (1ULL << 28);
        smNext.HandleBoardObservation(1ULL << 28, physical, 1001U, 2U);
        Tick(clock, smNext);
        Check(smNext.IsCpuMoveSettlingActive(),
              "cpu interruption fixture enters settle");

        runtime::PhysicalTransitionPlan pendingCpuMove;
        Check(smNext.TryClaimPendingInteractionMove(pendingCpuMove),
              "stable cpu move exposes action before next interaction");

        physical &= ~(1ULL << 55);
        smNext.HandleBoardObservation(1ULL << 55, physical, 1002U, 3U);
        Check(!smNext.IsCpuMoveSettlingActive(),
              "different pickup immediately ends cpu settle");
        Check(smNext.IsHumanPieceInHand(),
              "different pickup starts next physical interaction");
        Check(smNext.GetChessInteractionModel().board.changedMask ==
                  (1ULL << 55U),
              "next interaction records the new source");

        const uint32_t token =
            smNext.GetChessInteractionModel().phase.pendingActionToken;
        runtime::DispatchAppEventToStateMachine(
            smNext,
            runtime::AppEvent::MakeActionResult(clock.NowMs(), token, true));
        smNext.HandleInteractionPositionAcknowledged(
            cpuTargetOcc, 0ULL, 0ULL, 1ULL << 55U,
            (1ULL << 7U) | (1ULL << 28U), 0ULL,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            true, false, true, 0ULL);
        Check(smNext.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::PieceLifted,
              "cpu acknowledgement replays the next pickup canonically");
        Check(smNext.GetChessInteractionModel().phase.selectedSquare == 55U,
              "canonical next pickup retains its source");
        Check(smNext.IsHumanPieceInHand(),
              "piece-in-hand query agrees after cpu acknowledgement");
    }

    // Destination bounce remains owned by the orthogonal CPU settle window
    // even after the engine action has already been acknowledged. A transient
    // remove/restore pair must not enter a fresh human interaction.
    {
        runtime::GameStateMachine smPostAck(cfg);
        smPostAck.Reset();
        smPostAck.SetExpectedOccupancyMask(occ);
        smPostAck.SetExpectedMoverAndOpponentOccupancyMasks(moverOcc,
                                                           oppOcc);
        smPostAck.DebugSetPhysicalOccupancyMask(occ);
        Check(smPostAck.QueueCpuMove(12, 28, false, 0, '\0'),
              "queue cpu move for post-ack bounce");

        uint64_t physical = occ & ~(1ULL << 12);
        smPostAck.HandleBoardObservation(1ULL << 12, physical,
                                         1050U, 1U);
        physical |= (1ULL << 28);
        smPostAck.HandleBoardObservation(1ULL << 28, physical,
                                         1051U, 2U);
        Tick(clock, smPostAck);

        runtime::PhysicalTransitionPlan pendingCpuMove;
        Check(smPostAck.TryClaimPendingInteractionMove(pendingCpuMove),
              "post-ack bounce fixture exposes cpu action");
        const uint32_t token =
            smPostAck.GetChessInteractionModel().phase.pendingActionToken;
        runtime::DispatchAppEventToStateMachine(
            smPostAck,
            runtime::AppEvent::MakeActionResult(clock.NowMs(), token, true));
        smPostAck.HandleInteractionPositionAcknowledged(
            cpuTargetOcc, 0ULL, 0ULL, 1ULL << 55U,
            (1ULL << 7U) | (1ULL << 28U), 0ULL,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            true, false, true, 1ULL << 28U);
        Check(smPostAck.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle &&
                  smPostAck.IsCpuMoveSettlingActive(),
              "cpu action acknowledgement leaves bounce window active");

        physical &= ~(1ULL << 28);
        smPostAck.HandleBoardObservation(1ULL << 28, physical,
                                         1052U, 3U);
        Check(smPostAck.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle &&
                  smPostAck.IsCpuMoveSettlingActive(),
              "post-ack destination removal is suppressed by settle");
        Check(!smPostAck.HasHumanIllegal() && !smPostAck.HasCpuIllegal(),
              "post-ack destination removal has no invalid feedback");

        physical |= (1ULL << 28);
        smPostAck.HandleBoardObservation(1ULL << 28, physical,
                                         1053U, 4U);
        Check(smPostAck.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle &&
                  smPostAck.IsCpuMoveSettlingActive(),
              "post-ack destination restore remains a transient bounce");

        clock.AdvanceMs(50U);
        Tick(clock, smPostAck);
        Check(!smPostAck.IsCpuMoveSettlingActive() &&
                  smPostAck.GetChessInteractionModel().phase.tag ==
                      runtime::ChessInteractionPhase::Idle,
              "restored post-ack bounce expires without replay");
    }

    // If the destination is still absent when the post-ack settle window ends,
    // replay that suppressed observation exactly once against the acknowledged
    // post-CPU position.
    {
        runtime::GameStateMachine smPostAckAbsent(cfg);
        smPostAckAbsent.Reset();
        smPostAckAbsent.SetExpectedOccupancyMask(occ);
        smPostAckAbsent.SetExpectedMoverAndOpponentOccupancyMasks(moverOcc,
                                                                 oppOcc);
        smPostAckAbsent.DebugSetPhysicalOccupancyMask(occ);
        Check(smPostAckAbsent.QueueCpuMove(12, 28, false, 0, '\0'),
              "queue cpu move for post-ack persistent removal");
        uint64_t physical = occ & ~(1ULL << 12);
        smPostAckAbsent.HandleBoardObservation(1ULL << 12, physical,
                                               1070U, 1U);
        physical |= (1ULL << 28);
        smPostAckAbsent.HandleBoardObservation(1ULL << 28, physical,
                                               1071U, 2U);
        Tick(clock, smPostAckAbsent);
        runtime::PhysicalTransitionPlan pendingCpuMove;
        Check(smPostAckAbsent.TryClaimPendingInteractionMove(pendingCpuMove),
              "persistent post-ack fixture exposes cpu action");
        const uint32_t token =
            smPostAckAbsent.GetChessInteractionModel().phase.pendingActionToken;
        runtime::DispatchAppEventToStateMachine(
            smPostAckAbsent,
            runtime::AppEvent::MakeActionResult(clock.NowMs(), token, true));
        smPostAckAbsent.HandleInteractionPositionAcknowledged(
            cpuTargetOcc, 0ULL, 0ULL, 1ULL << 55U,
            (1ULL << 7U) | (1ULL << 28U), 0ULL,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            true, false, true, 1ULL << 28U);
        physical &= ~(1ULL << 28);
        smPostAckAbsent.HandleBoardObservation(1ULL << 28, physical,
                                               1072U, 3U);
        clock.AdvanceMs(50U);
        Tick(clock, smPostAckAbsent);
        Check(!smPostAckAbsent.IsCpuMoveSettlingActive(),
              "persistent post-ack removal ends settle window");
        Check(smPostAckAbsent.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::CaptureStaged &&
                  smPostAckAbsent.GetChessInteractionModel().phase.selectedSquare ==
                      28U,
              "persistent post-ack removal replays once as target-first");
        Check(!smPostAckAbsent.HasHumanIllegal(),
              "persistent post-ack replay avoids correction feedback");
    }

    // Destination bounce is suppressed only while it remains transient. If it
    // is still lifted when the CPU action is acknowledged, it is replayed as a
    // target-first pickup for the next side instead of being lost forever.
    {
        runtime::GameStateMachine smDestination(cfg);
        smDestination.Reset();
        smDestination.SetExpectedOccupancyMask(occ);
        smDestination.SetExpectedMoverAndOpponentOccupancyMasks(moverOcc,
                                                               oppOcc);
        smDestination.DebugSetPhysicalOccupancyMask(occ);
        Check(smDestination.QueueCpuMove(12, 28, false, 0, '\0'),
              "queue cpu move for persistent destination pickup");

        uint64_t physical = occ & ~(1ULL << 12);
        smDestination.HandleBoardObservation(1ULL << 12, physical,
                                             1100U, 1U);
        physical |= (1ULL << 28);
        smDestination.HandleBoardObservation(1ULL << 28, physical,
                                             1101U, 2U);
        Tick(clock, smDestination);
        runtime::PhysicalTransitionPlan pendingCpuMove;
        Check(smDestination.TryClaimPendingInteractionMove(pendingCpuMove),
              "stable cpu destination exposes action before bounce");
        physical &= ~(1ULL << 28);
        smDestination.HandleBoardObservation(1ULL << 28, physical,
                                             1102U, 3U);
        Check(smDestination.IsCpuMoveSettlingActive(),
              "destination removal is initially treated as cpu bounce");

        const uint32_t token =
            smDestination.GetChessInteractionModel().phase.pendingActionToken;
        runtime::DispatchAppEventToStateMachine(
            smDestination,
            runtime::AppEvent::MakeActionResult(clock.NowMs(), token, true));
        smDestination.HandleInteractionPositionAcknowledged(
            cpuTargetOcc, 0ULL, 0ULL, 1ULL << 55U,
            (1ULL << 7U) | (1ULL << 28U), 0ULL,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            true, false, true, 1ULL << 28U);
        Check(smDestination.IsCpuMoveSettlingActive(),
              "cpu acknowledgement preserves destination bounce ownership");
        Check(smDestination.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle,
              "destination-only bounce is not replayed at acknowledgement");
        physical |= 1ULL << 28U;
        smDestination.HandleBoardObservation(1ULL << 28U, physical,
                                             1103U, 4U);
        Check(smDestination.IsCpuMoveSettlingActive() &&
                  smDestination.GetChessInteractionModel().phase.tag ==
                      runtime::ChessInteractionPhase::Idle,
              "remove-ack-restore remains a transient CPU bounce");
    }

    // Undo during a CPU-prompted move should cancel that prompt and immediately
    // start undo for the last committed move.
    // This must work even if the user has already started physically moving
    // pieces (i.e., the state is no longer CpuMoving).
    {
        runtime::GameStateMachine sm2(cfg);
        sm2.Reset();

        // Queue a CPU move prompt.
        sm2.SetExpectedOccupancyMask(1ULL << 12);
        sm2.DebugSetPhysicalOccupancyMask(1ULL << 12);
        Check(sm2.QueueCpuMove(12, 28, false, 0, '\0'), "queue cpu move for undo test");
        Check(sm2.HasQueuedCpuMove(), "cpu move queued");

        // Simulate that the user already lifted the prompted piece.
        sm2.HandleSensorChange(12U, false);

        sm2.CancelQueuedCpuMove();
        Check(sm2.BeginUndoPrompt(52U, 44U,
                                  modules::PersistedSide::Unknown,
                                  '\0', '\0', 0U),
              "explicit undo prompt succeeds");
        Check(!sm2.HasQueuedCpuMove(), "undo cancels queued cpu move");
        Check(sm2.IsUndoPromptActive(), "undo starts undo prompt");

        // Coordinator cancellation leaves the reducer through one semantic seam.
        sm2.CancelUndoPrompt(true);
        Check(!sm2.IsUndoPromptActive(), "second undo cancels undo prompt");
    }

    if (g_failures == 0) {
        std::puts("[GAME_STATE_MACHINE_CPU_MOVING] ok");
        return 0;
    }

    std::printf("[GAME_STATE_MACHINE_CPU_MOVING] failures=%d\n", g_failures);
    return 1;
}
