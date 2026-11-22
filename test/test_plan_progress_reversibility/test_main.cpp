#include <cstdio>

#include "runtime/chess_interaction_model.hpp"

namespace {

int gFailures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++gFailures;
    }
}

void Observe(runtime::ChessInteractionModel& model,
             const uint32_t sequence,
             const uint64_t changed,
             const uint64_t occupied) {
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            sequence, sequence, changed, occupied));
}

void TestCastleSourceRestore() {
    constexpr uint64_t KING = 1ULL << 60U;
    constexpr uint64_t ROOK = 1ULL << 63U;
    runtime::ChessInteractionModel model;
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            KING | ROOK, KING, ROOK, KING | ROOK, 0ULL));
    Observe(model, 1U, 0ULL, KING | ROOK);
    Observe(model, 2U, KING | ROOK, 0ULL);
    Observe(model, 3U, KING, KING);
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::Castling &&
              model.phase.requiredAction ==
                  runtime::RequiredPhysicalAction::LiftPrimary,
          "restoring a castle source reopens its lift prompt");
    Check((model.phase.plan.progress.relocationsLiftedMask & 0x01U) == 0U,
          "castle source lift progress reflects current occupancy");
}

void TestCpuSourceRestore() {
    constexpr uint8_t FROM = 12U;
    constexpr uint8_t TO = 28U;
    runtime::ChessInteractionModel model;
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            1ULL << FROM, 0ULL, 0ULL, 0ULL, 1ULL << FROM));
    runtime::PhysicalTransitionPlan plan;
    plan.kind = runtime::PhysicalTransitionKind::QuietMove;
    plan.source = runtime::PhysicalTransitionSource::Cpu;
    plan.beforeOccupiedMask = 1ULL << FROM;
    plan.afterOccupiedMask = 1ULL << TO;
    plan.moveFrom = FROM;
    plan.moveTo = TO;
    plan.relocationCount = 1U;
    plan.relocations[0] = {FROM, TO};
    runtime::ReduceChessInteraction(
        model, runtime::ChessInteractionEvent::MakePlanRequested(plan));
    Observe(model, 1U, 1ULL << FROM, 0ULL);
    Check(model.phase.requiredAction ==
              runtime::RequiredPhysicalAction::PlacePrimary,
          "CPU source removal advances to placement");
    Observe(model, 2U, 1ULL << FROM, 1ULL << FROM);
    Check(model.phase.requiredAction ==
              runtime::RequiredPhysicalAction::LiftPrimary,
          "CPU source restoration returns to lift prompt");
}

void TestUndoRestoreSharesMoverSource() {
    constexpr uint8_t CAPTURE_SQUARE = 43U;
    constexpr uint8_t ORIGINAL_SOURCE = 52U;
    runtime::ChessInteractionModel model;
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            1ULL << CAPTURE_SQUARE,
            0ULL,
            0ULL,
            1ULL << CAPTURE_SQUARE,
            0ULL));
    runtime::PhysicalTransitionPlan undo;
    undo.kind = runtime::PhysicalTransitionKind::Undo;
    undo.source = runtime::PhysicalTransitionSource::Undo;
    undo.beforeOccupiedMask = 1ULL << CAPTURE_SQUARE;
    undo.afterOccupiedMask = (1ULL << ORIGINAL_SOURCE) |
                             (1ULL << CAPTURE_SQUARE);
    undo.moveFrom = ORIGINAL_SOURCE;
    undo.moveTo = CAPTURE_SQUARE;
    undo.relocationCount = 1U;
    undo.relocations[0] = {CAPTURE_SQUARE, ORIGINAL_SOURCE};
    undo.restorationRequired = true;
    undo.restorationSquare = CAPTURE_SQUARE;
    undo.restorationPieceFen = 'p';
    runtime::ReduceChessInteraction(
        model, runtime::ChessInteractionEvent::MakePlanRequested(undo));
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePlanProgressObserved(0ULL));
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePlanProgressObserved(
            undo.afterOccupiedMask));
    const runtime::PhysicalTransitionProgress& progress =
        model.phase.plan.progress;
    Check((progress.relocationsLiftedMask & 0x01U) != 0U &&
              (progress.relocationsPlacedMask & 0x01U) != 0U &&
              progress.restorationComplete,
          "one restore observation completes undo when capture shares mover source");
}

} // namespace

int main() {
    std::puts("[PLAN_PROGRESS_REVERSIBILITY] start");
    TestCastleSourceRestore();
    TestCpuSourceRestore();
    TestUndoRestoreSharesMoverSource();
    if (gFailures == 0) {
        std::puts("[PLAN_PROGRESS_REVERSIBILITY] ok");
        return 0;
    }
    std::printf("[PLAN_PROGRESS_REVERSIBILITY] failures=%d\n", gFailures);
    return 1;
}
