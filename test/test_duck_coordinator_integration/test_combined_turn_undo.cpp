#include <cstdint>
#include <cstdio>
#include <cstring>

#include "duck_coordinator_test_support.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/game_variant.hpp"
#include "modules/beginner_mode_controller.hpp"
#include "modules/home_state_snapshot.hpp"

int RunDuckCoordinatorTakebackGraceTests();

namespace {

using duck_coordinator_test::Harness;
using duck_coordinator_test::INITIAL_OCCUPANCY;

int g_failures = 0;
constexpr uint8_t QUIET_DUCK_BEFORE = 24U;
constexpr uint8_t QUIET_DUCK_AFTER = 25U;
constexpr char QUIET_POST_FEN[] =
    "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/"
    "RNBQKBNR b KQkq e3 0 1";
constexpr char EXTERNAL_FEN[] =
    "8/8/8/8/8/8/8/4K2k w - - 0 1";

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

uint64_t MaskFor(const uint8_t square) {
    return 1ULL << square;
}

void LatchDuck(Harness& harness, const uint8_t duckSquare) {
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | MaskFor(duckSquare));
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
}

uint64_t StageE2E4(Harness& harness, const uint8_t duckSquare) {
    uint64_t physical = INITIAL_OCCUPANCY | MaskFor(duckSquare);
    physical &= ~MaskFor(52U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(52U), physical);
    physical |= MaskFor(36U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(36U), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
    return physical;
}

uint64_t RelocateDuck(Harness& harness,
                      uint64_t physical,
                      const uint8_t from,
                      const uint8_t to) {
    physical &= ~MaskFor(from);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(from), physical);
    physical |= MaskFor(to);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(to), physical);
    harness.coordinator.Update();
    return physical;
}

void CheckFixedDuckPrompt(Harness& harness,
                          const uint8_t expectedFrom,
                          const uint8_t expectedTo,
                          const char* message) {
    uint8_t from = modules::INVALID_DUCK_SQUARE;
    uint8_t to = modules::INVALID_DUCK_SQUARE;
    Check(harness.stateMachine.TryGetDuckRelocation(from, to) &&
              from == expectedFrom && to == expectedTo,
          message);
    const modules::BeginnerModeController beginnerMode;
    const auto home = modules::BuildHomeStateSnapshot(
        harness.stateMachine, harness.coordinator, harness.settings,
        beginnerMode, modules::PersistenceDegradation::None);
    Check(home.duckRelocationOwner == 'u' &&
              home.duckRelocationFromSquare == expectedFrom &&
              home.duckRelocationToSquare == expectedTo,
          "fixed Undo/CancelRedo Duck plan publishes neutral exact guidance");
}

uint64_t CommitQuietDuckTurn(Harness& harness) {
    LatchDuck(harness, QUIET_DUCK_BEFORE);
    uint64_t physical = StageE2E4(harness, QUIET_DUCK_BEFORE);
    const modules::BeginnerModeController beginnerMode;
    const auto home = modules::BuildHomeStateSnapshot(
        harness.stateMachine, harness.coordinator, harness.settings,
        beginnerMode, modules::PersistenceDegradation::None);
    Check(home.duckRelocationOwner == 'h' &&
              home.duckRelocationFromSquare == QUIET_DUCK_BEFORE &&
              home.duckRelocationToSquare == modules::INVALID_DUCK_SQUARE,
          "ordinary Human Duck plan publishes selectable guidance");
    physical = RelocateDuck(harness, physical,
                            QUIET_DUCK_BEFORE, QUIET_DUCK_AFTER);
    Check(std::strcmp(harness.engine.Fen(), QUIET_POST_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.coordinator.GetDuckSquare() == QUIET_DUCK_AFTER,
          "fixture commits one complete Duck turn");
    return physical;
}

uint64_t BeginAndApplyLogicalUndo(Harness& harness, uint64_t physical) {
    Check(harness.coordinator.RequestUndo(1U),
          "Duck whole-turn undo request is accepted");
    CheckFixedDuckPrompt(
        harness, QUIET_DUCK_AFTER, QUIET_DUCK_BEFORE,
        "undo first requests new-to-prior fixed duck relocation");
    physical = RelocateDuck(harness, physical,
                            QUIET_DUCK_AFTER, QUIET_DUCK_BEFORE);
    uint8_t undoFrom = 0U;
    uint8_t undoTo = 0U;
    Check(harness.stateMachine.IsUndoPromptActive() &&
              harness.stateMachine.TryGetUndoPromptMove(
                  undoFrom, undoTo) &&
              undoFrom == 52U && undoTo == 36U,
          "chess inverse prompt starts only after duck returns");
    Check(std::strcmp(harness.engine.Fen(), QUIET_POST_FEN) == 0 &&
              harness.coordinator.GetDuckSquare() == QUIET_DUCK_AFTER,
          "duck relocation precedes logical chess undo");

    harness.coordinator.Update();
    Check(std::strcmp(harness.engine.Fen(), modules::INITIAL_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U &&
              harness.coordinator.GetDuckSquare() == QUIET_DUCK_BEFORE,
          "logical undo restores pre-chess FEN and prior duck square");
    Check(harness.stateMachine.IsUndoPromptActive() &&
              harness.coordinator.IsCancelUndoAvailable(),
          "physical chess inverse remains cancelable after logical undo");
    return physical;
}

uint64_t CancelUndoAndRedoDuck(Harness& harness, uint64_t physical) {
    Check(harness.coordinator.RequestUndo(1U) &&
              harness.coordinator.IsUndoActionPending(),
          "accepted cancel-undo remains pending before its update runs");
    harness.coordinator.Update();
    Check(std::strcmp(harness.engine.Fen(), QUIET_POST_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.coordinator.GetDuckSquare() == QUIET_DUCK_BEFORE,
          "cancel-undo restores the post-turn chess state first");
    CheckFixedDuckPrompt(
        harness, QUIET_DUCK_BEFORE, QUIET_DUCK_AFTER,
        "cancel-undo requests prior-to-new fixed duck redo");
    Check(harness.coordinator.IsUndoActionPending(),
          "cancel-undo stays pending when the fixed Duck redo starts");
    physical &= ~MaskFor(QUIET_DUCK_BEFORE);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(QUIET_DUCK_BEFORE), physical);
    Check(harness.coordinator.IsUndoActionPending(),
          "cancel-undo stays pending while the Duck redo is in hand");
    physical |= MaskFor(QUIET_DUCK_AFTER);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(QUIET_DUCK_AFTER), physical);
    harness.coordinator.Update();
    Check(std::strcmp(harness.engine.Fen(), QUIET_POST_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.coordinator.GetDuckSquare() == QUIET_DUCK_AFTER &&
              !harness.coordinator.IsUndoActionPending() &&
              !harness.stateMachine.IsDuckRelocationActive(),
          "duck redo restores the complete post-turn state");
    return physical;
}

void CompletePhysicalWholeTurnUndo(Harness& harness, uint64_t physical) {
    Check(harness.coordinator.RequestUndo(1U),
          "whole-turn undo can be restarted after redo");
    physical = RelocateDuck(harness, physical,
                            QUIET_DUCK_AFTER, QUIET_DUCK_BEFORE);
    harness.coordinator.Update();
    physical &= ~MaskFor(36U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(36U), physical);
    physical |= MaskFor(52U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(52U), physical);
    harness.coordinator.Update();
    Check(physical ==
                  (INITIAL_OCCUPANCY | MaskFor(QUIET_DUCK_BEFORE)) &&
              std::strcmp(harness.engine.Fen(), modules::INITIAL_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U &&
              harness.stateMachine.GetMovedSquaresMask() == 0ULL &&
              harness.coordinator.GetGameVariant() ==
                  modules::GameVariant::Undecided &&
              harness.coordinator.GetInitialDuckCandidateSquare() ==
                  QUIET_DUCK_BEFORE &&
              !harness.stateMachine.IsUndoPromptActive(),
          "completed physical inverse restores fresh Duck start state");
    Check(harness.coordinator.ShouldValidateInitialVariantTransition(
              INITIAL_OCCUPANCY),
          "fresh Duck start can remove the duck after whole-turn undo");
}

void TestWholeTurnUndoAndCancelRedo() {
    fxe::ManualMillis::SetNowMs(4000U);
    Harness harness("build/test_duck_coord_whole_turn_undo.json");
    Check(harness.LoadFresh(), "Duck undo harness initializes");
    uint64_t physical = CommitQuietDuckTurn(harness);
    physical = BeginAndApplyLogicalUndo(harness, physical);
    physical = CancelUndoAndRedoDuck(harness, physical);
    CompletePhysicalWholeTurnUndo(harness, physical);
}

void TestUntouchedDuckUndoCancelsBeforeRelocation() {
    fxe::ManualMillis::SetNowMs(4300U);
    Harness harness("build/test_duck_coord_untouched_undo_cancel.json");
    Check(harness.LoadFresh(),
          "untouched Duck cancel harness initializes");
    const uint64_t physical = CommitQuietDuckTurn(harness);

    Check(harness.coordinator.RequestUndo(1U) &&
              harness.coordinator.IsUndoActionPending() &&
              harness.stateMachine.IsDuckRelocationActive(),
          "Duck-first inverse projects canonical Undo pending state");
    Check(harness.coordinator.RequestUndo(1U) &&
              !harness.coordinator.IsUndoActionPending() &&
              !harness.stateMachine.IsDuckRelocationActive() &&
              harness.stateMachine.GetPhysicalOccupancyMask() == physical &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.coordinator.GetDuckSquare() == QUIET_DUCK_AFTER &&
              std::strcmp(harness.engine.Fen(), QUIET_POST_FEN) == 0,
          "second Undo cancels an untouched Duck-first inverse atomically");
}

void TestTouchedDuckReturnRejectsCancelUntilRestored() {
    fxe::ManualMillis::SetNowMs(4400U);
    Harness harness("build/test_duck_coord_touched_undo_cancel.json");
    Check(harness.LoadFresh(),
          "touched Duck cancel harness initializes");
    uint64_t physical = CommitQuietDuckTurn(harness);

    Check(harness.coordinator.RequestUndo(1U) &&
              harness.coordinator.IsUndoActionPending(),
          "Duck-first inverse is pending before the Duck is lifted");
    physical &= ~MaskFor(QUIET_DUCK_AFTER);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(QUIET_DUCK_AFTER), physical);
    Check(!harness.coordinator.RequestUndo(1U) &&
              harness.coordinator.IsUndoActionPending() &&
              harness.stateMachine.IsDuckRelocationActive(),
          "Cancel Undo is rejected while the Duck return is in hand");

    physical |= MaskFor(QUIET_DUCK_AFTER);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(QUIET_DUCK_AFTER), physical);
    Check(harness.coordinator.RequestUndo(1U) &&
              !harness.coordinator.IsUndoActionPending() &&
              !harness.stateMachine.IsDuckRelocationActive() &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.coordinator.GetDuckSquare() == QUIET_DUCK_AFTER &&
              std::strcmp(harness.engine.Fen(), QUIET_POST_FEN) == 0,
          "returning the Duck to its source makes cancellation safe again");
}

void TestUndoWhileDuckRelocationIsPending() {
    fxe::ManualMillis::SetNowMs(2750U);
    Harness harness("build/test_duck_coord_staged_chess_undo.json");
    Check(harness.LoadFresh(), "staged-chess undo harness initializes");

    LatchDuck(harness, QUIET_DUCK_BEFORE);
    uint64_t physical = StageE2E4(harness, QUIET_DUCK_BEFORE);
    Check(harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::DuckRelocation &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "staged chess move awaits mandatory duck relocation");

    uint8_t undoFrom = 0U;
    uint8_t undoTo = 0U;
    Check(harness.coordinator.RequestUndo(1U) &&
              harness.stateMachine.IsUndoPromptActive() &&
              harness.stateMachine.TryGetUndoPromptMove(
                  undoFrom, undoTo) &&
              undoFrom == 52U && undoTo == 36U,
          "undo during duck phase prompts the staged chess move backward");
    Check(harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::ChessMove &&
              std::strcmp(harness.engine.Fen(), modules::INITIAL_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "staged undo leaves engine and history at pre-move authority");

    physical &= ~MaskFor(36U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(36U), physical);
    physical |= MaskFor(52U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(52U), physical);
    harness.coordinator.Update();
    Check(!harness.stateMachine.IsUndoPromptActive() &&
              harness.coordinator.GetGameVariant() ==
                  modules::GameVariant::Undecided,
          "completing staged undo restores a fresh setup boundary");
}

void CheckRebootRestoresForwardTurn(const char* path,
                                    const char* message) {
    Harness reloaded(path, false);
    Check(reloaded.LoadExisting() &&
              std::strcmp(reloaded.engine.Fen(), QUIET_POST_FEN) == 0 &&
              reloaded.coordinator.GetUndoHistoryMoveCount() == 1U &&
              reloaded.coordinator.GetDuckSquare() == QUIET_DUCK_AFTER,
          message);
}

void TestFlushRejectsDuckUndoRelocation() {
    static constexpr char PATH[] =
        "build/test_duck_coord_undo_relocation_flush.json";
    fxe::ManualMillis::SetNowMs(5000U);
    Harness harness(PATH);
    Check(harness.LoadFresh(), "Duck undo-flush harness initializes");
    (void)CommitQuietDuckTurn(harness);
    Check(harness.coordinator.FlushToSettingsNow(),
          "forward Duck turn flushes before undo relocation starts");

    Check(harness.coordinator.RequestUndo(1U),
          "Duck undo begins before reverse-relocation flush");
    CheckFixedDuckPrompt(
        harness, QUIET_DUCK_AFTER, QUIET_DUCK_BEFORE,
        "reverse Duck relocation is active before flush");
    Check(!harness.coordinator.FlushToSettingsNow(),
          "flush refuses RAM-only reverse Duck relocation");
    CheckFixedDuckPrompt(
        harness, QUIET_DUCK_AFTER, QUIET_DUCK_BEFORE,
        "failed flush preserves reverse Duck relocation");
    Check(std::strcmp(harness.engine.Fen(), QUIET_POST_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.coordinator.GetDuckSquare() == QUIET_DUCK_AFTER,
          "failed reverse-relocation flush preserves logical state");
    CheckRebootRestoresForwardTurn(
        PATH, "reverse-relocation flush leaves saved forward turn intact");
    Check(harness.coordinator.LoadExternalFen(EXTERNAL_FEN) &&
              harness.coordinator.GetGameVariant() ==
                  modules::GameVariant::Classic &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U &&
              std::strcmp(harness.engine.Fen(), EXTERNAL_FEN) == 0,
          "external FEN abandons RAM-only reverse Duck relocation");
}

void TestFlushRejectsDuckCancelUndoRedo() {
    static constexpr char PATH[] =
        "build/test_duck_coord_redo_relocation_flush.json";
    fxe::ManualMillis::SetNowMs(5500U);
    Harness harness(PATH);
    Check(harness.LoadFresh(), "Duck redo-flush harness initializes");
    uint64_t physical = CommitQuietDuckTurn(harness);
    Check(harness.coordinator.FlushToSettingsNow(),
          "forward Duck turn flushes before cancel-undo relocation starts");
    (void)BeginAndApplyLogicalUndo(harness, physical);

    Check(!harness.coordinator.FlushToSettingsNow(),
          "flush refuses fixed Duck redo entered by cancel-undo");
    CheckFixedDuckPrompt(
        harness, QUIET_DUCK_BEFORE, QUIET_DUCK_AFTER,
        "flush cancellation leaves fixed Duck redo active");
    Check(std::strcmp(harness.engine.Fen(), QUIET_POST_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.coordinator.GetDuckSquare() == QUIET_DUCK_BEFORE,
          "redo-relocation flush does not persist the prior Duck square");
    fxe::ManualMillis::AdvanceMs(1000U);
    harness.coordinator.Update();
    CheckFixedDuckPrompt(
        harness, QUIET_DUCK_BEFORE, QUIET_DUCK_AFTER,
        "delayed persistence retry preserves fixed Duck redo");
    CheckRebootRestoresForwardTurn(
        PATH, "redo-relocation retry leaves saved forward turn intact");
    Check(harness.coordinator.LoadExternalFen(EXTERNAL_FEN) &&
              harness.coordinator.GetGameVariant() ==
                  modules::GameVariant::Classic &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U &&
              std::strcmp(harness.engine.Fen(), EXTERNAL_FEN) == 0,
          "external FEN abandons RAM-only Duck redo relocation");
}

void StageEnPassant(Harness& harness, uint64_t& physical) {
    constexpr uint8_t FROM = 28U;
    constexpr uint8_t TARGET = 19U;
    constexpr uint8_t CAPTURE = 27U;
    physical &= ~MaskFor(FROM);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(FROM), physical);
    harness.coordinator.UpdateLegalDestinationsMask();
    Check((harness.stateMachine.GetValidDestMask() & MaskFor(TARGET)) != 0ULL,
          "Duck legal pickup includes the en-passant target");
    physical &= ~MaskFor(CAPTURE);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(CAPTURE), physical);
    physical |= MaskFor(TARGET);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(TARGET), physical);
    Check(harness.stateMachine.GetChessInteractionModel().phase.plan.kind ==
              runtime::PhysicalTransitionKind::EnPassant,
          "physical choreography retains en-passant identity");
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
}

void TestEnPassantStagesUntilDuckRelocation() {
    static constexpr char PRE_FEN[] =
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1";
    static constexpr char POST_FEN[] =
        "4k3/8/3P4/8/8/8/8/4K3 b - - 0 1";
    constexpr uint8_t DUCK_FROM = 40U;
    constexpr uint8_t DUCK_TO = 41U;

    fxe::ManualMillis::SetNowMs(6000U);
    Harness harness("build/test_duck_coord_en_passant.json");
    Check(duck_coordinator_test::SeedPersistedDuckPosition(
              harness, PRE_FEN, DUCK_FROM),
          "Duck en-passant position persists");
    Check(harness.coordinator.LoadOrInitialize(),
          "Duck en-passant position loads");
    uint64_t physical =
        duck_coordinator_test::FenOccupancy(PRE_FEN) | MaskFor(DUCK_FROM);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    StageEnPassant(harness, physical);

    Check(harness.coordinator.GetDuckTurnPhase() ==
              modules::DuckTurnPhase::DuckRelocation &&
              std::strcmp(harness.engine.Fen(), PRE_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "en-passant remains staged until mandatory duck relocation");
    physical = RelocateDuck(
        harness, physical, DUCK_FROM, DUCK_TO);
    Check(std::strcmp(harness.engine.Fen(), POST_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.coordinator.GetDuckSquare() == DUCK_TO,
          "duck relocation atomically commits en-passant and its capture");
}

void TestCastlingStagesUntilDuckRelocation() {
    static constexpr char PRE_FEN[] =
        "4k3/8/8/8/8/8/8/4K2R w K - 0 1";
    static constexpr char POST_FEN[] =
        "4k3/8/8/8/8/8/8/5RK1 b - - 1 1";
    constexpr uint8_t DUCK_FROM = 32U;
    constexpr uint8_t DUCK_TO = 33U;
    constexpr uint8_t KING_FROM = 60U;
    constexpr uint8_t KING_TO = 62U;
    constexpr uint8_t ROOK_FROM = 63U;
    constexpr uint8_t ROOK_TO = 61U;

    fxe::ManualMillis::SetNowMs(7000U);
    Harness harness("build/test_duck_coord_castling.json");
    Check(duck_coordinator_test::SeedPersistedDuckPosition(
              harness, PRE_FEN, DUCK_FROM) &&
              harness.coordinator.LoadOrInitialize(),
          "Duck castling position loads");
    uint64_t physical =
        duck_coordinator_test::FenOccupancy(PRE_FEN) | MaskFor(DUCK_FROM);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);

    physical &= ~MaskFor(ROOK_FROM);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(ROOK_FROM), physical);
    harness.coordinator.UpdateLegalDestinationsMask();
    Check((harness.stateMachine.GetValidDestMask() & MaskFor(ROOK_TO)) != 0ULL,
          "Duck rook-first castle starts with ordinary rook moves");
    physical &= ~MaskFor(KING_FROM);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(KING_FROM), physical);
    harness.coordinator.UpdateLegalDestinationsMask();
    Check((harness.stateMachine.GetValidDestMask() & MaskFor(KING_TO)) != 0ULL,
          "Duck rook-first castling destination is pseudo-legal");
    physical |= MaskFor(KING_TO);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(KING_TO), physical);
    physical |= MaskFor(ROOK_TO);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(ROOK_TO), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
    Check(harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::DuckRelocation &&
              std::strcmp(harness.engine.Fen(), PRE_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "composite castling plan remains staged before duck relocation");

    physical = RelocateDuck(
        harness, physical, DUCK_FROM, DUCK_TO);
    Check(std::strcmp(harness.engine.Fen(), POST_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.coordinator.GetDuckSquare() == DUCK_TO,
          "duck relocation atomically commits castling");
}

void TestPromotionChoiceStagesUntilDuckRelocation() {
    static constexpr char PRE_FEN[] =
        "7k/P7/8/8/8/8/8/4K3 w - - 0 1";
    static constexpr char POST_FEN[] =
        "R6k/8/8/8/8/8/8/4K3 b - - 0 1";
    constexpr uint8_t DUCK_FROM = 39U;
    constexpr uint8_t DUCK_TO = 38U;
    constexpr uint8_t PAWN_FROM = 8U;
    constexpr uint8_t PAWN_TO = 0U;

    fxe::ManualMillis::SetNowMs(8000U);
    Harness harness("build/test_duck_coord_promotion.json");
    Check(duck_coordinator_test::SeedPersistedDuckPosition(harness, PRE_FEN, DUCK_FROM),
          "Duck promotion position seeds");
    harness.settings.Doc()["PROMOTION_MODE"] = "choose";
    Check(harness.coordinator.LoadOrInitialize(), "Duck promotion position loads");
    uint64_t physical =
        duck_coordinator_test::FenOccupancy(PRE_FEN) | MaskFor(DUCK_FROM);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    physical &= ~MaskFor(PAWN_FROM);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(PAWN_FROM), physical);
    harness.coordinator.UpdateLegalDestinationsMask();
    physical |= MaskFor(PAWN_TO);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(PAWN_TO), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
    Check(harness.stateMachine.IsPromotionActive(),
          "Duck promotion opens the ordinary choice modal");
    harness.stateMachine.HandleButtonEvent(
        modules::UserButton::Right, fxe::Button::Event::Press);
    const uint32_t token = harness.stateMachine.GetChessInteractionModel().phase.pendingActionToken;
    physical &= ~MaskFor(PAWN_TO);
    duck_coordinator_test::Observe(harness.stateMachine, MaskFor(PAWN_TO), physical);
    harness.stateMachine.HandleButtonEvent(
        modules::UserButton::P1O, fxe::Button::Event::Press);
    harness.coordinator.Update();
    Check(harness.stateMachine.IsPromotionActive() &&
              harness.stateMachine.GetPromotionSelection() == modules::PromotionPiece::Rook &&
              harness.stateMachine.GetChessInteractionModel().phase.pendingActionToken == token &&
              harness.coordinator.GetDuckTurnPhase() != modules::DuckTurnPhase::DuckRelocation &&
              std::strcmp(harness.engine.Fen(), PRE_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "Duck promotion cannot leave its selected Rook modal with replacement absent");
    physical |= MaskFor(PAWN_TO);
    duck_coordinator_test::Observe(harness.stateMachine, MaskFor(PAWN_TO), physical);
    harness.coordinator.Update();
    Check(harness.stateMachine.IsPromotionActive() &&
              harness.coordinator.GetDuckTurnPhase() != modules::DuckTurnPhase::DuckRelocation,
          "Duck promotion replacement cannot replay the rejected O");
    harness.stateMachine.HandleButtonEvent(
        modules::UserButton::P1O, fxe::Button::Event::Press);
    harness.coordinator.Update();
    Check(harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::DuckRelocation &&
              std::strcmp(harness.engine.Fen(), PRE_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "chosen underpromotion remains staged before duck relocation");

    physical = RelocateDuck(
        harness, physical, DUCK_FROM, DUCK_TO);
    Check(std::strcmp(harness.engine.Fen(), POST_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.coordinator.GetDuckSquare() == DUCK_TO,
          "duck relocation atomically commits chosen underpromotion");
}

} // namespace

int RunDuckCoordinatorCombinedTurnUndoTests() {
    TestWholeTurnUndoAndCancelRedo();
    TestUntouchedDuckUndoCancelsBeforeRelocation();
    TestTouchedDuckReturnRejectsCancelUntilRestored();
    g_failures += RunDuckCoordinatorTakebackGraceTests();
    TestUndoWhileDuckRelocationIsPending();
    TestFlushRejectsDuckUndoRelocation();
    TestFlushRejectsDuckCancelUndoRedo();
    TestEnPassantStagesUntilDuckRelocation();
    TestCastlingStagesUntilDuckRelocation();
    TestPromotionChoiceStagesUntilDuckRelocation();
    return g_failures;
}
