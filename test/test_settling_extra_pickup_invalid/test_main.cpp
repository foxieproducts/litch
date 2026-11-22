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
    std::puts("[SETTLING_NEXT_PICKUP] start");

#ifndef ARDUINO
    runtime::ManualClock clock(1000);

    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 100;
    cfg.cpuIdleDelayMs = 999999;
    cfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();
    sm.SetMoveConfirmPolicy(2000U, false);

    // Seed a minimal expected/physical occupancy model:
    // - a movable piece on FROM
    // - another piece on OTHER (the one we'll pick up during settle)
    static constexpr uint8_t FROM = 10;
    static constexpr uint8_t TO = 18;
    static constexpr uint8_t OTHER = 20;

    const uint64_t initialOcc = (1ULL << FROM) | (1ULL << OTHER);
    sm.SetExpectedOccupancyMask(initialOcc);
    sm.SetExpectedMoverAndOpponentOccupancyMasks((1ULL << FROM) | (1ULL << OTHER), 0);
    sm.DebugSetPhysicalOccupancyMask(initialOcc);

    sm.SetValidDestMask(1ULL << TO);

    sm.HandleSensorChange(FROM, false);
    Check(sm.IsHumanPieceInHand(), "pickup enters PickedUp");

    sm.HandleSensorChange(TO, true);
    Check(sm.IsMoveSettlingActive(), "place enters Settling");

    // User begins the next move target-first while the previous move is still
    // settling. Settling must finish immediately, then the removal must be
    // processed by the normal post-move pickup rules.
    sm.HandleSensorChange(OTHER, false);

    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "target-first pickup first exposes the completed settle action");
    Check(!sm.HasHumanIllegal(),
          "target-first pickup during settling is not discarded as illegal");
    Check(sm.HasCaptureFirstStaging(),
          "target-first pickup during settling is registered for the next move");
    Check((sm.GetCaptureFirstStagingMask() & (1ULL << OTHER)) != 0ULL,
          "capture-first state records the newly lifted square");

    // Production hardware supplies atomic observations. Verify the same
    // pass-through behavior without inventing an order between changed bits.
    sm.Reset();
    sm.SetMoveConfirmPolicy(5000U, false);
    sm.SetExpectedOccupancyMask(initialOcc);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(
        (1ULL << FROM) | (1ULL << OTHER), 0ULL);
    sm.DebugSetPhysicalOccupancyMask(initialOcc);
    sm.SetValidDestMask(1ULL << TO);

    const uint64_t liftedOcc = initialOcc & ~(1ULL << FROM);
    const uint64_t placedOcc = liftedOcc | (1ULL << TO);
    sm.HandleBoardObservation(1ULL << FROM, liftedOcc, 1000U, 1U);
    sm.HandleBoardObservation(1ULL << TO, placedOcc, 1001U, 2U);
    Check(sm.IsMoveSettlingActive(),
          "atomic setup enters Settling");

    sm.HandleBoardObservation(1ULL << OTHER,
                              placedOcc & ~(1ULL << OTHER),
                              1002U,
                              3U);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "atomic target-first pickup first exposes the completed settle action");
    Check(!sm.HasHumanIllegal(),
          "atomic target-first pickup does not latch invalid feedback");
    Check((sm.GetCaptureFirstStagingMask() & (1ULL << OTHER)) != 0ULL,
          "atomic target-first pickup records the newly lifted square");
    Check(sm.GetLastBoardObservationSequence() == 3U,
          "atomic pickup remains the canonical latest observation");

    // The destination by itself is the sole cancel gesture. It abandons the
    // pending placement and preserves the original source as the lifted piece.
    sm.Reset();
    sm.SetExpectedOccupancyMask(initialOcc);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(
        1ULL << FROM, 1ULL << OTHER);
    sm.DebugSetPhysicalOccupancyMask(initialOcc);
    sm.SetValidDestMask(1ULL << TO);
    sm.HandleBoardObservation(1ULL << FROM, liftedOcc, 1100U, 1U);
    sm.HandleBoardObservation(1ULL << TO, placedOcc, 1101U, 2U);
    sm.HandleBoardObservation(1ULL << TO,
                              placedOcc & ~(1ULL << TO),
                              1102U,
                              3U);
    Check(sm.IsHumanPieceInHand(),
          "destination-only re-lift returns to placement");
    Check(sm.GetChessInteractionModel().phase.selectedSquare == FROM,
          "destination-only re-lift preserves original source");
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::PieceLifted,
          "destination-only re-lift agrees with canonical reducer state");
    Check(!sm.HasCommittedMovePending(),
          "destination-only re-lift does not commit the abandoned move");

    // The cancel gesture must be destination-only. If another piece changes in
    // the same hardware observation, the old move is confirmed and the new
    // atomic observation remains available for post-action replay.
    sm.Reset();
    sm.SetExpectedOccupancyMask(initialOcc);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(
        1ULL << FROM, 1ULL << OTHER);
    sm.DebugSetPhysicalOccupancyMask(initialOcc);
    sm.SetValidDestMask(1ULL << TO);
    sm.HandleBoardObservation(1ULL << FROM, liftedOcc, 1150U, 1U);
    sm.HandleBoardObservation(1ULL << TO, placedOcc, 1151U, 2U);
    const uint64_t ambiguousOcc =
        placedOcc & ~(1ULL << TO) & ~(1ULL << OTHER);
    sm.HandleBoardObservation((1ULL << TO) | (1ULL << OTHER),
                              ambiguousOcc,
                              1152U,
                              3U);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "destination plus another change confirms instead of canceling");
    Check(sm.HasCommittedMovePending(),
          "mixed destination observation exposes the confirmed move");
    Check(sm.IsHumanPieceInHand(),
          "mixed destination observation projects the new mover pickup");

    // A pickup of the next side's own piece confirms the settled move first,
    // then begins that pickup from the projected post-move position.
    sm.Reset();
    sm.SetExpectedOccupancyMask(initialOcc);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(
        1ULL << FROM, 1ULL << OTHER);
    sm.DebugSetPhysicalOccupancyMask(initialOcc);
    sm.SetValidDestMask(1ULL << TO);
    sm.HandleBoardObservation(1ULL << FROM, liftedOcc, 1200U, 1U);
    sm.HandleBoardObservation(1ULL << TO, placedOcc, 1201U, 2U);
    sm.HandleBoardObservation(1ULL << OTHER,
                              placedOcc & ~(1ULL << OTHER),
                              1202U,
                              3U);
    Check(sm.IsHumanPieceInHand(),
          "next mover pickup ends settle and enters PickedUp");
    uint8_t activeSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    Check(sm.TryGetHumanPieceInHandSquare(activeSquare) &&
              activeSquare == OTHER,
          "next mover pickup records the newly lifted source");
    Check(sm.HasCommittedMovePending(),
          "next mover pickup implicitly confirms the prior move");

    // A bare new placement also ends settling. Classify it immediately against
    // the projected post-move position so useful correction feedback does not
    // wait for engine acknowledgement. Source+destination observations from a
    // plausible next mover remain the narrower replay exception used by fast
    // legal continuations such as castling.
    static constexpr uint8_t EXTRA = 22U;
    sm.Reset();
    sm.SetExpectedOccupancyMask(initialOcc);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(
        1ULL << FROM, 1ULL << OTHER);
    sm.DebugSetPhysicalOccupancyMask(initialOcc);
    sm.SetValidDestMask(1ULL << TO);
    sm.HandleBoardObservation(1ULL << FROM, liftedOcc, 1300U, 1U);
    sm.HandleBoardObservation(1ULL << TO, placedOcc, 1301U, 2U);
    sm.HandleBoardObservation(1ULL << EXTRA,
                              placedOcc | (1ULL << EXTRA),
                              1302U,
                              3U);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "unrelated placement first exposes the completed settle action");
    Check((sm.GetHumanIllegalMask() & (1ULL << EXTRA)) != 0ULL,
          "unrelated placement is judged against projected position");
    Check(sm.HasCommittedMovePending(),
          "unrelated placement implicitly confirms the prior move");
#else
    std::puts("[SETTLING_EXTRA_PICKUP_INVALID] skipped (ARDUINO)");
#endif

    if (g_failures == 0) {
        std::puts("[SETTLING_NEXT_PICKUP] ok");
        return 0;
    }

    std::printf("[SETTLING_NEXT_PICKUP] failures=%d\n", g_failures);
    return 1;
}
