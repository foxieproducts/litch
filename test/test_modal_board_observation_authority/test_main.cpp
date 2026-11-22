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

void CheckSnapshotOnlyObservation(runtime::GameStateMachine& stateMachine,
                                  const uint32_t sequence,
                                  const uint64_t before,
                                  const uint64_t changed,
                                  const uint64_t after,
                                  const char* phaseMessage,
                                  const char* snapshotMessage) {
    stateMachine.ApplyPhysicalOccupancySnapshot(before);
    const runtime::TaggedInteractionState phaseBefore =
        stateMachine.GetChessInteractionModel().phase;
    stateMachine.HandleBoardObservation(changed,
                                        after,
                                        sequence * 10U,
                                        sequence);
    const runtime::ChessInteractionModel& model =
        stateMachine.GetChessInteractionModel();
    Check(model.phase.tag == phaseBefore.tag &&
              model.phase.plan.kind == phaseBefore.plan.kind &&
              model.phase.pendingActionToken ==
                  phaseBefore.pendingActionToken,
          phaseMessage);
    Check(model.board.occupiedMask == after &&
              model.board.changedMask == changed &&
              model.board.sequence == sequence,
          snapshotMessage);
}

void TestPlacementOwnsObservation() {
    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    Check(stateMachine.ForceEnterPlacementMode(),
          "force placement modal starts");
    CheckSnapshotOnlyObservation(
        stateMachine,
        11U,
        1ULL << 12U,
        1ULL << 12U,
        0ULL,
        "placement observation does not start tagged pickup",
        "placement observation updates canonical physical snapshot");
    Check(stateMachine.IsPlacementModeActive(),
          "placement modal remains active after observation");
}

void TestKingQueenPromptOwnsObservation() {
    constexpr uint64_t PROMPT_MASK =
        (1ULL << 3U) | (1ULL << 4U) |
        (1ULL << 59U) | (1ULL << 60U);
    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    Check(stateMachine.StartKingQueenSwapPrompt(PROMPT_MASK),
          "king-queen prompt starts");
    CheckSnapshotOnlyObservation(
        stateMachine,
        12U,
        PROMPT_MASK,
        1ULL << 3U,
        PROMPT_MASK & ~(1ULL << 3U),
        "king-queen observation does not start tagged pickup",
        "king-queen observation updates canonical physical snapshot");
    Check(stateMachine.IsKingQueenSwapPromptActive(),
          "king-queen prompt remains active during choreography");
}

void TestPromotionOwnsObservation() {
    runtime::GameStateMachine stateMachine;
    stateMachine.Reset();
    Check(stateMachine.StartPromotion(), "promotion modal starts");
    CheckSnapshotOnlyObservation(
        stateMachine,
        13U,
        1ULL << 8U,
        1ULL << 8U,
        0ULL,
        "promotion observation does not start tagged pickup",
        "promotion observation updates canonical physical snapshot");
    Check(stateMachine.IsPromotionActive(),
          "promotion modal remains active after observation");
}

} // namespace

int main() {
    std::puts("[MODAL_BOARD_OBSERVATION_AUTHORITY] start");
    TestPlacementOwnsObservation();
    TestKingQueenPromptOwnsObservation();
    TestPromotionOwnsObservation();
    if (g_failures == 0) {
        std::puts("[MODAL_BOARD_OBSERVATION_AUTHORITY] ok");
        return 0;
    }
    std::printf("[MODAL_BOARD_OBSERVATION_AUTHORITY] failures=%d\n",
                g_failures);
    return 1;
}
