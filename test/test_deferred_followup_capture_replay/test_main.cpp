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

struct Fixture {
    static constexpr uint8_t OLD_FROM = 52U;
    static constexpr uint8_t OLD_TO = 36U;
    static constexpr uint8_t NEXT_FROM = 20U;
    static constexpr uint8_t TARGET = 27U;
    static constexpr uint8_t ILLEGAL_TARGET = 29U;
    static constexpr uint64_t OLD_FROM_MASK = 1ULL << OLD_FROM;
    static constexpr uint64_t OLD_TO_MASK = 1ULL << OLD_TO;
    static constexpr uint64_t NEXT_FROM_MASK = 1ULL << NEXT_FROM;
    static constexpr uint64_t TARGET_MASK = 1ULL << TARGET;
    static constexpr uint64_t ILLEGAL_TARGET_MASK = 1ULL << ILLEGAL_TARGET;

    runtime::GameStateMachine stateMachine;
    uint64_t physical = OLD_FROM_MASK | NEXT_FROM_MASK |
                        TARGET_MASK | ILLEGAL_TARGET_MASK;
    uint32_t sequence = 1U;

    Fixture() : stateMachine(MakeConfig()) {
        stateMachine.Reset();
        stateMachine.HandleInteractionPositionAcknowledged(
            physical, 0ULL, 0ULL, OLD_FROM_MASK,
            NEXT_FROM_MASK | TARGET_MASK | ILLEGAL_TARGET_MASK);
        stateMachine.ApplyPhysicalOccupancySnapshot(physical);
        stateMachine.SetValidDestMask(OLD_TO_MASK);
        Observe(OLD_FROM_MASK, false);
        Observe(OLD_TO_MASK, true);
        Check(stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::Settling,
              "fixture starts old move settle");
    }

    static runtime::GameStateMachineConfig MakeConfig() {
        runtime::GameStateMachineConfig config;
        config.settleDurationMs = 100U;
        config.cpuIdleDelayMs = 999999U;
        config.placementStableRequiredMs = 0U;
        return config;
    }

    void Observe(const uint64_t mask, const bool occupied) {
        if (occupied) {
            physical |= mask;
        } else {
            physical &= ~mask;
        }
        const uint32_t eventSequence = sequence++;
        stateMachine.HandleBoardObservation(
            mask, physical, eventSequence * 10U, eventSequence);
    }

    void AcknowledgeOldMove(
        const uint64_t legalTargets,
        const bool legalTargetsValid = true) {
        runtime::PhysicalTransitionPlan oldMove;
        Check(stateMachine.TryClaimPendingInteractionMove(oldMove),
              "settle interruption exposes old move exactly once");
        const uint32_t token =
            stateMachine.GetChessInteractionModel().phase.pendingActionToken;
        stateMachine.HandleInteractionActionResult(token, true);
        const uint64_t postMove = OLD_TO_MASK | NEXT_FROM_MASK |
                                  TARGET_MASK | ILLEGAL_TARGET_MASK;
        stateMachine.HandleInteractionPositionAcknowledged(
            postMove, 0ULL, 0ULL, NEXT_FROM_MASK,
            OLD_TO_MASK | TARGET_MASK | ILLEGAL_TARGET_MASK, 0ULL,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            true, false, legalTargetsValid, legalTargets);
    }
};

void TestTwoScanCaptureReplay(const bool targetFirst) {
    Fixture fixture;
    fixture.Observe(targetFirst ? Fixture::TARGET_MASK
                                : Fixture::NEXT_FROM_MASK,
                    false);
    fixture.Observe(targetFirst ? Fixture::NEXT_FROM_MASK
                                : Fixture::TARGET_MASK,
                    false);
    fixture.AcknowledgeOldMove(Fixture::TARGET_MASK);

    const runtime::TaggedInteractionState& phase =
        fixture.stateMachine.GetChessInteractionModel().phase;
    Check(phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
              phase.plan.kind == runtime::PhysicalTransitionKind::Capture,
          targetFirst
              ? "target-then-mover scans replay as one capture plan"
              : "mover-then-target scans replay as one capture plan");
    Check(phase.plan.moveFrom == Fixture::NEXT_FROM &&
              phase.plan.moveTo == Fixture::TARGET &&
              phase.plan.progress.removalComplete,
          "deferred capture replay retains both absent squares");
    fixture.Observe(Fixture::TARGET_MASK, true);
    Check(fixture.stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Settling,
          "deferred capture can complete with a fresh settle window");
}

void TestUnknownAndIllegalTargetsFailClosed() {
    Fixture unknown;
    unknown.Observe(Fixture::ILLEGAL_TARGET_MASK, false);
    unknown.AcknowledgeOldMove(0ULL, false);
    Check(unknown.stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction,
          "unknown post-action capture legality fails closed");

    Fixture illegal;
    illegal.Observe(Fixture::ILLEGAL_TARGET_MASK, false);
    illegal.AcknowledgeOldMove(Fixture::TARGET_MASK);
    Check(illegal.stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Correction,
          "illegal target-first removal during settle is not staged");
}

void TestCpuSettleReplaysNextPickupBeforeClaim() {
    constexpr uint8_t CPU_FROM = 12U;
    constexpr uint8_t CPU_TO = 28U;
    constexpr uint8_t NEXT_FROM = 52U;
    constexpr uint64_t CPU_FROM_MASK = 1ULL << CPU_FROM;
    constexpr uint64_t CPU_TO_MASK = 1ULL << CPU_TO;
    constexpr uint64_t NEXT_FROM_MASK = 1ULL << NEXT_FROM;
    constexpr uint64_t BEFORE = CPU_FROM_MASK | NEXT_FROM_MASK;
    constexpr uint64_t AFTER = CPU_TO_MASK | NEXT_FROM_MASK;

    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 100U;
    config.cpuIdleDelayMs = 999999U;
    config.placementStableRequiredMs = 0U;
    runtime::GameStateMachine stateMachine(config);
    stateMachine.HandleInteractionPositionAcknowledged(
        BEFORE, 0ULL, 0ULL, CPU_FROM_MASK, NEXT_FROM_MASK);
    stateMachine.ApplyPhysicalOccupancySnapshot(BEFORE);
    Check(stateMachine.QueueCpuMove(
              CPU_FROM, CPU_TO, false, 0U, '\0'),
          "queue CPU deferred-followup fixture");
    uint32_t sequence =
        stateMachine.GetLastBoardObservationSequence() + 1U;
    stateMachine.HandleBoardObservation(
        CPU_FROM_MASK, NEXT_FROM_MASK, 100U, sequence++);
    stateMachine.HandleBoardObservation(
        CPU_TO_MASK, AFTER, 101U, sequence++);
    stateMachine.Tick();
    Check(stateMachine.IsCpuMoveSettlingActive() &&
              stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::AwaitingAction,
          "completed CPU move owns a visual settle before claim");

    stateMachine.HandleBoardObservation(
        NEXT_FROM_MASK, CPU_TO_MASK, 102U, sequence++);
    Check(!stateMachine.IsMoveSettlingActive() &&
              stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::AwaitingAction,
          "next pickup ends CPU settle without stalling its action");

    runtime::PhysicalTransitionPlan oldMove;
    Check(stateMachine.TryClaimPendingInteractionMove(oldMove),
          "CPU action remains claimable with a deferred next-turn pickup");
    const uint32_t token =
        stateMachine.GetChessInteractionModel().phase.pendingActionToken;
    stateMachine.HandleInteractionActionResult(token, true);
    stateMachine.HandleInteractionPositionAcknowledged(
        AFTER, 0ULL, 0ULL, NEXT_FROM_MASK, CPU_TO_MASK);
    const runtime::TaggedInteractionState& phase =
        stateMachine.GetChessInteractionModel().phase;
    Check(phase.tag == runtime::ChessInteractionPhase::PieceLifted &&
              phase.selectedSquare == NEXT_FROM,
          "CPU acknowledgement replays the deferred pickup as the next move");
}

} // namespace

int main() {
    std::puts("[DEFERRED_FOLLOWUP_CAPTURE_REPLAY] start");
    TestTwoScanCaptureReplay(true);
    TestTwoScanCaptureReplay(false);
    TestUnknownAndIllegalTargetsFailClosed();
    TestCpuSettleReplaysNextPickupBeforeClaim();
    if (g_failures == 0) {
        std::puts("[DEFERRED_FOLLOWUP_CAPTURE_REPLAY] ok");
        return 0;
    }
    std::printf("[DEFERRED_FOLLOWUP_CAPTURE_REPLAY] failures=%d\n",
                g_failures);
    return 1;
}
