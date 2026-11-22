#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

void Observe(runtime::GameStateMachine& stateMachine,
             uint32_t& sequence,
             const uint64_t changed,
             const uint64_t occupied) {
    stateMachine.HandleBoardObservation(
        changed, occupied, fxe::Millis(), sequence++);
}

void AcknowledgeUndo(runtime::GameStateMachine& stateMachine) {
    const auto& phase = stateMachine.GetChessInteractionModel().phase;
    Check(phase.tag == runtime::ChessInteractionPhase::AwaitingAction &&
              phase.pendingActionToken != 0U,
          "undo exposes one reducer action token");
    if (phase.pendingActionToken != 0U) {
        stateMachine.HandleInteractionActionResult(
            phase.pendingActionToken, true);
    }
}

void TestUndoPromptAndCancellation() {
    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    stateMachine.SetExpectedOccupancyMask(1ULL << 28U);
    stateMachine.DebugSetPhysicalOccupancyMask(1ULL << 28U);

    Check(stateMachine.BeginUndoPrompt(
              12U, 28U, modules::PersistedSide::White,
              'P', '\0', 0U),
          "coordinator descriptor starts undo prompt");
    Check(stateMachine.IsUndoPromptActive(),
          "reducer Undo plan is prompt authority");
    uint8_t from = 0U;
    uint8_t to = 0U;
    Check(stateMachine.TryGetUndoPromptMove(from, to) &&
              from == 12U && to == 28U,
          "prompt identity derives from reducer plan");

    stateMachine.CancelUndoPrompt(true);
    Check(!stateMachine.IsUndoPromptActive(),
          "semantic cancellation clears reducer plan");
    Check(stateMachine.GetRenderState().moveTransition.kind ==
              runtime::MoveRenderTransitionKind::CancelUndo,
          "semantic cancellation records explicit render transition");
    stateMachine.ClearMoveRenderTransition();
    Check(stateMachine.GetRenderState().moveTransition.kind ==
              runtime::MoveRenderTransitionKind::None,
          "coordinator acknowledgement clears render transition");
}

void TestUndoPromptRejectsUnrelatedChanges() {
    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    const uint64_t forward = (1ULL << 7U) | (1ULL << 28U);
    stateMachine.SetExpectedOccupancyMask(forward);
    stateMachine.DebugSetPhysicalOccupancyMask(forward);
    Check(stateMachine.BeginUndoPrompt(
              12U, 28U, modules::PersistedSide::White,
              'P', '\0', 0U),
          "unrelated-change undo starts");
    AcknowledgeUndo(stateMachine);

    uint32_t sequence = 1U;
    uint64_t physical = forward & ~(1ULL << 7U);
    Observe(stateMachine, sequence, 1ULL << 7U, physical);
    Check(stateMachine.IsUndoPromptActive(),
          "unrelated pickup keeps undo prompt");
    Check((stateMachine.GetHumanIllegalMask() & (1ULL << 7U)) != 0ULL,
          "unrelated pickup receives local illegal feedback");

    physical &= ~(1ULL << 28U);
    Observe(stateMachine, sequence, 1ULL << 28U, physical);
    physical |= 1ULL << 12U;
    Observe(stateMachine, sequence, 1ULL << 12U, physical);
    Check(stateMachine.IsUndoPromptActive(),
          "required relocation cannot bypass unrelated correction");

    physical |= 1ULL << 7U;
    Observe(stateMachine, sequence, 1ULL << 7U, physical);
    Check(!stateMachine.IsUndoPromptActive(),
          "prompt completes after unrelated square is restored");
}

void TestCaptureAndCastlingRequirements() {
    {
        runtime::GameStateMachine stateMachine;
        stateMachine.Reset();
        stateMachine.SetExpectedOccupancyMask(1ULL << 28U);
        stateMachine.DebugSetPhysicalOccupancyMask(1ULL << 28U);
        Check(stateMachine.BeginUndoPrompt(
                  12U, 28U, modules::PersistedSide::White,
                  'P', 'p', 28U),
              "capture undo starts with restoration metadata");
        AcknowledgeUndo(stateMachine);
        uint32_t sequence = 1U;
        uint64_t physical = 0ULL;
        Observe(stateMachine, sequence, 1ULL << 28U, physical);
        physical |= 1ULL << 12U;
        Observe(stateMachine, sequence, 1ULL << 12U, physical);
        Check(stateMachine.IsUndoPromptActive(),
              "capture undo waits for captured piece restoration");
        physical |= 1ULL << 28U;
        Observe(stateMachine, sequence, 1ULL << 28U, physical);
        Check(!stateMachine.IsUndoPromptActive(),
              "capture undo completes after restoration");
        Check(stateMachine.GetHumanIllegalMask() == 0ULL,
              "legal capture restoration completes without a latched illegal marker");
    }

    {
        runtime::GameStateMachine stateMachine;
        stateMachine.Reset();
        const uint64_t castled = (1ULL << 61U) | (1ULL << 62U);
        stateMachine.SetExpectedOccupancyMask(castled);
        stateMachine.DebugSetPhysicalOccupancyMask(castled);
        Check(stateMachine.BeginUndoPrompt(
                  60U, 62U, modules::PersistedSide::White,
                  'K', '\0', 0U),
              "castling undo starts");
        AcknowledgeUndo(stateMachine);
        Check(stateMachine.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::LiftPrimary,
              "castling undo starts by prompting the king pickup");
        uint32_t sequence = 1U;
        uint64_t physical = castled & ~(1ULL << 62U);
        Observe(stateMachine, sequence, 1ULL << 62U, physical);
        Check(stateMachine.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary,
              "castling undo prompts immediate king return after pickup");
        physical |= 1ULL << 60U;
        Observe(stateMachine, sequence, 1ULL << 60U, physical);
        Check(stateMachine.IsUndoPromptActive(),
              "castling undo waits for rook");
        Check(stateMachine.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::LiftSecondary,
              "castling undo prompts rook pickup after king return");
        physical &= ~(1ULL << 61U);
        Observe(stateMachine, sequence, 1ULL << 61U, physical);
        Check(stateMachine.GetChessInteractionModel().phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlaceSecondary,
              "castling undo prompts rook return after pickup");
        physical |= 1ULL << 63U;
        Observe(stateMachine, sequence, 1ULL << 63U, physical);
        Check(!stateMachine.IsUndoPromptActive(),
              "castling undo completes after king and rook return");
        Check(stateMachine.GetHumanIllegalMask() == 0ULL,
              "legal castling return completes without a latched illegal marker");
    }
}

} // namespace

int main() {
    std::puts("[GAME_STATE_MACHINE_UNDO] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);

    TestUndoPromptAndCancellation();
    TestUndoPromptRejectsUnrelatedChanges();
    TestCaptureAndCastlingRequirements();

    if (g_failures == 0) {
        std::puts("[GAME_STATE_MACHINE_UNDO] ok");
        return 0;
    }
    std::printf("[GAME_STATE_MACHINE_UNDO] failures=%d\n", g_failures);
    return 1;
}
