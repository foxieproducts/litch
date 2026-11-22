#include <cstdio>

#include "runtime/chess_interaction_model.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

void TestCpuCastleRequiresWholePlan() {
    constexpr uint8_t KING_FROM = 60U;
    constexpr uint8_t KING_TO = 62U;
    constexpr uint8_t ROOK_FROM = 63U;
    constexpr uint8_t ROOK_TO = 61U;
    constexpr uint64_t BEFORE = (1ULL << KING_FROM) | (1ULL << ROOK_FROM);
    constexpr uint64_t AFTER = (1ULL << KING_TO) | (1ULL << ROOK_TO);

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            BEFORE, 1ULL << KING_FROM, 1ULL << ROOK_FROM, BEFORE, 0));

    runtime::PhysicalTransitionPlan plan;
    plan.kind = runtime::PhysicalTransitionKind::Castling;
    plan.source = runtime::PhysicalTransitionSource::Cpu;
    plan.beforeOccupiedMask = BEFORE;
    plan.afterOccupiedMask = AFTER;
    plan.moveFrom = KING_FROM;
    plan.moveTo = KING_TO;
    plan.relocationCount = 2U;
    plan.relocations[0] = {KING_FROM, KING_TO};
    plan.relocations[1] = {ROOK_FROM, ROOK_TO};
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePlanRequested(plan));
    Check(model.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan,
          "CPU request enters common executing-plan phase");
    Check(model.phase.plan.source == runtime::PhysicalTransitionSource::Cpu,
          "CPU source is retained in common plan");

    const uint64_t kingLifted = BEFORE & ~(1ULL << KING_FROM);
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            10U, 1U, 1ULL << KING_FROM, kingLifted));
    Check(model.phase.requiredAction ==
              runtime::RequiredPhysicalAction::PlacePrimary,
          "CPU castle returns the king before prompting the rook");

    const uint64_t kingPlaced = kingLifted | (1ULL << KING_TO);
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            20U, 2U, 1ULL << KING_TO, kingPlaced));
    Check(model.phase.pendingActionToken == 0U,
          "CPU castle cannot apply after only the king relocation");
    Check(model.phase.requiredAction ==
              runtime::RequiredPhysicalAction::LiftSecondary,
          "CPU castle explicitly prompts the rook only after king placement");

    const uint64_t rookLifted = kingPlaced & ~(1ULL << ROOK_FROM);
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            25U, 3U, 1ULL << ROOK_FROM, rookLifted));
    Check(model.phase.requiredAction ==
              runtime::RequiredPhysicalAction::PlaceSecondary,
          "CPU castle prompts the rook destination after rook pickup");

    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            30U, 4U, 1ULL << ROOK_TO, AFTER));
    Check(model.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
              model.phase.requiredAction ==
                  runtime::RequiredPhysicalAction::None,
          "whole CPU castle remains unclaimable until stability is confirmed");
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeSettleConfirmed());
    Check(model.phase.tag == runtime::ChessInteractionPhase::AwaitingAction &&
              model.phase.pendingActionToken != 0U,
          "stable CPU castle exposes exactly one pending engine action");

    const uint32_t token = model.phase.pendingActionToken;
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeActionResult(token, true));
    Check(model.phase.tag == runtime::ChessInteractionPhase::Idle,
          "CPU castle acknowledgement completes common plan");
    Check(model.acknowledged.occupiedMask == AFTER,
          "CPU castle acknowledgement advances logical occupancy");
}

void TestUndoIsAcknowledgedInversePlan() {
    constexpr uint8_t ORIGINAL_FROM = 36U;
    constexpr uint8_t CAPTURED_AT = 27U;
    constexpr uint64_t BEFORE = 1ULL << CAPTURED_AT;
    constexpr uint64_t AFTER = (1ULL << ORIGINAL_FROM) | (1ULL << CAPTURED_AT);

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            BEFORE, 0, 0, 0, BEFORE, BEFORE));

    runtime::PhysicalTransitionPlan undo;
    undo.kind = runtime::PhysicalTransitionKind::Undo;
    undo.source = runtime::PhysicalTransitionSource::Undo;
    undo.beforeOccupiedMask = BEFORE;
    undo.afterOccupiedMask = 1ULL << ORIGINAL_FROM;
    undo.moveFrom = ORIGINAL_FROM;
    undo.moveTo = CAPTURED_AT;
    undo.relocationCount = 1U;
    undo.relocations[0] = {CAPTURED_AT, ORIGINAL_FROM};
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePlanRequested(undo));
    Check(model.phase.tag == runtime::ChessInteractionPhase::AwaitingAction,
          "undo requests engine rewind before physical execution");
    Check(model.phase.pendingActionToken != 0U,
          "undo exposes exactly one pending adapter action");

    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeUndoRestorationConfigured(
            CAPTURED_AT, 'p'));
    Check(model.phase.plan.afterOccupiedMask == AFTER,
          "capture undo adds restored piece to inverse target");
    const uint32_t token = model.phase.pendingActionToken;
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeActionResult(token, true));
    Check(model.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan,
          "engine-acknowledged undo waits for physical inverse");
    Check(model.acknowledged.occupiedMask == AFTER,
          "undo acknowledgement advances logical inverse position");

    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            10U, 1U, 1ULL << CAPTURED_AT, 0));
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            20U, 2U, 1ULL << ORIGINAL_FROM, 1ULL << ORIGINAL_FROM));
    Check(model.phase.requiredAction ==
              runtime::RequiredPhysicalAction::RestoreCapture,
          "capture undo explicitly requires restored captured piece");
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            30U, 3U, 1ULL << CAPTURED_AT, AFTER));
    Check(model.phase.tag == runtime::ChessInteractionPhase::Idle,
          "physical inverse completion clears undo plan");
}

void TestCpuCapturePromotionRetainsBothRequirements() {
    constexpr uint8_t FROM = 9U;
    constexpr uint8_t TO = 0U;
    constexpr uint64_t BEFORE = (1ULL << FROM) | (1ULL << TO);

    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    stateMachine.HandleInteractionPositionAcknowledged(
        BEFORE,
        0ULL,
        0ULL,
        BEFORE,
        0ULL,
        1ULL << FROM);
    Check(stateMachine.QueueCpuMove(FROM, TO, true, TO, 'q'),
          "CPU capture promotion queues");

    const runtime::PhysicalTransitionPlan& plan =
        stateMachine.GetChessInteractionModel().phase.plan;
    Check(plan.kind == runtime::PhysicalTransitionKind::Promotion &&
              plan.promotionRequired,
          "CPU capture promotion keeps promotion identity");
    Check(plan.removalRequired && plan.removalSquare == TO,
          "CPU capture promotion keeps captured-square requirement");
}

} // namespace

int main() {
    std::puts("[CHESS_INTERACTION_CPU_PLANS] start");
    TestCpuCastleRequiresWholePlan();
    TestUndoIsAcknowledgedInversePlan();
    TestCpuCapturePromotionRetainsBothRequirements();
    if (g_failures == 0) {
        std::puts("[CHESS_INTERACTION_CPU_PLANS] ok");
        return 0;
    }
    std::printf("[CHESS_INTERACTION_CPU_PLANS] failures=%d\n", g_failures);
    return 1;
}
