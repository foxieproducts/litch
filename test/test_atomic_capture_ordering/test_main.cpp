#include <cstdio>

#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

void Observe(runtime::GameStateMachine& stateMachine,
             const uint32_t sequence,
             const uint64_t changedMask,
             const uint64_t occupiedMask) {
    stateMachine.HandleBoardObservation(changedMask,
                                        occupiedMask,
                                        sequence * 10U,
                                        sequence);
}

void TestAtomicNormalCapture(const uint8_t from,
                             const uint8_t target,
                             const char* orderingLabel) {
    const uint64_t fromMask = 1ULL << from;
    const uint64_t targetMask = 1ULL << target;
    const uint64_t before = fromMask | targetMask;

    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    stateMachine.HandleInteractionPositionAcknowledged(
        before, 0ULL, 0ULL, fromMask, targetMask);
    stateMachine.ApplyPhysicalOccupancySnapshot(before);

    Observe(stateMachine, 1U, fromMask | targetMask, 0ULL);
    const runtime::TaggedInteractionState& airborne =
        stateMachine.GetChessInteractionModel().phase;
    Check(airborne.tag == runtime::ChessInteractionPhase::ExecutingPlan,
          orderingLabel);
    Check(airborne.plan.kind == runtime::PhysicalTransitionKind::Capture,
          "same-scan normal capture keeps capture identity");
    Check(airborne.plan.moveFrom == from && airborne.plan.moveTo == target,
          "same-scan normal capture keeps canonical endpoints");
    Check(airborne.plan.progress.removalComplete,
          "same-scan normal capture records target removal");
    Check(!stateMachine.HasHumanIllegal(),
          "same-scan normal capture has no ordering-dependent correction");

    Observe(stateMachine, 2U, targetMask, targetMask);
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Settling,
          "same-scan normal capture accepts mover placement");
}

void TestAtomicEnPassant(const uint8_t from,
                         const uint8_t target,
                         const uint8_t captured,
                         const char* orderingLabel) {
    const uint64_t fromMask = 1ULL << from;
    const uint64_t targetMask = 1ULL << target;
    const uint64_t capturedMask = 1ULL << captured;
    const uint64_t before = fromMask | capturedMask;

    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    stateMachine.HandleInteractionPositionAcknowledged(
        before,
        0ULL,
        0ULL,
        fromMask,
        capturedMask,
        before,
        target,
        captured);
    stateMachine.ApplyPhysicalOccupancySnapshot(before);

    Observe(stateMachine, 1U, fromMask | capturedMask, 0ULL);
    const runtime::TaggedInteractionState& airborne =
        stateMachine.GetChessInteractionModel().phase;
    Check(airborne.tag == runtime::ChessInteractionPhase::ExecutingPlan,
          orderingLabel);
    Check(airborne.plan.kind == runtime::PhysicalTransitionKind::EnPassant,
          "same-scan en-passant keeps special-move identity");
    Check(airborne.plan.moveFrom == from && airborne.plan.moveTo == target &&
              airborne.plan.removalSquare == captured,
          "same-scan en-passant keeps metadata-derived squares");
    Check(airborne.plan.progress.removalComplete,
          "same-scan en-passant records captured pawn removal");
    Check(!stateMachine.HasHumanIllegal(),
          "same-scan en-passant has no square-order correction");

    Observe(stateMachine, 2U, targetMask, targetMask);
    Check(stateMachine.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Settling,
          "same-scan en-passant accepts target placement");
}

} // namespace

int main() {
    std::puts("[ATOMIC_CAPTURE_ORDERING] start");

    TestAtomicNormalCapture(
        36U, 27U,
        "normal capture works when target index sorts before source");
    TestAtomicNormalCapture(
        28U, 35U,
        "normal capture works when source index sorts before target");

    TestAtomicEnPassant(
        28U, 19U, 27U,
        "en-passant works when captured pawn sorts before source");
    TestAtomicEnPassant(
        35U, 44U, 36U,
        "en-passant works when source sorts before captured pawn");

    if (g_failures == 0) {
        std::puts("[ATOMIC_CAPTURE_ORDERING] ok");
        return 0;
    }
    std::printf("[ATOMIC_CAPTURE_ORDERING] failures=%d\n", g_failures);
    return 1;
}
