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

void TestCircleConfirmedEnPassant() {
    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    stateMachine.SetMoveConfirmPolicy(2000U, true);
    runtime::ChessPositionState position;
    position.occupiedMask = (1ULL << 28U) | (1ULL << 27U);
    position.moverOccupiedMask = 1ULL << 28U;
    position.opponentOccupiedMask = 1ULL << 27U;
    position.pawnMask = position.occupiedMask;
    position.enPassantTargetSquare = 19U;
    position.enPassantCaptureSquare = 27U;
    stateMachine.DebugSetExpectedPositionModel(position);
    stateMachine.DebugSetPhysicalOccupancyMask(position.occupiedMask);

    // e5xd6: the mover reaches its destination before d5 is removed.
    stateMachine.HandleSensorChange(28U, false);
    stateMachine.SetValidDestMask(1ULL << 19U);
    stateMachine.HandleSensorChange(19U, true);
    stateMachine.HandleSensorChange(27U, false);
    Check(!stateMachine.HasHumanIllegal(),
          "Circle en passant accepts the required captured-pawn removal");
    Check(stateMachine.IsMoveSettlingActive() &&
              !stateMachine.HasCommittedMovePending(),
          "Circle en passant still waits for O after capture removal");
    stateMachine.HandleButtonEvent(
        modules::UserButton::P1O, fxe::Button::Event::Press);
    fxe::ManualMillis::AdvanceMs(1000U);
    stateMachine.Tick();
    Check(stateMachine.HasCommittedMovePending(),
          "Circle en passant exposes its move after O and one second");
}

void TestCircleCorrectionCancellation(const bool moverReturnedFirst) {
    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    stateMachine.SetMoveConfirmPolicy(2000U, true);
    const uint64_t initial = (1ULL << 52U) | (1ULL << 51U);
    stateMachine.SetExpectedOccupancyMask(initial);
    stateMachine.SetExpectedMoverAndOpponentOccupancyMasks(initial, 0ULL);
    stateMachine.DebugSetPhysicalOccupancyMask(initial);
    stateMachine.HandleSensorChange(52U, false);
    stateMachine.SetValidDestMask(1ULL << 36U);
    stateMachine.HandleSensorChange(36U, true);

    // An accidental d2 pickup must not make returning e4 to e2 a move.
    stateMachine.HandleSensorChange(51U, false);
    stateMachine.HandleSensorChange(36U, false);
    stateMachine.HandleSensorChange(moverReturnedFirst ? 52U : 51U, true);
    Check(!stateMachine.HasCommittedMovePending(),
          "Circle correction restoration never confirms the abandoned move");
    stateMachine.HandleSensorChange(moverReturnedFirst ? 51U : 52U, true);
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle &&
              !stateMachine.IsMoveSettlingActive() &&
              !stateMachine.HasCommittedMovePending() &&
              !stateMachine.HasHumanIllegal(),
          "Circle correction returns to Idle in either restoration order");
}

void TestCircleConfirmationDuringCorrection(const bool confirmBeforeError) {
    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    stateMachine.SetMoveConfirmPolicy(2000U, true);
    const uint64_t initial = (1ULL << 52U) | (1ULL << 51U);
    stateMachine.SetExpectedOccupancyMask(initial);
    stateMachine.SetExpectedMoverAndOpponentOccupancyMasks(initial, 0ULL);
    stateMachine.DebugSetPhysicalOccupancyMask(initial);
    stateMachine.HandleSensorChange(52U, false);
    stateMachine.SetValidDestMask(1ULL << 36U);
    stateMachine.HandleSensorChange(36U, true);
    if (confirmBeforeError) {
        stateMachine.HandleButtonEvent(
            modules::UserButton::P1O, fxe::Button::Event::Press);
    }
    stateMachine.HandleSensorChange(51U, false);
    if (!confirmBeforeError) {
        stateMachine.HandleButtonEvent(
            modules::UserButton::P1O, fxe::Button::Event::Press);
    }
    fxe::ManualMillis::AdvanceMs(1000U);
    stateMachine.Tick();
    Check(!stateMachine.HasCommittedMovePending(),
          "Circle cannot commit while the board still needs correction");
    stateMachine.HandleSensorChange(51U, true);
    stateMachine.Tick();
    Check(stateMachine.HasCommittedMovePending() == confirmBeforeError,
          "correction preserves only confirmation accepted on a valid board");
    if (!confirmBeforeError) {
        stateMachine.HandleButtonEvent(
            modules::UserButton::P1O, fxe::Button::Event::Press);
        fxe::ManualMillis::AdvanceMs(1000U);
        stateMachine.Tick();
        Check(stateMachine.HasCommittedMovePending(),
              "a fresh O confirms the corrected pending move");
    }
}

void TestLiveConfirmationPolicyChanges() {
    struct PolicyChange {
        uint32_t fromDurationMs;
        bool fromRequiresO;
        uint32_t toDurationMs;
        bool toRequiresO;
    };
    constexpr PolicyChange CHANGES[] = {
        {2000U, true, 2000U, false},
        {2000U, true, 5000U, false},
        {2000U, false, 2000U, true},
        {5000U, false, 2000U, true},
        {2000U, false, 5000U, false},
        {5000U, false, 2000U, false},
    };
    for (const PolicyChange& change : CHANGES) {
        runtime::GameStateMachine stateMachine;
        stateMachine.Reset();
        stateMachine.SetMoveConfirmPolicy(
            change.fromDurationMs, change.fromRequiresO);
        stateMachine.SetExpectedOccupancyMask(1ULL << 52U);
        stateMachine.SetExpectedMoverAndOpponentOccupancyMasks(1ULL << 52U, 0ULL);
        stateMachine.DebugSetPhysicalOccupancyMask(1ULL << 52U);
        stateMachine.HandleSensorChange(52U, false);
        stateMachine.SetValidDestMask(1ULL << 36U);
        stateMachine.HandleSensorChange(36U, true);
        fxe::ManualMillis::AdvanceMs(750U);
        stateMachine.SetMoveConfirmPolicy(
            change.toDurationMs, change.toRequiresO);
        if (change.toRequiresO) {
            fxe::ManualMillis::AdvanceMs(10000U);
            stateMachine.Tick();
            Check(!stateMachine.HasCommittedMovePending(),
                  "changing timed confirmation to Circle stops auto-confirmation");
            stateMachine.HandleButtonEvent(
                modules::UserButton::P1O, fxe::Button::Event::Press);
            fxe::ManualMillis::AdvanceMs(1000U);
        } else {
            fxe::ManualMillis::AdvanceMs(change.toDurationMs - 1U);
            stateMachine.Tick();
            Check(!stateMachine.HasCommittedMovePending(),
                  "new timed confirmation waits its full interval from the change");
            // Production reapplies the same setting on every sensor cycle.
            stateMachine.SetMoveConfirmPolicy(change.toDurationMs, false);
            fxe::ManualMillis::AdvanceMs(1U);
        }
        stateMachine.Tick();
        Check(stateMachine.HasCommittedMovePending(),
              "changed policy completes at its new timer or Circle boundary");
    }

    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    stateMachine.SetMoveConfirmPolicy(2000U, true);
    stateMachine.SetExpectedOccupancyMask(1ULL << 52U);
    stateMachine.SetExpectedMoverAndOpponentOccupancyMasks(1ULL << 52U, 0ULL);
    stateMachine.DebugSetPhysicalOccupancyMask(1ULL << 52U);
    stateMachine.HandleSensorChange(52U, false);
    stateMachine.SetValidDestMask(1ULL << 36U);
    stateMachine.HandleSensorChange(36U, true);
    stateMachine.HandleButtonEvent(
        modules::UserButton::P1O, fxe::Button::Event::Press);
    fxe::ManualMillis::AdvanceMs(400U);
    stateMachine.SetMoveConfirmPolicy(5000U, false);
    fxe::ManualMillis::AdvanceMs(599U);
    stateMachine.Tick();
    Check(!stateMachine.HasCommittedMovePending(),
          "policy change cannot shorten already accepted Circle confirmation");
    fxe::ManualMillis::AdvanceMs(1U);
    stateMachine.Tick();
    Check(stateMachine.HasCommittedMovePending(),
          "policy change preserves the accepted Circle one-second deadline");
}

void TestCircleCastleSettle(const bool confirmAfterAcknowledgement) {
    constexpr uint64_t BEFORE = (1ULL << 60U) | (1ULL << 63U);
    constexpr uint64_t AFTER = (1ULL << 62U) | (1ULL << 61U);
    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    stateMachine.SetMoveConfirmPolicy(2000U, true);
    stateMachine.HandleInteractionPositionAcknowledged(
        BEFORE, 1ULL << 60U, 1ULL << 63U, BEFORE, 0ULL);
    stateMachine.DebugSetPhysicalOccupancyMask(BEFORE);
    stateMachine.HandleBoardObservation(
        BEFORE, 0ULL, fxe::Millis(),
        stateMachine.GetLastBoardObservationSequence() + 1U);
    stateMachine.HandleBoardObservation(
        AFTER, AFTER, fxe::Millis(),
        stateMachine.GetLastBoardObservationSequence() + 1U);
    runtime::PhysicalTransitionPlan move;
    Check(stateMachine.TryClaimPendingInteractionMove(move),
          "Circle castle still exposes its composite action immediately");
    if (!confirmAfterAcknowledgement) {
        stateMachine.HandleButtonEvent(
            modules::UserButton::P1O, fxe::Button::Event::Press);
    }
    stateMachine.HandleInteractionActionResult(
        stateMachine.GetChessInteractionModel().phase.pendingActionToken, true);
    stateMachine.HandleInteractionPositionAcknowledged(
        AFTER, 1ULL << 62U, 1ULL << 61U, 0ULL, AFTER);
    if (confirmAfterAcknowledgement) {
        stateMachine.HandleButtonEvent(
            modules::UserButton::P1O, fxe::Button::Event::Press);
    }
    fxe::ManualMillis::AdvanceMs(1000U);
    stateMachine.Tick();
    Check(!stateMachine.IsMoveSettlingActive() &&
              !stateMachine.HasCommittedMovePending(),
          "Circle finishes retained castle settle before or after acknowledgement");
}

} // namespace

int main() {
    std::puts("[GAME_STATE_MACHINE_PLACING] start");

    runtime::ManualClock clock(1000);
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(clock.NowMs());

    const auto advanceTime = [&](const uint32_t deltaMs) {
        clock.AdvanceMs(deltaMs);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
    };

    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 50;
    cfg.cpuIdleDelayMs = 999999;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

#ifndef ARDUINO
    TestCircleConfirmedEnPassant();
    TestCircleCorrectionCancellation(false);
    TestCircleCorrectionCancellation(true);
    TestCircleConfirmationDuringCorrection(false);
    TestCircleConfirmationDuringCorrection(true);
    TestLiveConfirmationPolicyChanges();
    TestCircleCastleSettle(false);
    TestCircleCastleSettle(true);
    fxe::ManualMillis::SetNowMs(clock.NowMs());

    // A normal pickup is represented directly by the tagged PieceLifted phase;
    // placement correction is an orthogonal modal and no longer aliases this
    // move-in-hand path.
    sm.SetExpectedOccupancyMask(1ULL << 4);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(1ULL << 4, 0ULL);
    sm.DebugSetPhysicalOccupancyMask(1ULL << 4);
    sm.HandleSensorChange(4, false);
    Check(sm.IsHumanPieceInHand(), "Idle->PickedUp");
    Check(sm.GetChessInteractionModel().phase.selectedSquare == 4,
          "tagged pickup records source");

    // Valid destination should transition to Settling.
    sm.HandleSensorChange(12, true);
    Check(sm.IsMoveSettlingActive(), "PieceLifted->Settling on valid place");
    Check(sm.GetChessInteractionModel().phase.plan.moveFrom == 4,
          "plan preserves source into settle");
    Check(sm.GetChessInteractionModel().phase.plan.moveTo == 12,
          "plan records destination into settle");

    // Settling should complete after settleDurationMs.
    advanceTime(49);
    sm.Tick();
    Check(sm.IsMoveSettlingActive(), "still settling before deadline");

    advanceTime(1);
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "settle exposes its action");

    // Regression: picking up a different piece during settling should
    // finalize immediately (no waiting for settleDurationMs).
    sm.Reset();
    sm.DebugSetPhysicalOccupancyMask((1ULL << 4) | (1ULL << 20));
    sm.SetExpectedMoverAndOpponentOccupancyMasks((1ULL << 4), (1ULL << 20));

    sm.HandleSensorChange(4, false);
    Check(sm.IsHumanPieceInHand(), "Idle->PickedUp (regression setup)");

    sm.SetValidDestMask(1ULL << 12);
    sm.HandleSensorChange(12, true);
    Check(sm.IsMoveSettlingActive(), "PickedUp->Settling (regression setup)");

    sm.HandleSensorChange(20, false);
    Check(sm.IsHumanPieceInHand(),
          "Settling: other pickup finalizes and starts new pickup");
    uint8_t activeSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    Check(sm.TryGetHumanPieceInHandSquare(activeSquare) &&
              activeSquare == 20U,
          "new pickup tracks from square");

    // Regression: during normal settling, picking up elsewhere should end
    // Settling immediately and progress to the correct pickup flow.
    sm.Reset();
    sm.DebugSetPhysicalOccupancyMask((1ULL << 4) | (1ULL << 20) | (1ULL << 30));
    sm.SetExpectedMoverAndOpponentOccupancyMasks(
        1ULL << 4, (1ULL << 20) | (1ULL << 30));

    sm.HandleSensorChange(4, false);
    Check(sm.IsHumanPieceInHand(),
          "Idle->PickedUp (blocked settle regression setup)");

    sm.SetValidDestMask(1ULL << 12);
    sm.HandleSensorChange(12, true);
    Check(sm.IsMoveSettlingActive(),
          "PickedUp->Settling (blocked settle regression setup)");
    sm.DebugSetPendingCaptureSquare(30);

    sm.HandleSensorChange(20, false);
    Check(!sm.IsMoveSettlingActive(),
          "pending capture metadata does not block settle confirmation");
    Check(sm.IsHumanPieceInHand(),
          "other pickup remains available for post-action replay");

    // Forced placement takes ownership from an active human settle. Correcting
    // that modal must not leave a stopped-but-active settle flag that blocks
    // the next move's fresh confirmation window.
    sm.Reset();
    sm.SetExpectedOccupancyMask(1ULL << 4U);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(1ULL << 4U, 0ULL);
    sm.DebugSetPhysicalOccupancyMask(1ULL << 4U);
    sm.HandleSensorChange(4U, false);
    sm.SetValidDestMask(1ULL << 12U);
    sm.HandleSensorChange(12U, true);
    Check(sm.IsMoveSettlingActive(),
          "force-placement setup starts human settle");
    Check(sm.ForceEnterPlacementMode(),
          "force placement accepts active human settle");
    Check(sm.IsPlacementModeActive() && !sm.IsMoveSettlingActive(),
          "force placement clears orthogonal settle ownership");
    sm.HandleBoardObservation((1ULL << 4U) | (1ULL << 12U),
                              1ULL << 4U,
                              clock.NowMs(),
                              sm.GetLastBoardObservationSequence() + 1U);
    advanceTime(cfg.placementStableRequiredMs);
    sm.Tick();
    Check(!sm.IsPlacementModeActive() && !sm.IsMoveSettlingActive(),
          "corrected placement exits without stale settle");
    sm.HandleSensorChange(4U, false);
    sm.SetValidDestMask(1ULL << 12U);
    sm.HandleSensorChange(12U, true);
    Check(sm.IsMoveSettlingActive(),
          "move after forced placement starts a fresh settle");

    // Opponent pickup while idle: if the lifted piece is not a legal capture
    // target, it should be immediately flagged as invalid.
    sm.Reset();
    sm.DebugSetPhysicalOccupancyMask((1ULL << 4) | (1ULL << 20));
    sm.SetExpectedMoverAndOpponentOccupancyMasks((1ULL << 4), (1ULL << 20));
    sm.SetLegalCaptureTargetsMask(0, true);

    sm.HandleSensorChange(20, false);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction,
          "illegal opponent pickup enters explicit correction");
    Check(sm.HasHumanIllegal(), "Idle: illegal opponent pickup is flagged");
    Check((sm.GetHumanIllegalMask() & (1ULL << 20)) != 0,
          "Idle: illegal mask contains opponent square");
    Check(!sm.HasCaptureFirstStaging(), "Idle: illegal opponent pickup does not stage capture-first");

    // Returning the piece clears the illegal marker.
    sm.HandleSensorChange(20, true);
    Check(!sm.HasHumanIllegal(), "Idle: returning illegal pickup clears marker");

    // MOVE_CONFIRM settle_5s: settling should wait for the extended duration.
    sm.Reset();
    sm.SetMoveConfirmPolicy(5000U, false);
    sm.HandleSensorChange(4, false);
    Check(sm.IsHumanPieceInHand(), "settle_5s: Idle->PickedUp");
    sm.SetValidDestMask(1ULL << 12);
    sm.HandleSensorChange(12, true);
    Check(sm.IsMoveSettlingActive(), "settle_5s: PickedUp->Settling");

    uint32_t settleElapsedMs = 0;
    uint32_t settleDurationMs = 0;
    Check(sm.TryGetSettleElapsedAndDurationMs(settleElapsedMs, settleDurationMs),
          "settle_5s: settle timing available");
    Check(settleDurationMs == 5000U, "settle_5s: settle timing uses 5s duration");

    advanceTime(4999);
    sm.Tick();
    Check(sm.IsMoveSettlingActive(),
          "settle_5s: still settling before 5s timeout");

    advanceTime(1);
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "settle_5s: timeout exposes its action");

    // MOVE_CONFIRM press_circle_to_confirm: no timer auto-finalize.
    sm.Reset();
    sm.SetMoveConfirmPolicy(2000U, true);
    sm.SetExpectedOccupancyMask((1ULL << 4) | (1ULL << 20));
    sm.DebugSetPhysicalOccupancyMask((1ULL << 4) | (1ULL << 20));
    sm.SetExpectedMoverAndOpponentOccupancyMasks(1ULL << 4,
                                                 1ULL << 20);

    sm.HandleSensorChange(4, false);
    Check(sm.IsHumanPieceInHand(),
          "press_circle_to_confirm: Idle->PickedUp");
    sm.SetValidDestMask(1ULL << 12);
    sm.HandleSensorChange(12, true);
    Check(sm.IsMoveSettlingActive(),
          "press_circle_to_confirm: PickedUp->Settling");

    advanceTime(10000);
    sm.Tick();
    Check(sm.IsMoveSettlingActive(),
          "press_circle_to_confirm: timer does not auto-finalize");

    sm.HandleSensorChange(20, false);
    Check(sm.IsMoveSettlingActive(),
          "press_circle_to_confirm: other pickup does not confirm settle");
    Check(!sm.HasCommittedMovePending(),
          "press_circle_to_confirm: other pickup does not expose the move");
    Check((sm.GetHumanIllegalMask() & (1ULL << 20)) != 0ULL,
          "press_circle_to_confirm: other pickup is explicitly invalid");
    sm.HandleSensorChange(20, true);
    Check(sm.IsMoveSettlingActive() && !sm.HasHumanIllegal(),
          "press_circle_to_confirm: returning other piece restores confirmation");

    // Re-picking the piece that is currently settling is the one observation
    // that aborts the settle and returns to the original placement flow.
    sm.Reset();
    sm.SetMoveConfirmPolicy(2000U, true);
    sm.SetExpectedOccupancyMask((1ULL << 4) | (1ULL << 20));
    sm.DebugSetPhysicalOccupancyMask((1ULL << 4) | (1ULL << 20));
    sm.HandleSensorChange(4, false);
    sm.SetValidDestMask(1ULL << 12);
    sm.HandleSensorChange(12, true);
    sm.HandleSensorChange(12, false);
    Check(sm.IsHumanPieceInHand(),
          "press_circle_to_confirm: settling piece pickup returns to placement");
    Check(sm.GetChessInteractionModel().phase.selectedSquare == 4U,
          "press_circle_to_confirm: settling piece pickup keeps original source");
    Check(!sm.HasCommittedMovePending(),
          "press_circle_to_confirm: settling piece pickup does not confirm the move");

    // An unrelated extra placement cannot substitute for explicit O consent.
    sm.Reset();
    sm.SetMoveConfirmPolicy(2000U, true);
    sm.SetExpectedOccupancyMask(1ULL << 4);
    sm.DebugSetPhysicalOccupancyMask(1ULL << 4);
    sm.HandleSensorChange(4, false);
    sm.SetValidDestMask(1ULL << 12);
    sm.HandleSensorChange(12, true);
    sm.HandleSensorChange(30, true);
    Check(sm.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Settling &&
              !sm.HasCommittedMovePending(),
          "press_circle_to_confirm: other placement preserves pending confirmation");
    Check((sm.GetHumanIllegalMask() & (1ULL << 30)) != 0ULL,
          "press_circle_to_confirm: other placement receives correction feedback");
    sm.HandleSensorChange(30, false);
    Check(sm.IsMoveSettlingActive() && !sm.HasHumanIllegal() &&
              !sm.HasCommittedMovePending(),
          "press_circle_to_confirm: removing the extra restores pending confirmation");

    // The explicit O path still runs its short confirmation settle when no new
    // board interaction supersedes it.
    sm.Reset();
    sm.SetMoveConfirmPolicy(2000U, true);
    sm.SetExpectedOccupancyMask(1ULL << 4);
    sm.DebugSetPhysicalOccupancyMask(1ULL << 4);
    sm.HandleSensorChange(4, false);
    sm.SetValidDestMask(1ULL << 12);
    sm.HandleSensorChange(12, true);

    sm.HandleButtonEvent(modules::UserButton::P1O, fxe::Button::Event::Press);
    Check(sm.IsMoveSettlingActive(),
          "press_circle_to_confirm: O press starts quick finalize settle");

    uint32_t postConfirmElapsedMs = 0;
    uint32_t postConfirmDurationMs = 0;
    Check(sm.TryGetSettleElapsedAndDurationMs(postConfirmElapsedMs, postConfirmDurationMs),
          "press_circle_to_confirm: finalize settle timing available");
    Check(postConfirmDurationMs == 1000U,
          "press_circle_to_confirm: finalize settle uses 1s duration");

    advanceTime(999);
    sm.Tick();
    Check(sm.IsMoveSettlingActive(),
          "press_circle_to_confirm: still settling before 1s timeout");

    advanceTime(1);
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "press_circle_to_confirm: confirmation exposes its action");

    // Illegal self-check prompt should remain active while the triggering
    // illegal marker is latched, then clear once corrected.
    sm.Reset();
    sm.SetExpectedOccupancyMask((1ULL << 12) | (1ULL << 60) | (1ULL << 4));
    sm.SetExpectedMoverAndOpponentOccupancyMasks((1ULL << 12) | (1ULL << 60), 1ULL << 4);
    sm.DebugSetPhysicalOccupancyMask((1ULL << 12) | (1ULL << 60) | (1ULL << 4));
    sm.HandleSensorChange(12, false);
    Check(sm.IsHumanPieceInHand(), "self-check prompt: pickup entered");
    Check(sm.RejectCurrentPickupAsIllegal(12), "self-check prompt: reject pickup succeeds");
    sm.SetKingSafetyPrompt(12, 60, 4);

    uint8_t kingSquare = 255;
    uint8_t attackerSquare = 255;
    Check(sm.TryGetKingSafetyPrompt(kingSquare, attackerSquare),
          "self-check prompt: active while illegal latched");
    Check(kingSquare == 60, "self-check prompt: king square propagated");
    Check(attackerSquare == 4, "self-check prompt: attacker square propagated");

    sm.HandleSensorChange(12, true);
    Check(!sm.HasHumanIllegal(), "self-check prompt: correction clears illegal marker");
    Check(!sm.TryGetKingSafetyPrompt(kingSquare, attackerSquare),
          "self-check prompt: clears when trigger illegal clears");
#else
    std::puts("[GAME_STATE_MACHINE_PLACING] skipped (ARDUINO)");
#endif

    if (g_failures == 0) {
        std::puts("[GAME_STATE_MACHINE_PLACING] ok");
        return 0;
    }

    std::printf("[GAME_STATE_MACHINE_PLACING] failures=%d\n", g_failures);
    return 1;
}
