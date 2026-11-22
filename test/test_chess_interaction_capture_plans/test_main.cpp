#include <cstdio>

#include "runtime/chess_interaction_model.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

void Acknowledge(runtime::ChessInteractionModel& model,
                 const uint64_t occupied,
                 const uint64_t pawns,
                 const uint64_t mover,
                 const uint64_t opponent,
                 const uint8_t epTarget = runtime::CHESS_INTERACTION_INVALID_SQUARE,
                 const uint8_t epCapture = runtime::CHESS_INTERACTION_INVALID_SQUARE) {
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            occupied, 0, 0, mover, opponent, pawns, epTarget, epCapture));
}

void Observe(runtime::ChessInteractionModel& model,
             const uint32_t sequence,
             const uint64_t changed,
             const uint64_t occupied) {
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeBoardObservation(
            sequence * 10U, sequence, changed, occupied));
}

void ConfirmSettle(runtime::ChessInteractionModel& model) {
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeSettleConfirmed());
}

void TestMoverFirstCapture() {
    constexpr uint8_t FROM = 52U;
    constexpr uint8_t TO = 43U;
    constexpr uint64_t FROM_MASK = 1ULL << FROM;
    constexpr uint64_t TO_MASK = 1ULL << TO;
    constexpr uint64_t BEFORE = FROM_MASK | TO_MASK;

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    Acknowledge(model, BEFORE, 0, FROM_MASK, TO_MASK);
    Observe(model, 1, 0, BEFORE);
    Observe(model, 2, FROM_MASK, TO_MASK);
    Observe(model, 3, TO_MASK, 0);

    Check(model.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan,
          "mover-first capture waits for mover placement");
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::Capture,
          "mover-first capture builds capture plan");
    Check(model.phase.plan.removalRequired && model.phase.plan.removalSquare == TO,
          "capture plan records destination removal");
    Check(model.phase.pendingActionToken == 0U,
          "capture exposes no action before mover placement");

    Observe(model, 4, TO_MASK, TO_MASK);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Settling,
          "completed capture awaits settle confirmation");
    Check(model.phase.pendingActionToken == 0U,
          "completed capture exposes no action before settle");
    Check(model.phase.plan.afterOccupiedMask == TO_MASK,
          "capture plan owns final occupancy");

    Observe(model, 5, TO_MASK, 0ULL);
    Check(model.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
              model.phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary,
          "capture destination relift reopens mover placement");
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::Capture &&
              model.phase.plan.progress.removalComplete,
          "capture destination relift retains removal history");
    Observe(model, 6, TO_MASK, TO_MASK);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Settling,
          "replacing captured destination starts a fresh settle");

    ConfirmSettle(model);
    const uint32_t token = model.phase.pendingActionToken;
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakeActionResult(token, false));
    Check(model.phase.tag == runtime::ChessInteractionPhase::Correction,
          "rejected capture enters explicit correction");
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::Capture &&
              model.phase.plan.progress.removalComplete,
          "rejected capture preserves its completed durable plan progress");
}

void TestTargetFirstCapture() {
    constexpr uint8_t FROM = 52U;
    constexpr uint8_t TO = 43U;
    constexpr uint64_t FROM_MASK = 1ULL << FROM;
    constexpr uint64_t TO_MASK = 1ULL << TO;
    constexpr uint64_t BEFORE = FROM_MASK | TO_MASK;

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            BEFORE, 0, 0, FROM_MASK, TO_MASK, 0,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            true, false, true, TO_MASK));
    Observe(model, 1, 0, BEFORE);
    Observe(model, 2, TO_MASK, FROM_MASK);
    Check(model.phase.tag == runtime::ChessInteractionPhase::CaptureStaged,
          "target-first removal has an explicit tagged phase");
    Check(model.phase.selectedSquare == TO,
          "target-first phase retains captured square");

    Observe(model, 3, FROM_MASK, 0);
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::Capture,
          "target-first source lift creates same capture plan");
    Observe(model, 4, TO_MASK, TO_MASK);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Settling &&
              model.phase.pendingActionToken == 0U,
          "target-first capture waits for settle confirmation");
}

void TestModalCaptureObservationRemainsSnapshotOnly() {
    constexpr uint64_t MOVER = 1ULL << 52U;
    constexpr uint64_t TARGET = 1ULL << 43U;
    constexpr uint64_t EXTRA = 1ULL << 51U;
    constexpr uint64_t BEFORE = MOVER | TARGET | EXTRA;
    runtime::ChessInteractionModel selected;
    Acknowledge(selected, BEFORE, 0ULL, MOVER | EXTRA, TARGET);
    Observe(selected, 1U, 0ULL, BEFORE);
    Observe(selected, 2U, MOVER, TARGET | EXTRA);
    runtime::ChessInteractionModel executing = selected;

    runtime::ReduceChessInteraction(selected,
        runtime::ChessInteractionEvent::MakeModalBoardObservation(
            30U, 3U, TARGET | EXTRA, 0ULL));
    Check(selected.board.sequence == 3U && selected.board.occupiedMask == 0ULL &&
              selected.board.stableSinceMs == 30U &&
              selected.phase.tag == runtime::ChessInteractionPhase::PieceLifted &&
              selected.phase.selectedSquare == 52U &&
              selected.phase.plan.kind == runtime::PhysicalTransitionKind::None &&
              selected.phase.pendingActionToken == 0U && selected.nextActionToken == 1U,
          "default modal observation updates Hall truth without inferring a selected capture");

    Observe(executing, 3U, TARGET, EXTRA);
    runtime::ReduceChessInteraction(executing,
        runtime::ChessInteractionEvent::MakeModalBoardObservation(
            40U, 4U, TARGET, TARGET | EXTRA));
    Check(executing.board.occupiedMask == (TARGET | EXTRA) &&
              executing.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
              executing.phase.requiredAction == runtime::RequiredPhysicalAction::PlacePrimary &&
              executing.phase.plan.progress.removalComplete &&
              executing.phase.plan.progress.relocationsPlacedMask == 0U &&
              executing.phase.pendingActionToken == 0U && executing.nextActionToken == 1U &&
              executing.acknowledged.occupiedMask == BEFORE,
          "default modal landing neither records placement nor completes an existing capture");
}

void TestCaptureProgressObservationAdmission() {
    constexpr uint64_t MOVER = 1ULL << 52U;
    constexpr uint64_t TARGET = 1ULL << 43U;
    constexpr uint64_t EXTRA = 1ULL << 51U;
    constexpr uint64_t BEFORE = MOVER | TARGET | EXTRA;
    runtime::ChessInteractionModel baseline;
    runtime::ReduceChessInteraction(baseline,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            BEFORE, 0ULL, 0ULL, MOVER | EXTRA, TARGET, 0ULL,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            runtime::CHESS_INTERACTION_INVALID_SQUARE, true, false, true, TARGET));
    Observe(baseline, 1U, 0ULL, BEFORE);

    runtime::ChessInteractionModel idle = baseline;
    runtime::ReduceChessInteraction(idle,
        runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            20U, 2U, MOVER | TARGET, EXTRA));
    Check(idle.phase.tag == runtime::ChessInteractionPhase::Idle &&
              idle.phase.plan.kind == runtime::PhysicalTransitionKind::None &&
              idle.board.occupiedMask == EXTRA && idle.nextActionToken == 1U,
          "capture-progress observation cannot select a new mover from Idle");

    runtime::ChessInteractionModel staged = baseline;
    Observe(staged, 2U, TARGET, MOVER | EXTRA);
    runtime::ReduceChessInteraction(staged,
        runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            30U, 3U, MOVER, EXTRA));
    Check(staged.phase.tag == runtime::ChessInteractionPhase::CaptureStaged &&
              staged.phase.selectedSquare == 43U &&
              staged.phase.plan.kind == runtime::PhysicalTransitionKind::None &&
              staged.phase.pendingActionToken == 0U,
          "capture-progress observation cannot select the capturer from target-first staging");

    runtime::ChessInteractionModel selected = baseline;
    Observe(selected, 2U, MOVER, TARGET | EXTRA);
    runtime::ChessInteractionModel sourcePresent = selected;
    runtime::ReduceChessInteraction(sourcePresent,
        runtime::ChessInteractionEvent::MakeModalBoardObservation(
            30U, 3U, MOVER, BEFORE));
    runtime::ReduceChessInteraction(sourcePresent,
        runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            40U, 4U, TARGET, MOVER | EXTRA));
    Check(sourcePresent.phase.tag == runtime::ChessInteractionPhase::PieceLifted &&
              sourcePresent.phase.selectedSquare == 52U &&
              sourcePresent.phase.plan.kind == runtime::PhysicalTransitionKind::None,
          "retained selection cannot infer a capture while its source is physically present");

    runtime::ReduceChessInteraction(selected,
        runtime::ChessInteractionEvent::MakeModalBoardObservation(
            30U, 3U, TARGET, EXTRA));
    runtime::ReduceChessInteraction(selected,
        runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            40U, 4U, EXTRA, 0ULL));
    runtime::ReduceChessInteraction(selected,
        runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            50U, 5U, 0ULL, 0ULL));
    Check(selected.phase.tag == runtime::ChessInteractionPhase::PieceLifted &&
              selected.phase.plan.kind == runtime::PhysicalTransitionKind::None &&
              selected.phase.pendingActionToken == 0U && selected.nextActionToken == 1U,
          "an already-empty opponent square and unrelated removal provide no fresh capture proof");

    constexpr uint64_t EP_MOVER = 1ULL << 28U;
    constexpr uint64_t EP_VICTIM = 1ULL << 27U;
    runtime::ChessInteractionModel enPassant;
    Acknowledge(enPassant, EP_MOVER | EP_VICTIM | EXTRA,
        EP_MOVER | EP_VICTIM, EP_MOVER | EXTRA, EP_VICTIM, 19U, 27U);
    Observe(enPassant, 1U, 0ULL, EP_MOVER | EP_VICTIM | EXTRA);
    Observe(enPassant, 2U, EP_MOVER, EP_VICTIM | EXTRA);
    runtime::ReduceChessInteraction(enPassant,
        runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            30U, 3U, EP_VICTIM | EXTRA, 0ULL));
    Check(enPassant.phase.tag == runtime::ChessInteractionPhase::PieceLifted &&
              enPassant.phase.plan.kind == runtime::PhysicalTransitionKind::None &&
              enPassant.phase.pendingActionToken == 0U,
          "human en-passant removal does not begin a plan behind unrelated correction");
}

void TestCaptureProgressDefersCompletionAndResetsProof() {
    constexpr uint64_t MOVER = 1ULL << 52U;
    constexpr uint64_t TARGET = 1ULL << 43U;
    constexpr uint64_t EXTRA = 1ULL << 51U;
    constexpr uint64_t BEFORE = MOVER | TARGET | EXTRA;
    runtime::ChessInteractionModel model;
    Acknowledge(model, BEFORE, 0ULL, MOVER | EXTRA, TARGET);
    Observe(model, 1U, 0ULL, BEFORE);
    Observe(model, 2U, MOVER, TARGET | EXTRA);
    runtime::ReduceChessInteraction(model,
        runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            30U, 3U, TARGET | EXTRA, 0ULL));
    Check(model.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
              model.phase.plan.kind == runtime::PhysicalTransitionKind::Capture &&
              model.phase.plan.moveFrom == 52U && model.phase.plan.moveTo == 43U &&
              model.phase.plan.progress.removalComplete &&
              model.phase.requiredAction == runtime::RequiredPhysicalAction::PlacePrimary &&
              model.phase.pendingActionToken == 0U && model.nextActionToken == 1U,
          "fresh opponent removal records capture progress for the already-held human mover");
    runtime::ReduceChessInteraction(model,
        runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            40U, 4U, TARGET, TARGET));
    Check(model.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
              model.phase.plan.progress.relocationsPlacedMask == 1U &&
              model.phase.requiredAction == runtime::RequiredPhysicalAction::None &&
              model.phase.pendingActionToken == 0U && model.nextActionToken == 1U &&
              model.acknowledged.occupiedMask == BEFORE,
          "capture landing behind correction records facts without settling or allocating an action");

    runtime::ChessInteractionModel stale = model;
    for (const uint32_t sequence : {3U, 4U}) {
        runtime::ChessInteractionModel expected = stale;
        ++expected.inputFaultEpoch;
        runtime::ReduceChessInteraction(stale,
            runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
                100U, sequence, MOVER, MOVER | TARGET));
        Check(runtime::ComputeChessInteractionModelHash(stale) ==
                  runtime::ComputeChessInteractionModelHash(expected),
              "older and duplicate capture observations change only the input-fault epoch");
    }

    runtime::ChessInteractionModel returned = model;
    runtime::ReduceChessInteraction(returned,
        runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            50U, 5U, MOVER, MOVER | TARGET));
    Check(returned.phase.tag == runtime::ChessInteractionPhase::PieceLifted &&
              returned.phase.selectedSquare == 52U &&
              returned.phase.plan.kind == runtime::PhysicalTransitionKind::None &&
              returned.phase.pendingActionToken == 0U && returned.nextActionToken == 1U,
          "human source return behind correction discards the old capture proof");
    runtime::ReduceChessInteraction(returned,
        runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            60U, 6U, MOVER, TARGET));
    Check(returned.phase.tag == runtime::ChessInteractionPhase::PieceLifted &&
              returned.phase.plan.kind == runtime::PhysicalTransitionKind::None,
          "lifting the returned source cannot reuse old victim-removal proof");
    runtime::ReduceChessInteraction(returned,
        runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            70U, 7U, TARGET, 0ULL));
    Check(returned.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
              returned.phase.plan.progress.removalComplete &&
              returned.phase.requiredAction == runtime::RequiredPhysicalAction::PlacePrimary,
          "a fresh victim removal starts the returned mover's new capture attempt");

    runtime::ReduceChessInteraction(model,
        runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
            50U, 5U, EXTRA, TARGET | EXTRA));
    ConfirmSettle(model);
    Check(model.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
              model.phase.pendingActionToken == 0U && model.nextActionToken == 1U,
          "even a corrected capture needs explicit reconciliation before settle confirmation");
    runtime::ReduceChessInteraction(model,
        runtime::ChessInteractionEvent::MakePlanProgressObserved(TARGET | EXTRA));
    Check(model.phase.tag == runtime::ChessInteractionPhase::Settling &&
              model.phase.pendingActionToken == 0U && model.nextActionToken == 1U,
          "explicit plan reconciliation starts human settling without an engine action");
    ConfirmSettle(model);
    Check(model.phase.tag == runtime::ChessInteractionPhase::AwaitingAction &&
              model.phase.pendingActionToken == 1U && model.nextActionToken == 2U &&
              model.acknowledged.occupiedMask == BEFORE,
          "only later settle confirmation allocates the capture's one acknowledged action");
}

void TestAtomicPickupHandoffs() {
    constexpr uint8_t FIRST = 52U;
    constexpr uint8_t SECOND = 51U;
    constexpr uint8_t TARGET = 43U;
    constexpr uint64_t FIRST_MASK = 1ULL << FIRST;
    constexpr uint64_t SECOND_MASK = 1ULL << SECOND;
    constexpr uint64_t TARGET_MASK = 1ULL << TARGET;
    constexpr uint64_t BEFORE = FIRST_MASK | SECOND_MASK | TARGET_MASK;

    runtime::ChessInteractionModel pickup;
    runtime::ResetChessInteractionModel(pickup);
    runtime::ReduceChessInteraction(
        pickup,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            BEFORE, 0, 0, FIRST_MASK | SECOND_MASK, TARGET_MASK));
    Observe(pickup, 1U, 0ULL, BEFORE);
    Observe(pickup, 2U, FIRST_MASK, BEFORE & ~FIRST_MASK);
    Observe(pickup, 3U, FIRST_MASK | SECOND_MASK,
            BEFORE & ~SECOND_MASK);
    Check(pickup.phase.tag == runtime::ChessInteractionPhase::PieceLifted &&
              pickup.phase.selectedSquare == SECOND,
          "atomic source restore hands ownership to the newly lifted mover");

    runtime::ChessInteractionModel staged;
    runtime::ResetChessInteractionModel(staged);
    runtime::ReduceChessInteraction(
        staged,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            BEFORE, 0, 0, FIRST_MASK | SECOND_MASK, TARGET_MASK, 0,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            true, false, true, TARGET_MASK));
    Observe(staged, 1U, 0ULL, BEFORE);
    Observe(staged, 2U, TARGET_MASK, BEFORE & ~TARGET_MASK);
    Observe(staged, 3U, TARGET_MASK | FIRST_MASK,
            BEFORE & ~FIRST_MASK);
    Check(staged.phase.tag == runtime::ChessInteractionPhase::Settling &&
              staged.phase.plan.moveFrom == FIRST &&
              staged.phase.plan.moveTo == TARGET &&
              staged.phase.plan.progress.removalComplete &&
              staged.phase.pendingActionToken == 0U,
          "observed victim removal survives atomic capturer lift and landing");
    // Returning the victim while holding the mover has the same occupancy.
    // Restore the source before confirmation to cancel either interpretation.
    Observe(staged, 4U, FIRST_MASK, BEFORE);
    Check(staged.phase.tag == runtime::ChessInteractionPhase::Idle &&
              staged.nextActionToken == 1U,
          "restoring the source cancels the atomic capture before any action is emitted");
    runtime::ReduceChessInteraction(
        staged, runtime::ChessInteractionEvent::MakeActionResult(99U, true));
    Check(staged.phase.tag == runtime::ChessInteractionPhase::Idle &&
              staged.acknowledged.occupiedMask == BEFORE,
          "an unmatched acknowledgement cannot recommit the canceled capture");

    runtime::ChessInteractionModel ambiguous;
    runtime::ResetChessInteractionModel(ambiguous);
    runtime::ReduceChessInteraction(
        ambiguous,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            BEFORE, 0, 0, FIRST_MASK | SECOND_MASK, TARGET_MASK, 0,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            true, false, true, TARGET_MASK));
    Observe(ambiguous, 1U, 0ULL, BEFORE);
    Observe(ambiguous, 2U, TARGET_MASK, BEFORE & ~TARGET_MASK);
    Observe(ambiguous, 3U, FIRST_MASK | SECOND_MASK,
            BEFORE & ~TARGET_MASK & ~FIRST_MASK & ~SECOND_MASK);
    Check(ambiguous.phase.tag == runtime::ChessInteractionPhase::Correction,
          "staged capture rejects an ambiguous multi-mover pickup");
}

void TestReturnedSourceStagesAnotherCapture() {
    constexpr uint64_t PAWN = 1ULL << 48U; // a2
    constexpr uint64_t ROOK = 1ULL << 59U; // d1
    constexpr uint64_t TARGET = 1ULL << 27U; // d5
    constexpr uint64_t NONCAPTURABLE = 1ULL << 26U; // c5
    constexpr uint64_t BEFORE = PAWN | ROOK | TARGET | NONCAPTURABLE;
    for (const uint64_t removed : {TARGET, NONCAPTURABLE}) {
        runtime::ChessInteractionModel model;
        runtime::ReduceChessInteraction(model,
            runtime::ChessInteractionEvent::MakePositionAcknowledged(
                BEFORE, 0ULL, ROOK, PAWN | ROOK, TARGET | NONCAPTURABLE, PAWN,
                runtime::CHESS_INTERACTION_INVALID_SQUARE,
                runtime::CHESS_INTERACTION_INVALID_SQUARE, true, false, true, TARGET));
        Observe(model, 1U, 0ULL, BEFORE);
        Observe(model, 2U, PAWN, BEFORE & ~PAWN);
        Observe(model, 3U, PAWN | removed, BEFORE & ~removed);
        Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::None &&
                  model.phase.pendingActionToken == 0U && model.nextActionToken == 1U &&
                  model.acknowledged.occupiedMask == BEFORE,
              "atomic source return is consumed before a different victim can create a ghost capture");
        if (removed == NONCAPTURABLE) {
            Check(model.phase.tag == runtime::ChessInteractionPhase::Correction &&
                      model.phase.correctionMask == NONCAPTURABLE &&
                      model.phase.requiredAction == runtime::RequiredPhysicalAction::CorrectBoard,
                  "returned A2 plus noncapturable C5 removal requires only C5 correction");
            Observe(model, 4U, NONCAPTURABLE, BEFORE);
            Check(model.phase.tag == runtime::ChessInteractionPhase::Idle &&
                      model.phase.selectedSquare == runtime::CHESS_INTERACTION_INVALID_SQUARE &&
                      model.nextActionToken == 1U,
                  "returning the noncapturable victim recovers without reviving A2 selection");
            Observe(model, 5U, PAWN, BEFORE & ~PAWN);
            Observe(model, 6U, PAWN | TARGET, BEFORE & ~TARGET);
        }
        Check(model.phase.tag == runtime::ChessInteractionPhase::CaptureStaged &&
                  model.phase.selectedSquare == 27U &&
                  model.phase.requiredAction == runtime::RequiredPhysicalAction::LiftPrimary &&
                  model.phase.plan.kind == runtime::PhysicalTransitionKind::None &&
                  model.phase.pendingActionToken == 0U && model.nextActionToken == 1U,
              "returned A2 plus legal D5 removal stages the victim without retaining an A2 plan");
        Observe(model, 7U, ROOK, BEFORE & ~TARGET & ~ROOK);
        Check(model.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
                  model.phase.plan.moveFrom == 59U && model.phase.plan.moveTo == 27U &&
                  model.phase.plan.progress.removalComplete &&
                  model.phase.requiredAction == runtime::RequiredPhysicalAction::PlacePrimary,
              "fresh D1 pickup owns the staged capture instead of the returned pawn");
        Observe(model, 8U, TARGET, BEFORE & ~ROOK);
        Check(model.phase.tag == runtime::ChessInteractionPhase::Settling &&
                  model.phase.pendingActionToken == 0U && model.nextActionToken == 1U,
              "D1 capture landing retains ordinary unconfirmed settling");
        ConfirmSettle(model);
        Check(model.phase.tag == runtime::ChessInteractionPhase::AwaitingAction &&
                  model.phase.plan.moveFrom == 59U && model.phase.pendingActionToken == 1U &&
                  model.nextActionToken == 2U && model.acknowledged.occupiedMask == BEFORE,
              "only the confirmed rook capture exposes one action token");
    }
}

void TestCaptureFactsSourceReturnCannotSelectAnotherMover() {
    constexpr uint64_t PAWN = 1ULL << 48U; // a2
    constexpr uint64_t ROOK = 1ULL << 59U; // d1
    constexpr uint64_t TARGET = 1ULL << 27U; // d5
    constexpr uint64_t BEFORE = PAWN | ROOK | TARGET;
    for (const bool existingCapture : {false, true}) {
        runtime::ChessInteractionModel model;
        Acknowledge(model, BEFORE, PAWN, PAWN | ROOK, TARGET);
        Observe(model, 1U, 0ULL, BEFORE);
        Observe(model, 2U, PAWN, BEFORE & ~PAWN);
        if (existingCapture) Observe(model, 3U, TARGET, ROOK);
        // Unlike ordinary atomic input, correction owns this observation.
        // Returning A2 while lifting D1 cannot select D1 or stage a new victim.
        runtime::ReduceChessInteraction(model,
            runtime::ChessInteractionEvent::MakeCaptureProgressBoardObservation(
                40U, 4U, model.board.occupiedMask ^ PAWN, PAWN));
        Check(model.board.occupiedMask == PAWN &&
                  model.phase.tag == runtime::ChessInteractionPhase::PieceLifted &&
                  model.phase.selectedSquare == 48U &&
                  model.phase.plan.kind == runtime::PhysicalTransitionKind::None &&
                  model.phase.pendingActionToken == 0U && model.nextActionToken == 1U &&
                  model.acknowledged.occupiedMask == BEFORE,
              "facts-only source return discards capture proof without selecting D1 or staging D5");
        ConfirmSettle(model);
        Check(model.phase.pendingActionToken == 0U && model.nextActionToken == 1U,
              "facts-only source return leaves no ghost capture available for confirmation");
    }
}

void TestRestoredHumanPlansCancel() {
    constexpr uint8_t FROM = 52U;
    constexpr uint8_t TARGET = 43U;
    constexpr uint64_t FROM_MASK = 1ULL << FROM;
    constexpr uint64_t TARGET_MASK = 1ULL << TARGET;
    constexpr uint64_t BEFORE = FROM_MASK | TARGET_MASK;
    runtime::ChessInteractionModel capture;
    Acknowledge(capture, BEFORE, 0ULL, FROM_MASK, TARGET_MASK);
    Observe(capture, 1U, 0ULL, BEFORE);
    Observe(capture, 2U, FROM_MASK, TARGET_MASK);
    Observe(capture, 3U, TARGET_MASK, 0ULL);
    Observe(capture, 4U, BEFORE, BEFORE);
    Check(capture.phase.tag == runtime::ChessInteractionPhase::Idle,
          "fully restored capture clears its uncommitted plan");

    constexpr uint8_t EP_FROM = 28U;
    constexpr uint8_t EP_TARGET = 19U;
    constexpr uint8_t EP_CAPTURE = 27U;
    constexpr uint64_t EP_FROM_MASK = 1ULL << EP_FROM;
    constexpr uint64_t EP_TARGET_MASK = 1ULL << EP_TARGET;
    constexpr uint64_t EP_CAPTURE_MASK = 1ULL << EP_CAPTURE;
    constexpr uint64_t EP_BEFORE = EP_FROM_MASK | EP_CAPTURE_MASK;
    runtime::ChessInteractionModel enPassant;
    Acknowledge(enPassant,
                EP_BEFORE,
                EP_BEFORE,
                EP_FROM_MASK,
                EP_CAPTURE_MASK,
                EP_TARGET,
                EP_CAPTURE);
    Observe(enPassant, 1U, 0ULL, EP_BEFORE);
    Observe(enPassant, 2U, EP_FROM_MASK, EP_CAPTURE_MASK);
    Observe(enPassant, 3U, EP_TARGET_MASK,
            EP_TARGET_MASK | EP_CAPTURE_MASK);
    Observe(enPassant, 4U, EP_FROM_MASK | EP_TARGET_MASK, EP_BEFORE);
    Check(enPassant.phase.tag == runtime::ChessInteractionPhase::Idle,
          "fully restored en-passant clears its uncommitted plan");
}

void TestAtomicEnPassantWithExtraRemovalRejects() {
    constexpr uint8_t FROM = 28U;
    constexpr uint8_t TARGET = 19U;
    constexpr uint8_t CAPTURE = 27U;
    constexpr uint8_t EXTRA = 40U;
    constexpr uint64_t FROM_MASK = 1ULL << FROM;
    constexpr uint64_t TARGET_MASK = 1ULL << TARGET;
    constexpr uint64_t CAPTURE_MASK = 1ULL << CAPTURE;
    constexpr uint64_t EXTRA_MASK = 1ULL << EXTRA;
    constexpr uint64_t BEFORE = FROM_MASK | CAPTURE_MASK | EXTRA_MASK;

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    Acknowledge(model, BEFORE, FROM_MASK | CAPTURE_MASK,
                FROM_MASK | EXTRA_MASK, CAPTURE_MASK, TARGET, CAPTURE);
    Observe(model, 1U, 0ULL, BEFORE);
    Observe(model, 2U, FROM_MASK, BEFORE & ~FROM_MASK);
    const uint64_t completedWithExtraMissing = TARGET_MASK;
    Observe(model, 3U, TARGET_MASK | CAPTURE_MASK | EXTRA_MASK,
            completedWithExtraMissing);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Correction,
          "atomic en-passant plus unrelated removal cannot enter settle");

    // An unrelated opponent pickup must not masquerade as the next turn's
    // source when the EP victim has not been removed yet.
    for (const bool sourceAlreadyLifted : {false, true}) {
        for (const bool victimRemoved : {false, true}) {
            runtime::ResetChessInteractionModel(model);
            Acknowledge(model, BEFORE, FROM_MASK | CAPTURE_MASK,
                        FROM_MASK, CAPTURE_MASK | EXTRA_MASK, TARGET, CAPTURE);
            Observe(model, 1U, 0ULL, BEFORE);
            if (sourceAlreadyLifted) {
                Observe(model, 2U, FROM_MASK, BEFORE & ~FROM_MASK);
            }
            const uint64_t victimChange = victimRemoved ? CAPTURE_MASK : 0ULL;
            const uint64_t changed = TARGET_MASK | EXTRA_MASK | victimChange |
                (sourceAlreadyLifted ? 0ULL : FROM_MASK);
            const uint64_t occupied =
                (BEFORE & ~FROM_MASK & ~EXTRA_MASK & ~victimChange) | TARGET_MASK;
            Observe(model, 3U, changed, occupied);
            Check(model.phase.tag == runtime::ChessInteractionPhase::Correction &&
                      model.phase.requiredAction ==
                          runtime::RequiredPhysicalAction::CorrectBoard &&
                      model.phase.correctionMask == (occupied ^ BEFORE) &&
                      model.phase.pendingActionToken == 0U &&
                      model.acknowledged.occupiedMask == BEFORE,
                  "EP landing with another opponent pickup requires canonical correction before any action");
        }
    }
}

void TestExecutingCaptureRejectsThirdPiece() {
    constexpr uint64_t MOVER = 1ULL << 52U;
    constexpr uint64_t TARGET = 1ULL << 43U;
    constexpr uint64_t THIRD = 1ULL << 51U;
    constexpr uint64_t BEFORE = MOVER | TARGET | THIRD;
    runtime::ChessInteractionModel model;
    Acknowledge(model, BEFORE, 0ULL, MOVER | THIRD, TARGET);
    Observe(model, 1U, 0ULL, BEFORE);
    Observe(model, 2U, MOVER | TARGET, THIRD);
    Check(model.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan,
          "third-piece capture fixture starts an executing plan");
    Observe(model, 3U, THIRD, 0ULL);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Correction,
          "executing capture rejects a third-piece observation");
}

void TestIllegalTargetFirstCaptureRequiresCorrection() {
    constexpr uint8_t FROM = 52U;
    constexpr uint8_t ILLEGAL_TARGET = 43U;
    constexpr uint8_t LEGAL_TARGET = 45U;
    constexpr uint64_t FROM_MASK = 1ULL << FROM;
    constexpr uint64_t ILLEGAL_TARGET_MASK = 1ULL << ILLEGAL_TARGET;
    constexpr uint64_t BEFORE = FROM_MASK | ILLEGAL_TARGET_MASK;

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model,
        runtime::ChessInteractionEvent::MakePositionAcknowledged(
            BEFORE,
            0,
            0,
            FROM_MASK,
            ILLEGAL_TARGET_MASK,
            0,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            runtime::CHESS_INTERACTION_INVALID_SQUARE,
            true,
            false,
            true,
            1ULL << LEGAL_TARGET));
    Observe(model, 1, 0, BEFORE);
    Observe(model,
            2,
            ILLEGAL_TARGET_MASK,
            FROM_MASK);

    Check(model.phase.tag == runtime::ChessInteractionPhase::Correction,
          "illegal target-first removal cannot become durable staging");
    Check(model.phase.correctionMask == ILLEGAL_TARGET_MASK,
          "illegal target-first removal identifies the square to restore");
}

void TestEnPassantBlocksUntilRemoval() {
    constexpr uint8_t FROM = 28U;       // e5
    constexpr uint8_t TARGET = 19U;     // d6
    constexpr uint8_t CAPTURE = 27U;    // d5
    constexpr uint64_t FROM_MASK = 1ULL << FROM;
    constexpr uint64_t TARGET_MASK = 1ULL << TARGET;
    constexpr uint64_t CAPTURE_MASK = 1ULL << CAPTURE;
    constexpr uint64_t BEFORE = FROM_MASK | CAPTURE_MASK;

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    Acknowledge(model,
                BEFORE,
                BEFORE,
                FROM_MASK,
                CAPTURE_MASK,
                TARGET,
                CAPTURE);
    Observe(model, 1, 0, BEFORE);
    Observe(model, 2, FROM_MASK, CAPTURE_MASK);
    Observe(model, 3, TARGET_MASK, TARGET_MASK | CAPTURE_MASK);

    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::EnPassant,
          "empty en-passant target creates en-passant plan");
    Check(model.phase.plan.removalSquare == CAPTURE,
          "en-passant plan derives off-destination removal");
    Check(model.phase.requiredAction == runtime::RequiredPhysicalAction::RemoveCapture,
          "en-passant requires captured pawn removal after mover placement");
    Check(model.phase.pendingActionToken == 0U,
          "en-passant exposes no action while captured pawn remains");

    Observe(model, 4, CAPTURE_MASK, TARGET_MASK);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Settling,
          "en-passant awaits settle after off-destination removal");
    Check(model.phase.pendingActionToken == 0U,
          "completed en-passant exposes no action before settle confirmation");

    Observe(model, 5, CAPTURE_MASK, TARGET_MASK | CAPTURE_MASK);
    Check(model.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
              model.phase.requiredAction ==
                  runtime::RequiredPhysicalAction::RemoveCapture,
          "restoring an en-passant capture square reopens its removal prompt");
    Observe(model, 6, CAPTURE_MASK, TARGET_MASK);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Settling,
          "removing the restored en-passant pawn starts a fresh settle");

    Observe(model, 7, TARGET_MASK, 0ULL);
    Check(model.phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
              model.phase.requiredAction ==
                  runtime::RequiredPhysicalAction::PlacePrimary,
          "en-passant destination relift reopens mover placement");
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::EnPassant &&
              model.phase.plan.progress.removalComplete,
          "en-passant destination relift retains captured-pawn removal");
    Observe(model, 8, TARGET_MASK, TARGET_MASK);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Settling,
          "replacing en-passant destination starts a fresh settle");

    constexpr uint64_t ORDINARY_TARGET = 1ULL << 21U; // f6
    constexpr uint64_t ORDINARY_BEFORE = BEFORE | ORDINARY_TARGET;
    runtime::ResetChessInteractionModel(model);
    runtime::ReduceChessInteraction(
        model, runtime::ChessInteractionEvent::MakePositionAcknowledged(
            ORDINARY_BEFORE, 0ULL, 0ULL, FROM_MASK,
            CAPTURE_MASK | ORDINARY_TARGET, BEFORE, TARGET, CAPTURE,
            true, false, true, ORDINARY_TARGET));
    Observe(model, 1U, 0ULL, ORDINARY_BEFORE);
    Observe(model, 2U, ORDINARY_TARGET, BEFORE);
    Observe(model, 3U, FROM_MASK | ORDINARY_TARGET,
            ORDINARY_BEFORE & ~FROM_MASK);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Settling &&
              model.phase.plan.kind == runtime::PhysicalTransitionKind::Capture &&
              model.phase.plan.moveTo == 21U &&
              model.phase.plan.removalSquare == 21U &&
              model.phase.plan.progress.removalComplete,
          "EP-eligible pawn retains observed removal for an atomic ordinary capture elsewhere");
    ConfirmSettle(model);
    Check(model.phase.tag == runtime::ChessInteractionPhase::AwaitingAction,
          "ordinary capture by an EP-eligible pawn exposes its one move action");
}

void TestOrdinaryCaptureOfEnPassantVictim() {
    for (const uint8_t mirror : {uint8_t{0U}, uint8_t{56U}}) {
        for (const bool pawnMover : {false, true}) {
            for (uint8_t order = 0U; order < 3U; ++order) {
                const uint8_t from = (pawnMover ? 34U : 41U) ^ mirror;
                const uint8_t epFrom = 28U ^ mirror;
                const uint8_t target = 27U ^ mirror;
                const uint8_t epTarget = 19U ^ mirror;
                const uint64_t fromBit = 1ULL << from;
                const uint64_t targetBit = 1ULL << target;
                const uint64_t epFromBit = 1ULL << epFrom;
                const uint64_t before = fromBit | targetBit | epFromBit;
                runtime::ChessInteractionModel model;
                runtime::ResetChessInteractionModel(model);
                Acknowledge(model, before,
                            targetBit | epFromBit | (pawnMover ? fromBit : 0ULL),
                            fromBit | epFromBit, targetBit, epTarget, target);
                Observe(model, 1U, 0ULL, before);
                if (order == 2U) {
                    Observe(model, 2U, fromBit | targetBit, epFromBit);
                } else {
                    const uint64_t first = order == 0U ? fromBit : targetBit;
                    Observe(model, 2U, first, before & ~first);
                    Observe(model, 3U, (fromBit | targetBit) ^ first, epFromBit);
                }
                Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::Capture &&
                          model.phase.plan.moveTo == target,
                      "ordinary capture of EP victim keeps its occupied landing for either side/order");
                Observe(model, 4U, targetBit, epFromBit | targetBit);
                Check(model.phase.tag == runtime::ChessInteractionPhase::Settling,
                      "ordinary capture of EP victim reaches settle without an EP removal prompt");
                ConfirmSettle(model);
                Check(model.phase.tag == runtime::ChessInteractionPhase::AwaitingAction,
                      "ordinary capture of EP victim exposes one acknowledged action");
            }
        }
    }
}

void TestNonPawnLandingOnEnPassantTarget() {
    constexpr uint64_t FROM = 1ULL << 36U;  // Ne4
    constexpr uint64_t PAWN = 1ULL << 26U;  // Pc5
    constexpr uint64_t VICTIM = 1ULL << 27U; // pd5
    constexpr uint64_t TARGET = 1ULL << 19U; // d6
    constexpr uint64_t BEFORE = FROM | PAWN | VICTIM;
    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    Acknowledge(model, BEFORE, PAWN | VICTIM, FROM | PAWN, VICTIM, 19U, 27U);
    Observe(model, 1U, 0ULL, BEFORE);
    Observe(model, 2U, FROM, PAWN | VICTIM);
    Observe(model, 3U, TARGET, PAWN | VICTIM | TARGET);
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::QuietMove &&
              !model.phase.plan.removalRequired &&
              model.phase.tag == runtime::ChessInteractionPhase::Settling,
          "knight landing on EP target leaves vulnerable pawn in place");

    runtime::ResetChessInteractionModel(model);
    Acknowledge(model, BEFORE, PAWN | VICTIM, FROM | PAWN, VICTIM, 19U, 27U);
    Observe(model, 1U, 0ULL, BEFORE);
    Observe(model, 2U, FROM, PAWN | VICTIM);
    Observe(model, 3U, TARGET | VICTIM, PAWN | TARGET);
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::QuietMove &&
              !model.phase.plan.removalRequired,
          "coalesced knight landing and next pawn pickup remain a quiet move plus follow-up");

    constexpr uint64_t QUIET_TARGET = 1ULL << 18U; // c6
    runtime::ResetChessInteractionModel(model);
    Acknowledge(model, BEFORE, PAWN | VICTIM, FROM | PAWN, VICTIM, 19U, 27U);
    Observe(model, 1U, 0ULL, BEFORE);
    Observe(model, 2U, PAWN, FROM | VICTIM);
    Observe(model, 3U, QUIET_TARGET | VICTIM, FROM | QUIET_TARGET);
    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::QuietMove &&
              model.phase.plan.moveFrom == 26U &&
              model.phase.plan.moveTo == 18U &&
              !model.phase.plan.removalRequired &&
              model.phase.tag == runtime::ChessInteractionPhase::Settling,
          "EP-eligible pawn quiet landing plus next-side pickup remains an ordinary move");
}

void TestPromotionUsesPlan() {
    constexpr uint8_t FROM = 8U;
    constexpr uint8_t TO = 0U;
    constexpr uint64_t FROM_MASK = 1ULL << FROM;
    constexpr uint64_t TO_MASK = 1ULL << TO;

    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    Acknowledge(model, FROM_MASK, FROM_MASK, FROM_MASK, 0);
    Observe(model, 1, 0, FROM_MASK);
    Observe(model, 2, FROM_MASK, 0);
    Observe(model, 3, TO_MASK, TO_MASK);

    Check(model.phase.plan.kind == runtime::PhysicalTransitionKind::Promotion,
          "last-rank pawn move uses promotion plan");
    Check(model.phase.plan.promotionRequired,
          "promotion requirement is part of the transition plan");
    Check(model.phase.tag == runtime::ChessInteractionPhase::Settling,
          "physical promotion completion awaits settle confirmation");
    Check(model.phase.pendingActionToken == 0U,
          "promotion plan exposes no adapter action before settle");
}

void TestZeroCorrectionClearsPhase() {
    runtime::ChessInteractionModel model;
    runtime::ResetChessInteractionModel(model);
    model.phase.tag = runtime::ChessInteractionPhase::PieceLifted;
    model.phase.selectedSquare = 12U;
    runtime::EnterChessInteractionCorrection(model, 0ULL);
    Check(model.phase.tag == runtime::ChessInteractionPhase::Idle &&
              model.phase.correctionMask == 0ULL,
          "zero correction mask normalizes to Idle");
}

} // namespace

int main() {
    std::puts("[CHESS_INTERACTION_CAPTURE_PLANS] start");
    TestMoverFirstCapture();
    TestTargetFirstCapture();
    TestModalCaptureObservationRemainsSnapshotOnly();
    TestCaptureProgressObservationAdmission();
    TestCaptureProgressDefersCompletionAndResetsProof();
    TestIllegalTargetFirstCaptureRequiresCorrection();
    TestAtomicPickupHandoffs();
    TestReturnedSourceStagesAnotherCapture();
    TestCaptureFactsSourceReturnCannotSelectAnotherMover();
    TestRestoredHumanPlansCancel();
    TestEnPassantBlocksUntilRemoval();
    TestOrdinaryCaptureOfEnPassantVictim();
    TestNonPawnLandingOnEnPassantTarget();
    TestAtomicEnPassantWithExtraRemovalRejects();
    TestExecutingCaptureRejectsThirdPiece();
    TestPromotionUsesPlan();
    TestZeroCorrectionClearsPhase();
    if (g_failures == 0) {
        std::puts("[CHESS_INTERACTION_CAPTURE_PLANS] ok");
        return 0;
    }
    std::printf("[CHESS_INTERACTION_CAPTURE_PLANS] failures=%d\n", g_failures);
    return 1;
}
