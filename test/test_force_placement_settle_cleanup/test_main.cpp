#include <cstdio>

#include "runtime/game_state_machine.hpp"

namespace {

int gFailures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++gFailures;
    }
}

void TestUnclaimedCastle() {
    constexpr uint64_t BEFORE = (1ULL << 60U) | (1ULL << 63U);
    constexpr uint64_t AFTER = (1ULL << 62U) | (1ULL << 61U);
    runtime::GameStateMachine machine;
    machine.HandleInteractionPositionAcknowledged(
        BEFORE, 1ULL << 60U, 1ULL << 63U, BEFORE, 0ULL);
    machine.ApplyPhysicalOccupancySnapshot(BEFORE);
    uint32_t sequence = machine.GetLastBoardObservationSequence() + 1U;
    machine.HandleBoardObservation(BEFORE, 0ULL, 1U, sequence++);
    machine.HandleBoardObservation(AFTER, AFTER, 2U, sequence++);
    Check(machine.HasCommittedMovePending(),
          "force-placement castle fixture has an unclaimed action");
    Check(machine.ForceEnterPlacementMode(),
          "force placement accepts an unclaimed castle");
    Check(!machine.HasCommittedMovePending() &&
              machine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Idle,
          "force placement cancels the unclaimed castle token and plan");
    Check(!machine.IsMoveSettlingActive(),
          "force placement clears composite settle ownership");
}

void TestCpuPostAckBounce() {
    constexpr uint8_t FROM = 12U;
    constexpr uint8_t TO = 28U;
    constexpr uint64_t BEFORE = 1ULL << FROM;
    constexpr uint64_t AFTER = 1ULL << TO;
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 1000U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    runtime::GameStateMachine machine(config);
    machine.HandleInteractionPositionAcknowledged(
        BEFORE, 0ULL, 0ULL, BEFORE, 0ULL);
    machine.ApplyPhysicalOccupancySnapshot(BEFORE);
    Check(machine.QueueCpuMove(FROM, TO, false, 0U, '\0'),
          "queue CPU force-placement fixture");
    uint32_t sequence = machine.GetLastBoardObservationSequence() + 1U;
    machine.HandleBoardObservation(BEFORE, 0ULL, 10U, sequence++);
    machine.HandleBoardObservation(AFTER, AFTER, 11U, sequence++);
    machine.Tick();
    runtime::PhysicalTransitionPlan action;
    Check(machine.TryClaimPendingInteractionMove(action),
          "stable CPU move exposes its action");
    const uint32_t token =
        machine.GetChessInteractionModel().phase.pendingActionToken;
    machine.HandleInteractionActionResult(token, true);
    machine.HandleBoardObservation(AFTER, 0ULL, 12U, sequence++);
    Check(machine.IsCpuMoveSettlingActive(),
          "CPU destination bounce remains owned by settle before force");
    Check(machine.ForceEnterPlacementMode(),
          "force placement accepts a CPU settle bounce");
    Check(machine.IsPlacementModeActive() &&
              !machine.IsMoveSettlingActive(),
          "force placement clears CPU settle and suppression ownership");
}

void TestRepeatedForceEntryAfterRecovery() {
    constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
    runtime::GameStateMachine machine;
    machine.HandleInteractionPositionAcknowledged(OPENING, 0ULL, 0ULL, OPENING, 0ULL);
    machine.ApplyPhysicalOccupancySnapshot(0ULL);
    Check(machine.ForceEnterPlacementMode(), "empty startup owns placement");
    for (uint32_t sequence = 1U; sequence <= 3U; ++sequence) {
        machine.HandleRecoveryBoardObservation(0ULL, 0ULL, sequence, sequence);
        Check(machine.GetHumanIllegalMask() == OPENING,
              "recovery fixture projects the exact missing-piece correction");
        Check(machine.ForceEnterPlacementMode() &&
                  machine.GetHumanIllegalMask() == 0ULL &&
                  machine.GetPlacementMissingMask() == OPENING,
              "repeated forced placement clears generic correction and retains missing-piece guidance");
    }
}

} // namespace

int main() {
    std::puts("[FORCE_PLACEMENT_SETTLE_CLEANUP] start");
    TestUnclaimedCastle();
    TestCpuPostAckBounce();
    TestRepeatedForceEntryAfterRecovery();
    if (gFailures == 0) {
        std::puts("[FORCE_PLACEMENT_SETTLE_CLEANUP] ok");
        return 0;
    }
    std::printf("[FORCE_PLACEMENT_SETTLE_CLEANUP] failures=%d\n",
                gFailures);
    return 1;
}
