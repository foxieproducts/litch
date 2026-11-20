#include <cstdio>

#include "runtime/chess_interaction_model.hpp"
#include "runtime/invariant_checks.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[INVARIANTS] start");

    runtime::InvariantReport<8> report;
    runtime::CheckSquareIndexMappingStable(report);

    Check(report.IsOk(), "square mapping invariants violated");

    runtime::ChessInteractionModel interaction;
    runtime::ResetChessInteractionModel(interaction);
    runtime::CheckChessInteractionModelInvariants(interaction, report);
    Check(report.IsOk(), "default interaction model violates invariants");

    constexpr uint64_t KING_FROM = (1ULL << 60);
    constexpr uint64_t ROOK_FROM = (1ULL << 63);
    constexpr uint64_t CASTLE_FROM = KING_FROM | ROOK_FROM;
    constexpr uint64_t CASTLE_TO = (1ULL << 62) | (1ULL << 61);

    runtime::ReduceChessInteraction(
        interaction,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            CASTLE_FROM,
            KING_FROM,
            ROOK_FROM));
    runtime::ReduceChessInteraction(
        interaction,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            10,
            1,
            0,
            CASTLE_FROM));
    runtime::ReduceChessInteraction(
        interaction,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            20,
            2,
            CASTLE_FROM,
            0));

    Check(interaction.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan,
          "same-scan castle pickup starts one tagged plan phase");
    Check(interaction.phase.plan.kind == runtime::PhysicalTransitionKind::Castling,
          "same-scan castle pickup builds a castling plan");
    Check(interaction.phase.plan.progress.relocationsLiftedMask == 0x03U,
          "same-scan castle pickup records both relocation lifts");
    Check(interaction.board.timestampMs == 20 && interaction.board.sequence == 2,
          "model preserves atomic observation time and sequence");
    runtime::CheckChessInteractionModelInvariants(interaction, report);
    Check(report.IsOk(), "active castle plan violates invariants");

    runtime::ReduceChessInteraction(
        interaction,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            30,
            3,
            CASTLE_TO,
            CASTLE_TO));
    Check(interaction.phase.tag == runtime::ChessInteractionPhase::AwaitingAction,
          "physical castle completion waits for an acknowledged action");
    Check(interaction.acknowledged.occupiedMask == CASTLE_FROM,
          "physical completion does not advance acknowledged position");
    Check(interaction.phase.pendingActionToken != 0U,
          "physical completion exposes one pending apply-move action");

    const uint32_t actionToken = interaction.phase.pendingActionToken;
    runtime::ReduceChessInteraction(
        interaction,
        runtime::ChessInteractionEvent::MakeActionResult(actionToken, true));
    Check(interaction.phase.tag == runtime::ChessInteractionPhase::Idle,
          "successful acknowledgement completes the plan");
    Check(interaction.acknowledged.occupiedMask == CASTLE_TO,
          "successful acknowledgement advances acknowledged position");
    const runtime::ChessInteractionModel interactionCopy = interaction;
    Check(runtime::ComputeChessInteractionModelHash(interaction) ==
              runtime::ComputeChessInteractionModelHash(interactionCopy),
          "model hash is stable across value copies");

    runtime::ChessInteractionModel rookFirst;
    runtime::ResetChessInteractionModel(rookFirst);
    runtime::ReduceChessInteraction(
        rookFirst,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            CASTLE_FROM,
            KING_FROM,
            ROOK_FROM));
    runtime::ReduceChessInteraction(
        rookFirst,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            40, 1, 0, CASTLE_FROM));
    runtime::ReduceChessInteraction(
        rookFirst,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            50, 2, ROOK_FROM, KING_FROM));
    runtime::ReduceChessInteraction(
        rookFirst,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            60, 3, KING_FROM, 0));
    Check(rookFirst.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan,
          "separately observed rook-first enters a tagged plan");
    Check(rookFirst.phase.plan.kind ==
              runtime::PhysicalTransitionKind::Castling,
          "rook-first upgrades to the canonical castle plan");
    Check(rookFirst.phase.pendingActionToken == 0U,
          "rook-first castle does not expose an early apply action");
    runtime::CheckChessInteractionModelInvariants(rookFirst, report);
    Check(report.IsOk(), "rook-first castle plan violates invariants");

    const uint32_t faultEpoch = rookFirst.inputFaultEpoch;
    runtime::ReduceChessInteraction(
        rookFirst,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            70, 3, 0, 0));
    Check(rookFirst.inputFaultEpoch == faultEpoch + 1,
          "non-monotonic observation sequence is rejected deterministically");

    runtime::ChessInteractionModel partialCastle;
    runtime::ResetChessInteractionModel(partialCastle);
    runtime::ReduceChessInteraction(
        partialCastle,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            CASTLE_FROM,
            KING_FROM,
            ROOK_FROM));
    runtime::ReduceChessInteraction(
        partialCastle,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            80, 1, 0, CASTLE_FROM));
    runtime::ReduceChessInteraction(
        partialCastle,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            90, 2, KING_FROM, ROOK_FROM));
    runtime::ReduceChessInteraction(
        partialCastle,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            100, 3, (1ULL << 62), ROOK_FROM | (1ULL << 62)));
    Check(partialCastle.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan,
          "king relocation retains incomplete castle plan");
    Check(partialCastle.phase.pendingActionToken == 0U,
          "king relocation does not expose an early castle action");
    Check(partialCastle.phase.requiredAction ==
              runtime::RequiredPhysicalAction::LiftSecondary,
          "partial castle requires the rook relocation next");
    Check(partialCastle.acknowledged.occupiedMask == CASTLE_FROM,
          "partial castle retains acknowledged pre-move position");
    runtime::ReduceChessInteraction(
        partialCastle,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            110, 4, ROOK_FROM, (1ULL << 62)));
    runtime::ReduceChessInteraction(
        partialCastle,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            120, 5, (1ULL << 61), CASTLE_TO));
    Check(partialCastle.phase.tag == runtime::ChessInteractionPhase::AwaitingAction,
          "complete physical castle awaits engine acknowledgement");
    Check(partialCastle.phase.pendingActionToken != 0U,
          "complete physical castle exposes exactly one engine action");
    const uint32_t partialToken = partialCastle.phase.pendingActionToken;
    runtime::ReduceChessInteraction(
        partialCastle,
        runtime::ChessInteractionEvent::MakeActionResult(partialToken, true));
    Check(partialCastle.phase.tag == runtime::ChessInteractionPhase::Idle,
          "castle completes after its engine acknowledgement");
    Check(partialCastle.acknowledged.occupiedMask == CASTLE_TO,
          "castle advances logical position only after acknowledgement");

    runtime::ChessInteractionModel invalid;
    runtime::ResetChessInteractionModel(invalid);
    invalid.phase.plan.kind = runtime::PhysicalTransitionKind::Castling;
    runtime::InvariantReport<8> invalidReport;
    runtime::CheckChessInteractionModelInvariants(invalid, invalidReport);
    Check(!invalidReport.IsOk(), "idle phase with a live plan fails invariants");

    if (g_failures == 0) {
        std::puts("[INVARIANTS] ok");
        return 0;
    }

    std::printf("[INVARIANTS] failures=%d\n", g_failures);
    return 1;
}
