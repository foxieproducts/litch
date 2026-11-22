#include <cstdio>

#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

void TestRejectedOConfirmCastle() {
    constexpr uint8_t KING_FROM = 60U;
    constexpr uint8_t KING_TO = 62U;
    constexpr uint8_t ROOK_FROM = 63U;
    constexpr uint8_t ROOK_TO = 61U;
    constexpr uint64_t BEFORE =
        (1ULL << KING_FROM) | (1ULL << ROOK_FROM);
    constexpr uint64_t PICKUP = BEFORE;
    constexpr uint64_t AFTER =
        (1ULL << KING_TO) | (1ULL << ROOK_TO);

    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    stateMachine.SetMoveConfirmPolicy(0U, true);
    stateMachine.HandleInteractionPositionAcknowledged(
        BEFORE, 1ULL << KING_FROM, 1ULL << ROOK_FROM, BEFORE, 0ULL);
    stateMachine.ApplyPhysicalOccupancySnapshot(BEFORE);
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;
    stateMachine.HandleBoardObservation(
        PICKUP, 0ULL, 1000U, sequence++);
    stateMachine.HandleBoardObservation(
        AFTER, AFTER, 1001U, sequence++);
    Check(stateMachine.IsMoveSettlingActive() &&
              stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::AwaitingAction,
          "O-confirm castle owns an orthogonal settle while awaiting action");

    runtime::PhysicalTransitionPlan action;
    Check(stateMachine.TryClaimPendingInteractionMove(action),
          "completed castle exposes one action");
    const uint32_t token =
        stateMachine.GetChessInteractionModel().phase.pendingActionToken;
    stateMachine.HandleInteractionActionResult(token, false);
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction,
          "rejected castle enters correction");
    Check(!stateMachine.IsMoveSettlingActive(),
          "rejected O-confirm castle releases settle ownership immediately");

    stateMachine.HandleBoardObservation(
        BEFORE | AFTER, BEFORE, 1002U, sequence++);
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle &&
              !stateMachine.IsMoveSettlingActive(),
          "corrected rejected castle returns to idle without stale settle");
}

void TestRejectedCpuMove() {
    constexpr uint8_t FROM = 12U;
    constexpr uint8_t TO = 28U;
    constexpr uint64_t BEFORE = 1ULL << FROM;
    constexpr uint64_t AFTER = 1ULL << TO;
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 100U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    runtime::GameStateMachine stateMachine(config);
    stateMachine.Reset();
    stateMachine.HandleInteractionPositionAcknowledged(
        BEFORE, 0ULL, 0ULL, BEFORE, 0ULL);
    stateMachine.ApplyPhysicalOccupancySnapshot(BEFORE);
    Check(stateMachine.QueueCpuMove(FROM, TO, false, 0U, '\0'),
          "queue CPU rejection fixture");
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;
    stateMachine.HandleBoardObservation(
        1ULL << FROM, 0ULL, 2000U, sequence++);
    stateMachine.HandleBoardObservation(
        1ULL << TO, AFTER, 2001U, sequence++);
    stateMachine.Tick();
    Check(stateMachine.IsCpuMoveSettlingActive(),
          "stable CPU move starts settle before action result");
    runtime::PhysicalTransitionPlan action;
    Check(stateMachine.TryClaimPendingInteractionMove(action),
          "stable CPU move exposes action");
    const uint32_t token =
        stateMachine.GetChessInteractionModel().phase.pendingActionToken;
    stateMachine.HandleInteractionActionResult(token, false);
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction &&
              !stateMachine.IsMoveSettlingActive(),
          "rejected CPU action clears settle and enters correction");
}

} // namespace

int main() {
    std::puts("[ACTION_REJECTION_CLEARS_SETTLE] start");
    TestRejectedOConfirmCastle();
    TestRejectedCpuMove();
    if (g_failures == 0) {
        std::puts("[ACTION_REJECTION_CLEARS_SETTLE] ok");
        return 0;
    }
    std::printf("[ACTION_REJECTION_CLEARS_SETTLE] failures=%d\n",
                g_failures);
    return 1;
}
