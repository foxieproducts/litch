#include <cstdio>
#include <cstring>

#include "runtime/chess_interaction_model.hpp"
#include "runtime/chess_interaction_persistence.hpp"

namespace {

int g_failures = 0;

constexpr uint8_t WHITE_PIECE = 60U;
constexpr uint8_t BLACK_PIECE = 4U;
constexpr uint8_t DUCK = 24U;
constexpr uint8_t DESTINATION_A = 32U;
constexpr uint8_t DESTINATION_B = 40U;
constexpr uint64_t WHITE_MASK = 1ULL << WHITE_PIECE;
constexpr uint64_t BLACK_MASK = 1ULL << BLACK_PIECE;
constexpr uint64_t DUCK_MASK = 1ULL << DUCK;
constexpr uint64_t DESTINATION_A_MASK = 1ULL << DESTINATION_A;
constexpr uint64_t DESTINATION_B_MASK = 1ULL << DESTINATION_B;
constexpr uint64_t BEFORE = WHITE_MASK | BLACK_MASK | DUCK_MASK;
constexpr uint64_t LIFTED = BEFORE & ~DUCK_MASK;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

void CheckInvariants(const runtime::ChessInteractionModel& model,
                     const char* message) {
    runtime::InvariantReport<8> report;
    runtime::CheckChessInteractionModelInvariants(model, report);
    Check(report.IsOk(), message);
}

void AcknowledgePosition(runtime::ChessInteractionModel& model) {
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            BEFORE,
            0ULL,
            0ULL,
            WHITE_MASK,
            BLACK_MASK));
}

void Seed(runtime::ChessInteractionModel& model) {
    runtime::ResetChessInteractionModel(model);
    AcknowledgePosition(model);
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            10U, 1U, 0ULL, BEFORE));
}

void Observe(runtime::ChessInteractionModel& model,
             const uint32_t sequence,
             const uint64_t occupiedMask) {
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            sequence * 10U,
            sequence,
            model.board.occupiedMask ^ occupiedMask,
            occupiedMask));
}

void TestSelectableRelocationAndAcknowledgement() {
    runtime::ChessInteractionModel model;
    Seed(model);

    Check(runtime::BeginDuckRelocationPlan(
              model, DUCK, runtime::CHESS_INTERACTION_INVALID_SQUARE),
          "selectable duck relocation begins from neutral occupied square");
    Check(model.phase.tag == runtime::ChessInteractionPhase::DuckRelocation &&
              model.phase.requiredAction ==
                  runtime::RequiredPhysicalAction::LiftPrimary,
          "duck relocation starts by requesting the duck lift");
    Check(model.phase.plan.beforeOccupiedMask == BEFORE &&
              model.phase.plan.afterOccupiedMask == BEFORE,
          "unplaced duck plan retains authoritative physical occupancy");
    Check(runtime::ProjectPhysicalOccupancyForInteractionProgress(
              model.acknowledged, model.phase) == BEFORE,
          "fresh relocation projects the duck at its committed source");
    AcknowledgePosition(model);
    Check(model.phase.tag == runtime::ChessInteractionPhase::DuckRelocation,
          "equivalent position publication preserves active relocation");
    CheckInvariants(model, "fresh selectable relocation satisfies invariants");

    Observe(model, 2U, LIFTED);
    Check(model.phase.requiredAction ==
              runtime::RequiredPhysicalAction::PlacePrimary &&
              model.phase.plan.progress.relocationsLiftedMask == 1U,
          "lifting the duck requests a selectable empty destination");
    Check(runtime::ProjectPhysicalOccupancyForInteractionProgress(
              model.acknowledged, model.phase) == LIFTED,
          "in-hand duck projection excludes only the neutral blocker");

    Observe(model, 3U, BEFORE);
    Check(model.phase.tag == runtime::ChessInteractionPhase::DuckRelocation &&
              model.phase.requiredAction ==
                  runtime::RequiredPhysicalAction::LiftPrimary &&
              model.phase.pendingActionToken == 0U,
          "returning the duck to its source safely restarts relocation");

    const uint64_t after = LIFTED | DESTINATION_A_MASK;
    Observe(model, 4U, after);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Settling &&
              model.phase.pendingActionToken == 0U &&
              model.phase.plan.moveTo == DESTINATION_A &&
              model.phase.plan.afterOccupiedMask == after,
          "atomic selectable relocation records its physical destination");
    char settlingEncoded[1024] = {};
    runtime::ChessInteractionDurableState settlingDecoded;
    runtime::ChessInteractionModel settlingRestored;
    Seed(settlingRestored);
    Check(runtime::EncodeChessInteractionDurableState(model, settlingEncoded, sizeof(settlingEncoded)) &&
              runtime::DecodeChessInteractionDurableState(settlingEncoded, settlingDecoded) &&
              runtime::RestoreChessInteractionDurableState(settlingRestored, settlingDecoded) &&
              settlingRestored.phase.tag == runtime::ChessInteractionPhase::Settling &&
              settlingRestored.phase.pendingActionToken == 0U,
          "unconfirmed Duck placement round-trips without creating an action token");
    CheckInvariants(model, "unconfirmed Duck placement satisfies invariants");
    runtime::ReduceChessInteraction(model, runtime::ChessInteractionEvent::MakeSettleConfirmed());
    Check(model.phase.pendingActionToken != 0U &&
              runtime::CanSafelyPersistChessInteractionState(model),
          "completed relocation exposes one durable action token");
    CheckInvariants(model, "completed selectable relocation satisfies invariants");

    const uint64_t chessMasksBefore =
        model.acknowledged.moverOccupiedMask |
        model.acknowledged.opponentOccupiedMask;
    const uint32_t token = model.phase.pendingActionToken;
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeActionResult(token, true));
    Check(model.phase.tag == runtime::ChessInteractionPhase::Idle &&
              model.acknowledged.occupiedMask == after,
          "successful action acknowledges relocated physical occupancy");
    Check((model.acknowledged.moverOccupiedMask |
           model.acknowledged.opponentOccupiedMask) == chessMasksBefore &&
              ((model.acknowledged.moverOccupiedMask |
                model.acknowledged.opponentOccupiedMask) &
               DESTINATION_A_MASK) == 0ULL,
          "duck relocation does not add the neutral blocker to chess masks");
    CheckInvariants(model, "acknowledged selectable relocation satisfies invariants");
}

void TestFixedDestinationCorrectionAndRollback() {
    runtime::ChessInteractionModel model;
    Seed(model);

    const uint32_t faultBefore = model.inputFaultEpoch;
    Check(!runtime::BeginDuckRelocationPlan(model, DUCK, DUCK),
          "fixed destination cannot equal the duck source");
    Check(!runtime::BeginDuckRelocationPlan(model, DUCK, WHITE_PIECE),
          "fixed destination cannot overwrite a chess piece");
    Check(!runtime::BeginDuckRelocationPlan(
              model, WHITE_PIECE, DESTINATION_A),
          "typed chess occupancy cannot be classified as the duck");
    Check(model.phase.tag == runtime::ChessInteractionPhase::Idle &&
              model.inputFaultEpoch == faultBefore + 3U,
          "rejected relocation requests leave the reducer idle");

    Check(runtime::BeginDuckRelocationPlan(
              model, DUCK, DESTINATION_A),
          "fixed duck relocation begins for undo choreography");
    Observe(model, 2U, BEFORE & ~WHITE_MASK);
    Check(model.phase.tag == runtime::ChessInteractionPhase::DuckRelocation &&
              model.phase.requiredAction ==
                  runtime::RequiredPhysicalAction::CorrectBoard &&
              model.phase.correctionMask == WHITE_MASK,
          "unrelated chess-piece lift enters duck-owned correction");
    Observe(model, 3U, BEFORE);
    Check(model.phase.requiredAction ==
              runtime::RequiredPhysicalAction::LiftPrimary,
          "restoring an unrelated piece returns to the duck source step");

    Observe(model, 4U, LIFTED);
    Observe(model, 5U, LIFTED | DESTINATION_B_MASK);
    Check(model.phase.requiredAction ==
              runtime::RequiredPhysicalAction::CorrectBoard &&
              model.phase.correctionMask == DESTINATION_B_MASK &&
              model.phase.selectedSquare == DESTINATION_A,
          "wrong fixed destination requests removal and retains target");
    Observe(model, 6U, LIFTED);
    Check(model.phase.requiredAction ==
              runtime::RequiredPhysicalAction::PlacePrimary &&
              model.phase.plan.moveTo == DESTINATION_A,
          "removing a wrongly placed duck resumes the fixed destination");

    const uint64_t after = LIFTED | DESTINATION_A_MASK;
    Observe(model, 7U, after);
    const uint32_t token = model.phase.pendingActionToken;
    Check(token != 0U,
          "fixed relocation completion exposes an action token");
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeActionResult(token, false));
    Check(model.phase.tag == runtime::ChessInteractionPhase::Correction &&
              model.phase.correctionMask == (DUCK_MASK | DESTINATION_A_MASK),
          "failed fixed action requests rollback to acknowledged source");
    Check(runtime::CanSafelyPersistChessInteractionState(model),
          "failed-action rollback is a durable correction boundary");
    CheckInvariants(model, "failed fixed action satisfies invariants");

    Observe(model, 8U, BEFORE);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Idle &&
              model.acknowledged.occupiedMask == BEFORE,
          "returning duck to source completes failed-action rollback");
}

void TestReliftInvalidatesPendingToken() {
    runtime::ChessInteractionModel model;
    Seed(model);
    Check(runtime::BeginDuckRelocationPlan(
              model, DUCK, runtime::CHESS_INTERACTION_INVALID_SQUARE),
          "token regression fixture begins");

    Observe(model, 2U, LIFTED | DESTINATION_A_MASK);
    runtime::ReduceChessInteraction(model, runtime::ChessInteractionEvent::MakeSettleConfirmed());
    const uint32_t firstToken = model.phase.pendingActionToken;
    Observe(model, 3U, LIFTED);
    Check(model.phase.tag == runtime::ChessInteractionPhase::DuckRelocation &&
              model.phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary &&
              model.phase.pendingActionToken == 0U &&
              model.phase.plan.moveTo ==
                  runtime::CHESS_INTERACTION_INVALID_SQUARE,
          "relifting before acknowledgement revokes token and destination");

    Observe(model, 4U, LIFTED | DESTINATION_B_MASK);
    runtime::ReduceChessInteraction(model, runtime::ChessInteractionEvent::MakeSettleConfirmed());
    const uint32_t secondToken = model.phase.pendingActionToken;
    Check(secondToken != 0U && secondToken != firstToken &&
              model.phase.plan.moveTo == DESTINATION_B,
          "replacement placement receives a fresh token and destination");
    const uint32_t faultBefore = model.inputFaultEpoch;
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeActionResult(firstToken, true));
    Check(model.inputFaultEpoch == faultBefore + 1U &&
              model.phase.pendingActionToken == secondToken &&
              model.acknowledged.occupiedMask == BEFORE,
          "revoked action token cannot acknowledge stale duck placement");
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeActionResult(secondToken, true));
    Check(model.phase.tag == runtime::ChessInteractionPhase::Idle &&
              model.acknowledged.occupiedMask ==
                  (LIFTED | DESTINATION_B_MASK),
          "fresh replacement token acknowledges the final destination");
}

void TestDurableV3AndRebootRecovery() {
    runtime::ChessInteractionModel model;
    Seed(model);
    Check(runtime::BeginDuckRelocationPlan(
              model, DUCK, DESTINATION_A),
          "durable fixed relocation begins");
    Observe(model, 2U, LIFTED);

    char encoded[128] = {};
    Check(runtime::EncodeChessInteractionDurableState(
              model, encoded, sizeof(encoded)),
          "in-hand duck relocation encodes durably");
    Check(encoded[0] == '0' && encoded[1] == '3',
          "new duck interaction state writes schema version three");

    runtime::ChessInteractionDurableState decoded;
    Check(runtime::DecodeChessInteractionDurableState(encoded, decoded),
          "version-three in-hand duck relocation decodes");
    runtime::ChessInteractionModel restored;
    runtime::ResetChessInteractionModel(restored);
    AcknowledgePosition(restored);
    Check(runtime::RestoreChessInteractionDurableState(restored, decoded),
          "in-hand duck relocation restores across reboot");
    Check(restored.phase.tag ==
              runtime::ChessInteractionPhase::DuckRelocation &&
              restored.phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary &&
              restored.phase.selectedSquare == DESTINATION_A &&
              restored.board.valid && restored.board.occupiedMask == LIFTED,
          "reboot restore retains fixed destination and physical projection");
    CheckInvariants(restored, "restored in-hand relocation satisfies invariants");

    runtime::ReduceChessInteraction(
        restored,
        runtime::ChessInteractionEvent::MakePlanProgressObserved(
            LIFTED | DESTINATION_A_MASK));
    const uint32_t token = restored.phase.pendingActionToken;
    Check(restored.phase.tag == runtime::ChessInteractionPhase::AwaitingAction &&
              token != 0U,
          "boot scan can complete restored duck placement");
    Check(runtime::EncodeChessInteractionDurableState(
              restored, encoded, sizeof(encoded)),
          "awaiting duck action encodes durably");
    Check(runtime::DecodeChessInteractionDurableState(encoded, decoded),
          "awaiting duck action decodes durably");

    runtime::ChessInteractionModel awaitingRestore;
    runtime::ResetChessInteractionModel(awaitingRestore);
    AcknowledgePosition(awaitingRestore);
    Check(runtime::RestoreChessInteractionDurableState(
              awaitingRestore, decoded),
          "awaiting duck action restores across reboot");
    Check(awaitingRestore.phase.pendingActionToken == token &&
              awaitingRestore.board.occupiedMask ==
                  (LIFTED | DESTINATION_A_MASK),
          "reboot preserves action token and completed physical projection");
    runtime::ReduceChessInteraction(
        awaitingRestore,
        runtime::ChessInteractionEvent::MakeActionResult(token, true));
    Check(awaitingRestore.phase.tag == runtime::ChessInteractionPhase::Idle &&
              awaitingRestore.acknowledged.occupiedMask ==
                  (LIFTED | DESTINATION_A_MASK),
          "restored action token commits relocation exactly once");

    char versionTwo[128] = {};
    std::strcpy(versionTwo, encoded);
    versionTwo[0] = '0';
    versionTwo[1] = '2';
    Check(!runtime::DecodeChessInteractionDurableState(
              versionTwo, decoded),
          "version two cannot reinterpret a duck phase or plan kind");

    char versionOne[128] = {};
    std::memcpy(versionOne, encoded, 44U);
    versionOne[0] = '0';
    versionOne[1] = '1';
    versionOne[44] = '0';
    versionOne[45] = '0';
    std::strcpy(versionOne + 46U, encoded + 44U);
    Check(!runtime::DecodeChessInteractionDurableState(
              versionOne, decoded),
          "version one cannot reinterpret a duck phase or plan kind");
}

void TestCpuActionReadyDurabilityRetainsFixedDestination() {
    runtime::ChessInteractionModel model;
    Seed(model);
    Check(runtime::BeginDuckRelocationPlan(
              model, DUCK, DESTINATION_A, runtime::PhysicalTransitionSource::Cpu),
          "CPU action-ready fixture starts through the fixed relocation builder");
    Observe(model, 2U, LIFTED);
    const uint64_t after = LIFTED | DESTINATION_A_MASK;
    Observe(model, 3U, after);
    const uint32_t originalToken = model.phase.pendingActionToken;
    Check(model.phase.tag == runtime::ChessInteractionPhase::AwaitingAction &&
              originalToken != 0U && runtime::CanSafelyPersistChessInteractionState(model),
          "CPU Duck AwaitingAction is an explicit safe durable boundary");

    char encoded[128] = {};
    const bool encodedCpu = runtime::EncodeChessInteractionDurableState(
        model, encoded, sizeof(encoded));
    Check(encodedCpu, "action-ready CPU Duck state encodes without losing source or fixed target");
    if (!encodedCpu) {
        return;
    }
    const auto rejectsMissingFixedTarget = [](const char* source) {
        char missingTarget[128] = {};
        std::strcpy(missingTarget, source);
        // Version 3 starts with version, phase, selectedSquare. Mutate only
        // the fixed-target marker; moveTo and complete progress remain valid.
        constexpr size_t SELECTED_SQUARE_HEX_OFFSET = 4U;
        missingTarget[SELECTED_SQUARE_HEX_OFFSET] = 'f';
        missingTarget[SELECTED_SQUARE_HEX_OFFSET + 1U] = 'f';
        runtime::ChessInteractionDurableState corrupt;
        return !runtime::DecodeChessInteractionDurableState(missingTarget, corrupt);
    };
    const bool rejectsAwaiting = rejectsMissingFixedTarget(encoded);
    Check(rejectsAwaiting,
          "CPU action-ready payload rejects an absent fixed-target marker despite a valid moveTo");

    runtime::ChessInteractionDurableState decoded;
    runtime::ChessInteractionModel restored;
    Seed(restored);
    const bool restoredCpu = runtime::DecodeChessInteractionDurableState(encoded, decoded) &&
        runtime::RestoreChessInteractionDurableState(restored, decoded);
    Check(restoredCpu, "CPU action-ready payload restores through the existing validated reducer boundary");
    if (!restoredCpu) {
        return;
    }
    Observe(restored, 2U, after);
    Check(restored.phase.pendingActionToken == originalToken &&
              restored.phase.plan.source == runtime::PhysicalTransitionSource::Cpu &&
              restored.phase.selectedSquare == DESTINATION_A &&
              restored.board.occupiedMask == after &&
              restored.acknowledged.occupiedMask == BEFORE,
          "first matching boot scan retains the original CPU token and uncommitted physical projection");
    Observe(restored, 3U, LIFTED);
    const bool retainedFixed =
        restored.phase.tag == runtime::ChessInteractionPhase::DuckRelocation &&
        restored.phase.requiredAction == runtime::RequiredPhysicalAction::PlacePrimary &&
        restored.phase.plan.source == runtime::PhysicalTransitionSource::Cpu &&
        restored.phase.selectedSquare == DESTINATION_A &&
        restored.phase.plan.moveTo == DESTINATION_A &&
        restored.phase.pendingActionToken == 0U;
    Check(retainedFixed,
          "destination lift after restore revokes the token while preserving the exact fixed CPU target");
    CheckInvariants(restored, "restored CPU destination lift satisfies interaction invariants");

    Observe(restored, 4U, LIFTED | DESTINATION_B_MASK);
    Check(restored.phase.requiredAction == runtime::RequiredPhysicalAction::CorrectBoard &&
              restored.phase.correctionMask == DESTINATION_B_MASK &&
              restored.phase.selectedSquare == DESTINATION_A,
          "a different post-restore destination is rejected rather than becoming a selectable CPU move");
    Observe(restored, 5U, LIFTED);
    Observe(restored, 6U, after);
    const uint32_t replacementToken = restored.phase.pendingActionToken;
    const uint32_t faultsBefore = restored.inputFaultEpoch;
    runtime::ReduceChessInteraction(
        restored, runtime::ChessInteractionEvent::MakeActionResult(originalToken, true));
    Check(replacementToken != 0U && replacementToken != originalToken &&
              restored.phase.pendingActionToken == replacementToken &&
              restored.inputFaultEpoch == faultsBefore + 1U &&
              restored.acknowledged.occupiedMask == BEFORE,
          "the original pre-reboot token cannot acknowledge a replacement placement");
    runtime::ReduceChessInteraction(
        restored, runtime::ChessInteractionEvent::MakeActionResult(replacementToken, true));
    Check(restored.phase.tag == runtime::ChessInteractionPhase::Idle &&
              restored.acknowledged.occupiedMask == after,
          "only the replacement token commits the original fixed CPU destination");
    CheckInvariants(restored, "completed restored CPU relocation satisfies interaction invariants");

    runtime::ReduceChessInteraction(
        model, runtime::ChessInteractionEvent::MakeActionResult(originalToken, false));
    const bool correctionEncoded =
        model.phase.tag == runtime::ChessInteractionPhase::Correction &&
        runtime::CanSafelyPersistChessInteractionState(model) &&
        runtime::EncodeChessInteractionDurableState(model, encoded, sizeof(encoded));
    Check(correctionEncoded, "failed CPU Duck action retains a valid durable rollback plan");
    const bool rejectsCorrection = correctionEncoded && rejectsMissingFixedTarget(encoded);
    Check(rejectsCorrection, "CPU action-correction payload also rejects a lost fixed-target marker");
    std::printf("[CPU_DUCK_ACTION_READY_DURABLE] encoded=%u restored=%u "
                "fixed_after_lift=%u reject_missing_awaiting=%u reject_missing_correction=%u "
                "original_token=%u replacement_token=%u\n",
                encodedCpu, restoredCpu, retainedFixed, rejectsAwaiting, rejectsCorrection,
                originalToken, replacementToken);
}

} // namespace

int main() {
    TestSelectableRelocationAndAcknowledgement();
    TestFixedDestinationCorrectionAndRollback();
    TestReliftInvalidatesPendingToken();
    TestDurableV3AndRebootRecovery();
    TestCpuActionReadyDurabilityRetainsFixedDestination();

    if (g_failures == 0) {
        std::printf("[PASS] duck interaction reducer tests\n");
        return 0;
    }
    std::printf("[FAIL] %d duck interaction reducer assertion(s)\n",
                g_failures);
    return 1;
}
