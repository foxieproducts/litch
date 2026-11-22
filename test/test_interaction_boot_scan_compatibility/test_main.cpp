#include <cstdio>

#include "runtime/chess_interaction_persistence.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

bool BootPreserves(const runtime::ChessInteractionDurableState& durable,
                   const uint64_t before,
                   const uint64_t mover,
                   const uint64_t opponent,
                   const uint64_t legalTargets,
                   const uint64_t scanned,
                   const runtime::ChessInteractionPhase expectedPhase) {
    runtime::GameStateMachine stateMachine;
    stateMachine.SetExpectedOccupancyMask(before);
    stateMachine.SetExpectedMoverAndOpponentOccupancyMasks(mover, opponent);
    stateMachine.SetLegalCaptureTargetsMask(legalTargets, true);
    if (!stateMachine.RestoreInteractionDurableState(durable)) {
        return false;
    }
    stateMachine.ApplyPhysicalOccupancySnapshot(scanned);
    if (!stateMachine.IsPhysicalOccupancyConsistentWithInteraction()) {
        (void)stateMachine.ForceEnterPlacementMode();
    }
    return !stateMachine.IsPlacementModeActive() &&
           stateMachine.GetChessInteractionModel().phase.tag == expectedPhase;
}

} // namespace

int main() {
    std::puts("[INTERACTION_BOOT_SCAN_COMPATIBILITY] start");
#ifndef ARDUINO
    constexpr uint8_t MOVER = 4U;
    constexpr uint8_t TARGET = 20U;
    constexpr uint64_t MOVER_MASK = 1ULL << MOVER;
    constexpr uint64_t TARGET_MASK = 1ULL << TARGET;
    constexpr uint64_t BEFORE = MOVER_MASK | TARGET_MASK;

    runtime::GameStateMachine pieceLifted;
    pieceLifted.SetExpectedOccupancyMask(BEFORE);
    pieceLifted.SetExpectedMoverAndOpponentOccupancyMasks(
        MOVER_MASK, TARGET_MASK);
    pieceLifted.DebugSetPhysicalOccupancyMask(BEFORE);
    pieceLifted.HandleSensorChange(MOVER, false);
    runtime::ChessInteractionDurableState liftedDurable;
    Check(runtime::ExportChessInteractionDurableState(
              pieceLifted.GetChessInteractionModel(), liftedDurable),
          "export PieceLifted boot state");
    Check(BootPreserves(liftedDurable,
                        BEFORE,
                        MOVER_MASK,
                        TARGET_MASK,
                        TARGET_MASK,
                        BEFORE & ~MOVER_MASK,
                        runtime::ChessInteractionPhase::PieceLifted),
          "matching production-order scan preserves PieceLifted");

    runtime::GameStateMachine captureStaged;
    captureStaged.SetExpectedOccupancyMask(BEFORE);
    captureStaged.SetExpectedMoverAndOpponentOccupancyMasks(
        MOVER_MASK, TARGET_MASK);
    captureStaged.SetLegalCaptureTargetsMask(TARGET_MASK, true);
    captureStaged.DebugSetPhysicalOccupancyMask(BEFORE);
    captureStaged.HandleSensorChange(TARGET, false);
    runtime::ChessInteractionDurableState stagedDurable;
    Check(runtime::ExportChessInteractionDurableState(
              captureStaged.GetChessInteractionModel(), stagedDurable),
          "export CaptureStaged boot state");
    Check(BootPreserves(stagedDurable,
                        BEFORE,
                        MOVER_MASK,
                        TARGET_MASK,
                        TARGET_MASK,
                        BEFORE & ~TARGET_MASK,
                        runtime::ChessInteractionPhase::CaptureStaged),
          "matching production-order scan preserves CaptureStaged");

    runtime::GameStateMachine captureSettling;
    captureSettling.SetExpectedOccupancyMask(BEFORE);
    captureSettling.SetExpectedMoverAndOpponentOccupancyMasks(
        MOVER_MASK, TARGET_MASK);
    captureSettling.SetLegalCaptureTargetsMask(TARGET_MASK, true);
    captureSettling.DebugSetPhysicalOccupancyMask(BEFORE);
    captureSettling.HandleSensorChange(TARGET, false);
    captureSettling.HandleSensorChange(MOVER, false);
    captureSettling.SetValidDestMask(TARGET_MASK);
    captureSettling.HandleSensorChange(TARGET, true);
    runtime::ChessInteractionDurableState captureDurable;
    Check(runtime::ExportChessInteractionDurableState(
              captureSettling.GetChessInteractionModel(), captureDurable),
          "export completed capture settle boot state");
    Check(BootPreserves(captureDurable,
                        BEFORE,
                        MOVER_MASK,
                        TARGET_MASK,
                        TARGET_MASK,
                        TARGET_MASK,
                        runtime::ChessInteractionPhase::Settling),
          "capture destination placement wins durable removal projection");

    runtime::GameStateMachine divergent;
    divergent.SetExpectedOccupancyMask(BEFORE);
    Check(divergent.RestoreInteractionDurableState(liftedDurable),
          "restore divergent boot state");
    divergent.ApplyPhysicalOccupancySnapshot(BEFORE | (1ULL << 30U));
    if (!divergent.IsPhysicalOccupancyConsistentWithInteraction()) {
        (void)divergent.ForceEnterPlacementMode();
    }
    Check(divergent.IsPlacementModeActive(),
          "truly divergent production-order scan enters placement");
#endif

    if (g_failures == 0) {
        std::puts("[INTERACTION_BOOT_SCAN_COMPATIBILITY] ok");
        return 0;
    }
    std::printf("[INTERACTION_BOOT_SCAN_COMPATIBILITY] failures=%d\n",
                g_failures);
    return 1;
}
