#include <cstdio>

#include "foxie_embedded/millis.hpp"
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

runtime::ChessInteractionDurableState MakeSettlingState() {
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 50U;
    runtime::GameStateMachine stateMachine(config);
    stateMachine.SetExpectedOccupancyMask(1ULL << 4U);
    stateMachine.DebugSetPhysicalOccupancyMask(1ULL << 4U);
    stateMachine.HandleSensorChange(4U, false);
    stateMachine.SetValidDestMask(1ULL << 12U);
    stateMachine.HandleSensorChange(12U, true);
    runtime::ChessInteractionDurableState durable;
    Check(runtime::ExportChessInteractionDurableState(
              stateMachine.GetChessInteractionModel(), durable),
          "export human settling phase");
    return durable;
}

runtime::ChessInteractionDurableState MakeEnPassantSettlingState() {
    constexpr uint8_t FROM = 28U;
    constexpr uint8_t DESTINATION = 19U;
    constexpr uint8_t CAPTURE = 27U;
    runtime::GameStateMachine stateMachine;
    runtime::ChessPositionState position;
    position.occupiedMask = (1ULL << FROM) | (1ULL << CAPTURE);
    position.pawnMask = position.occupiedMask;
    position.moverOccupiedMask = 1ULL << FROM;
    position.opponentOccupiedMask = 1ULL << CAPTURE;
    position.enPassantTargetSquare = DESTINATION;
    position.enPassantCaptureSquare = CAPTURE;
    stateMachine.DebugSetExpectedPositionModel(position);
    stateMachine.DebugSetPhysicalOccupancyMask(position.occupiedMask);
    stateMachine.HandleSensorChange(FROM, false);
    stateMachine.HandleSensorChange(DESTINATION, true);
    stateMachine.HandleSensorChange(CAPTURE, false);
    runtime::ChessInteractionDurableState durable;
    Check(runtime::ExportChessInteractionDurableState(
              stateMachine.GetChessInteractionModel(), durable),
          "export en-passant settling phase");
    return durable;
}

runtime::GameStateMachine RestoreSettling(
    const runtime::ChessInteractionDurableState& durable,
    const bool requireO) {
    runtime::GameStateMachineConfig config;
    config.settleDurationMs = 50U;
    runtime::GameStateMachine stateMachine(config);
    stateMachine.SetMoveConfirmPolicy(50U, requireO);
    stateMachine.SetExpectedOccupancyMask(1ULL << 4U);
    Check(stateMachine.RestoreInteractionDurableState(durable),
          "restore human settling phase");
    stateMachine.ApplyPhysicalOccupancySnapshot(1ULL << 12U);
    if (!stateMachine.IsPhysicalOccupancyConsistentWithInteraction()) {
        (void)stateMachine.ForceEnterPlacementMode();
    }
    Check(!stateMachine.IsPlacementModeActive(),
          "matching boot scan preserves restored settle");
    return stateMachine;
}

} // namespace

int main() {
    std::puts("[DURABLE_SETTLE_RESTORE] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    const runtime::ChessInteractionDurableState durable = MakeSettlingState();

    runtime::GameStateMachine timed = RestoreSettling(durable, false);
    Check(timed.IsMoveSettlingActive(),
          "timed restore reconstructs settle ownership");
    Check(timed.GetSettleSquare() == 12U,
          "timed restore reconstructs settle render target");
    fxe::ManualMillis::AdvanceMs(49U);
    timed.Tick();
    Check(timed.IsMoveSettlingActive(),
          "timed restored settle remains active before deadline");
    fxe::ManualMillis::AdvanceMs(1U);
    timed.Tick();
    Check(timed.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "timed restored settle exposes action at bounded deadline");

    runtime::GameStateMachine confirm = RestoreSettling(durable, true);
    fxe::ManualMillis::AdvanceMs(10000U);
    confirm.Tick();
    Check(confirm.IsMoveSettlingActive(),
          "O-confirm restored settle does not auto-finalize");
    confirm.HandleButtonEvent(modules::UserButton::P1O,
                              fxe::Button::Event::Press);
    uint32_t elapsedMs = 0U;
    uint32_t durationMs = 0U;
    Check(confirm.TryGetSettleElapsedAndDurationMs(elapsedMs, durationMs) &&
              durationMs == 1000U,
          "O-confirm restored settle starts bounded confirmation window");
    fxe::ManualMillis::AdvanceMs(1000U);
    confirm.Tick();
    Check(confirm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "O-confirm restored settle exposes action after confirmation");

    runtime::GameStateMachine relifted;
    relifted.SetExpectedOccupancyMask(1ULL << 4U);
    Check(relifted.RestoreInteractionDurableState(durable),
          "restore quiet settle for divergent scan");
    relifted.ApplyPhysicalOccupancySnapshot(0ULL);
    Check(relifted.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::PieceLifted,
          "boot scan with quiet destination absent reopens pickup placement");
    Check(!relifted.IsMoveSettlingActive() &&
              !relifted.HasCommittedMovePending(),
          "reopened quiet boot scan cannot expose an action");
    relifted.HandleSensorChange(12U, true);
    Check(relifted.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Settling,
          "replacing quiet destination starts a fresh settle after boot");

    runtime::GameStateMachine reverted;
    reverted.SetExpectedOccupancyMask(1ULL << 4U);
    Check(reverted.RestoreInteractionDurableState(durable),
          "restore quiet settle for full physical revert");
    reverted.ApplyPhysicalOccupancySnapshot(1ULL << 4U);
    Check(reverted.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Idle &&
              !reverted.IsMoveSettlingActive(),
          "fully reverted boot scan clears the uncommitted move to Idle");

    constexpr uint8_t EP_DESTINATION = 19U;
    constexpr uint8_t EP_CAPTURE = 27U;
    constexpr uint8_t EP_FROM = 28U;
    const runtime::ChessInteractionDurableState enPassantDurable =
        MakeEnPassantSettlingState();
    runtime::GameStateMachine ep;
    runtime::ChessPositionState epPosition;
    epPosition.occupiedMask = (1ULL << EP_FROM) | (1ULL << EP_CAPTURE);
    epPosition.pawnMask = epPosition.occupiedMask;
    epPosition.moverOccupiedMask = 1ULL << EP_FROM;
    epPosition.opponentOccupiedMask = 1ULL << EP_CAPTURE;
    epPosition.enPassantTargetSquare = EP_DESTINATION;
    epPosition.enPassantCaptureSquare = EP_CAPTURE;
    ep.DebugSetExpectedPositionModel(epPosition);
    Check(ep.RestoreInteractionDurableState(enPassantDurable),
          "restore en-passant settle for divergent scan");
    ep.ApplyPhysicalOccupancySnapshot(1ULL << EP_CAPTURE);
    Check(ep.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::ExecutingPlan &&
              ep.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::RemoveCapture,
          "boot scan recomputes restored en-passant removal progress");
    Check(!ep.IsMoveSettlingActive() && !ep.HasCommittedMovePending(),
          "incomplete restored en-passant cannot expose an action");
    ep.HandleSensorChange(EP_CAPTURE, false);
    Check(ep.GetChessInteractionModel().phase.requiredAction ==
              runtime::RequiredPhysicalAction::PlacePrimary,
          "restored en-passant prompts mover placement after capture removal");
    ep.HandleSensorChange(EP_DESTINATION, true);
    Check(ep.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::Settling,
          "restored en-passant completion starts a fresh settle");

    if (g_failures == 0) {
        std::puts("[DURABLE_SETTLE_RESTORE] ok");
        return 0;
    }
    std::printf("[DURABLE_SETTLE_RESTORE] failures=%d\n", g_failures);
    return 1;
}
