#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/chessboard_hardware.hpp"
#include "modules/runtime_health.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

class ScriptedAtomicInput final : public modules::IBoardInput {
  public:
    explicit ScriptedAtomicInput(const uint64_t initialOccupancy)
        : m_snapshot(initialOccupancy) {}

    bool TryReadSnapshot(uint64_t& outOcc64) override {
        outOcc64 = m_snapshot;
        return true;
    }

    bool TryReadDelta(modules::SquareDelta&) override { return false; }
    bool SupportsAtomicObservations() const override { return true; }
    void Resync() override {}

    modules::BoardScanAttemptResult CaptureScanAttempt() override {
        return m_next;
    }

    void SetAttempt(const modules::BoardScanAttemptStatus status,
                    const uint64_t rawOccupancy = 0ULL) {
        m_next = modules::BoardScanAttemptResult{status, rawOccupancy};
    }

  private:
    uint64_t m_snapshot = 0ULL;
    modules::BoardScanAttemptResult m_next{};
};

void Poll(const uint32_t nowMs,
          hardware::ChessboardHardware& board,
          modules::BoardInputRuntimeHealth& health,
          runtime::InteractionDispatcher& dispatcher) {
    fxe::ManualMillis::SetNowMs(nowMs);
    const modules::BoardScanAttemptResult attempt = board.CaptureScanAttempt();
    const modules::BoardInputHealthDecision decision =
        health.ObserveScanAttempt(nowMs, attempt);
    if (!decision.dispatchObservation) {
        return;
    }
    const bool accepted = board.AcceptAndDispatchScan(
        attempt,
        dispatcher,
        decision.authoritativeResync
            ? hardware::ChessboardHardware::ObservationDispatchMode::Always
            : hardware::ChessboardHardware::ObservationDispatchMode::ChangedOnly);
    if (decision.authoritativeResync) {
        health.AcknowledgeAuthoritativeResync(accepted);
    }
}

} // namespace

int main() {
    std::puts("[RUNTIME_HEALTH_INTEGRATION] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

    constexpr uint64_t OCCUPANCY = 1ULL << 52U;
    constexpr uint64_t MOVED_OCCUPANCY = 1ULL << 44U;
    ScriptedAtomicInput input(OCCUPANCY);
    hardware::ChessboardHardware board(input);
    board.Resync();

    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    stateMachine.ApplyPhysicalOccupancySnapshot(OCCUPANCY);
    runtime::ChessPositionState expected;
    expected.occupiedMask = OCCUPANCY;
    expected.moverOccupiedMask = OCCUPANCY;
    stateMachine.DebugSetExpectedPositionModel(expected);
    runtime::InteractionDispatcher dispatcher(stateMachine);
    runtime::RuntimeHealthReducer reducer;
    modules::BoardInputRuntimeHealth health(reducer);

    input.SetAttempt(modules::BoardScanAttemptStatus::Failed, UINT64_MAX);
    Poll(0U, board, health, dispatcher);
    input.SetAttempt(modules::BoardScanAttemptStatus::NotDue);
    Poll(50U, board, health, dispatcher);
    input.SetAttempt(modules::BoardScanAttemptStatus::Failed, UINT64_MAX);
    Poll(100U, board, health, dispatcher);

    Check(health.Snapshot().state == runtime::RuntimeHealthState::InputPaused,
          "sustained hardware failure reaches the shared reducer");
    Check(stateMachine.GetLastBoardObservationSequence() == 0U &&
              stateMachine.GetPhysicalOccupancyMask() == OCCUPANCY &&
              board.GetOccupied(52U),
          "failed and NotDue polls preserve last-known occupancy without dispatch");

    input.SetAttempt(modules::BoardScanAttemptStatus::Succeeded,
                     MOVED_OCCUPANCY);
    Poll(101U, board, health, dispatcher);
    Poll(200U, board, health, dispatcher);
    Check(stateMachine.GetLastBoardObservationSequence() == 0U,
          "pre-boundary fresh scans remain blocked from move inference");

    Poll(201U, board, health, dispatcher);
    Check(health.Snapshot().state == runtime::RuntimeHealthState::Healthy &&
              stateMachine.GetLastBoardObservationSequence() == 1U &&
              stateMachine.GetLastBoardObservationChangedMask() ==
                  (OCCUPANCY | MOVED_OCCUPANCY) &&
              stateMachine.GetPhysicalOccupancyMask() == MOVED_OCCUPANCY,
          "bounded recovery dispatches one complete authoritative snapshot");
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Correction &&
              !stateMachine.IsMoveSettlingActive(),
          "changed recovery snapshot becomes correction, never a synthetic move");

    Poll(202U, board, health, dispatcher);
    Check(stateMachine.GetLastBoardObservationSequence() == 1U,
          "ordinary unchanged scan after recovery does not repeat resync");

    // Production assigns one global sequence across ticks, buttons, and board
    // events. Recovery must enter through that same sequence authority rather
    // than reuse ChessboardHardware's smaller local observation count.
    ScriptedAtomicInput sequencedInput(OCCUPANCY);
    hardware::ChessboardHardware sequencedBoard(sequencedInput);
    sequencedBoard.Resync();
    runtime::GameStateMachine sequencedStateMachine;
    sequencedStateMachine.Reset();
    sequencedStateMachine.ApplyPhysicalOccupancySnapshot(OCCUPANCY);
    sequencedStateMachine.DebugSetExpectedPositionModel(expected);
    runtime::AppEventQueue eventQueue;
    eventQueue.Reset();
    runtime::InteractionDispatcher sequencedDispatcher(
        sequencedStateMachine, eventQueue);
    runtime::RuntimeHealthReducer sequencedReducer;
    modules::BoardInputRuntimeHealth sequencedHealth(sequencedReducer);

    sequencedDispatcher.DispatchTick(300U);
    sequencedDispatcher.DispatchTick(301U);
    sequencedInput.SetAttempt(modules::BoardScanAttemptStatus::Succeeded,
                              MOVED_OCCUPANCY);
    Poll(302U, sequencedBoard, sequencedHealth, sequencedDispatcher);
    Check(sequencedStateMachine.GetLastBoardObservationSequence() == 3U &&
              sequencedStateMachine.GetPhysicalOccupancyMask() ==
                  MOVED_OCCUPANCY,
          "ordinary production observation uses the global event sequence");

    sequencedInput.SetAttempt(modules::BoardScanAttemptStatus::Failed);
    Poll(303U, sequencedBoard, sequencedHealth, sequencedDispatcher);
    Poll(403U, sequencedBoard, sequencedHealth, sequencedDispatcher);
    sequencedInput.SetAttempt(modules::BoardScanAttemptStatus::Succeeded,
                              OCCUPANCY);
    Poll(404U, sequencedBoard, sequencedHealth, sequencedDispatcher);
    Poll(504U, sequencedBoard, sequencedHealth, sequencedDispatcher);
    Check(sequencedHealth.Snapshot().state ==
                  runtime::RuntimeHealthState::Healthy &&
              sequencedStateMachine.GetLastBoardObservationSequence() == 4U &&
              sequencedStateMachine.GetPhysicalOccupancyMask() == OCCUPANCY,
          "recovery resync uses the next global sequence and repairs occupancy");

    // A real I2C pause can span the instant the next player lifts a piece.
    // Recovery still has enough tagged-plan evidence to recognize that exact
    // one-square next-side pickup; it must not turn legal fast play into the
    // generic invalid-board recovery state.
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E4 = 36U;
    constexpr uint8_t E7 = 12U;
    constexpr uint64_t BEFORE = (1ULL << E2) | (1ULL << E7);
    constexpr uint64_t E2_LIFTED = BEFORE & ~(1ULL << E2);
    constexpr uint64_t E4_PLACED = E2_LIFTED | (1ULL << E4);
    constexpr uint64_t NEXT_PICKUP = E4_PLACED & ~(1ULL << E7);

    ScriptedAtomicInput settleInput(BEFORE);
    hardware::ChessboardHardware settleBoard(settleInput);
    settleBoard.Resync();
    runtime::GameStateMachineConfig settleConfig;
    settleConfig.settleDurationMs = 2000U;
    runtime::GameStateMachine settleStateMachine(settleConfig);
    settleStateMachine.Reset();
    settleStateMachine.ApplyPhysicalOccupancySnapshot(BEFORE);
    runtime::ChessPositionState settleExpected;
    settleExpected.occupiedMask = BEFORE;
    settleExpected.moverOccupiedMask = 1ULL << E2;
    settleExpected.opponentOccupiedMask = 1ULL << E7;
    settleExpected.sideToMoveValid = true;
    settleStateMachine.DebugSetExpectedPositionModel(settleExpected);
    runtime::InteractionDispatcher settleDispatcher(settleStateMachine);
    runtime::RuntimeHealthReducer settleReducer;
    modules::BoardInputRuntimeHealth settleHealth(settleReducer);

    settleInput.SetAttempt(
        modules::BoardScanAttemptStatus::Succeeded, E2_LIFTED);
    Poll(1000U, settleBoard, settleHealth, settleDispatcher);
    settleStateMachine.SetValidDestMask(1ULL << E4);
    settleInput.SetAttempt(
        modules::BoardScanAttemptStatus::Succeeded, E4_PLACED);
    Poll(1001U, settleBoard, settleHealth, settleDispatcher);
    Check(settleStateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Settling,
          "health recovery regression begins with a legal settling move");

    settleInput.SetAttempt(modules::BoardScanAttemptStatus::Failed);
    Poll(1002U, settleBoard, settleHealth, settleDispatcher);
    Poll(1102U, settleBoard, settleHealth, settleDispatcher);
    Check(settleHealth.Snapshot().state ==
              runtime::RuntimeHealthState::InputPaused,
          "sustained scan failure pauses the settling observation path");

    settleInput.SetAttempt(
        modules::BoardScanAttemptStatus::Succeeded, NEXT_PICKUP);
    Poll(1103U, settleBoard, settleHealth, settleDispatcher);
    Poll(1203U, settleBoard, settleHealth, settleDispatcher);
    Check(settleHealth.Snapshot().state ==
                  runtime::RuntimeHealthState::Healthy &&
              settleStateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::AwaitingAction &&
              !settleStateMachine.HasHumanIllegal() &&
              settleStateMachine.GetChessInteractionModel()
                      .phase.correctionMask == 0ULL,
          "authoritative recovery preserves a valid next-player pickup during settling");

    if (g_failures == 0) {
        std::puts("[RUNTIME_HEALTH_INTEGRATION] ok");
        return 0;
    }
    std::printf("[RUNTIME_HEALTH_INTEGRATION] failures=%d\n", g_failures);
    return 1;
}
