#include <cstdint>
#include <cstdio>
#include <cstring>

#include "duck_coordinator_test_support.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/game_variant.hpp"
#include "modules/home_state_notification.hpp"
#include "modules/orientation.hpp"
#include "runtime/runtime_health.hpp"

int RunDuckCoordinatorCombinedTurnUndoTests();
int RunDuckCoordinatorEarlyLiftTests();
int RunDuckCoordinatorRecoveryCapacityTests();
int RunDuckPseudoLegalHistoryReplayTests();
int RunDuckCoordinatorSurfaceBlockerTests();
int RunDuckKingCaptureOutcomeTest();

namespace {

using duck_coordinator_test::Harness;
using duck_coordinator_test::INITIAL_OCCUPANCY;

int g_failures = 0;

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

void SetTime(const uint32_t nowMs = 1000U) {
    fxe::ManualMillis::SetNowMs(nowMs);
}

void LatchDuck(Harness& harness, const uint8_t duckSquare) {
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | MaskFor(duckSquare));
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
}

uint64_t MapPhysicalMaskToLogical(const uint64_t physical,
                                  const bool rotated) {
    uint64_t logical = 0ULL;
    for (uint8_t physicalSquare = 0U; physicalSquare < 64U;
         ++physicalSquare) {
        if ((physical & MaskFor(physicalSquare)) != 0ULL) {
            logical |= MaskFor(modules::MapSquareToLogical(
                physicalSquare, rotated));
        }
    }
    return logical;
}

void TestValidActivationAfterStability() {
    SetTime();
    Harness rankSix("build/test_duck_coord_rank6.json");
    Check(rankSix.LoadFresh(), "rank-6 harness initializes");
    constexpr uint8_t DUCK_C6 = 18U;
    const uint64_t occupancy = INITIAL_OCCUPANCY | MaskFor(DUCK_C6);
    rankSix.stateMachine.ApplyPhysicalOccupancySnapshot(occupancy);
    Check(rankSix.coordinator.ShouldDeferInitialPlacementForVariantDetection(
              occupancy),
          "valid candidate defers ordinary extra-piece feedback");
    fxe::ManualMillis::AdvanceMs(199U);
    rankSix.coordinator.Update();
    Check(rankSix.coordinator.GetGameVariant() ==
              modules::GameVariant::Undecided,
          "candidate does not latch before 200 ms stability");
    fxe::ManualMillis::AdvanceMs(1U);
    rankSix.coordinator.Update();
    Check(rankSix.coordinator.GetGameVariant() == modules::GameVariant::Duck,
          "rank-6 candidate latches Duck after stability");
    Check(rankSix.coordinator.GetDuckSquare() == DUCK_C6,
          "rank-6 logical duck square is retained");
    Check(rankSix.stateMachine.GetExpectedOccupancyMask() == occupancy,
          "latched expected occupancy includes duck");
    Check(!rankSix.stateMachine.IsHintThinkingActive(),
          "Duck latch cancels any pre-variant hint search");

    SetTime(2000U);
    Harness rankFive("build/test_duck_coord_rank5.json");
    Check(rankFive.LoadFresh(), "rank-5 harness initializes");
    rankFive.settings.Doc()["VS_CPU_LEVEL"] = 7U;
    constexpr uint8_t DUCK_F5 = 29U;
    LatchDuck(rankFive, DUCK_F5);
    Check(rankFive.coordinator.GetGameVariant() == modules::GameVariant::Duck &&
              rankFive.coordinator.GetDuckSquare() == DUCK_F5,
          "rank-5 candidate also activates Duck");
    Check(rankFive.engine.thinkStartCalls == 0U,
          "Duck activation does not start an orthodox engine search");
    Check(rankFive.settings.ReadValue("VS_CPU_LEVEL", 99U) == 7U,
          "Duck activation preserves the configured CPU level");

    SetTime(2200U);
    Harness rankFour("build/test_duck_coord_rank4.json");
    Check(rankFour.LoadFresh(), "rank-4 harness initializes");
    constexpr uint8_t DUCK_D4 = 35U;
    LatchDuck(rankFour, DUCK_D4);
    Check(rankFour.coordinator.GetGameVariant() == modules::GameVariant::Duck &&
              rankFour.coordinator.GetDuckSquare() == DUCK_D4,
          "rank-4 candidate also activates Duck");

    SetTime(2400U);
    Harness rankThree("build/test_duck_coord_rank3.json");
    Check(rankThree.LoadFresh(), "rank-3 harness initializes");
    constexpr uint8_t DUCK_C3 = 42U;
    LatchDuck(rankThree, DUCK_C3);
    Check(rankThree.coordinator.GetGameVariant() == modules::GameVariant::Duck &&
              rankThree.coordinator.GetDuckSquare() == DUCK_C3,
          "rank-3 candidate also activates Duck");
}

void TestHomeStateOwnsProvisionalDuckBeforeLatch() {
    SetTime(2400U);
    Harness harness("build/test_duck_home_projection.json");
    Check(harness.LoadFresh(), "Duck Home projection harness initializes");
    constexpr uint8_t DUCK_C6 = 18U;
    const uint64_t occupancy =
        INITIAL_OCCUPANCY | MaskFor(DUCK_C6);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(occupancy);
    harness.coordinator.Update();

    const modules::GameVariantStateSnapshot beforeLatch =
        harness.coordinator.GetGameVariantStateSnapshot();
    Check(beforeLatch.variant == modules::GameVariant::Undecided,
          "Duck remains undecided during the 200 ms stability gate");
    Check(modules::HomeStateDuckSquareForPresentation(
              beforeLatch,
              harness.coordinator.GetInitialDuckCandidateSquare()) ==
              DUCK_C6,
          "Home treats the valid provisional Duck as intentional before latch");

    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
    const modules::GameVariantStateSnapshot afterLatch =
        harness.coordinator.GetGameVariantStateSnapshot();
    Check(afterLatch.variant == modules::GameVariant::Duck &&
              modules::HomeStateDuckSquareForPresentation(
                  afterLatch,
                  harness.coordinator.GetInitialDuckCandidateSquare()) ==
                  DUCK_C6,
          "Home retains the same Duck square across the durable latch");

    SetTime(2800U);
    Harness rankFour("build/test_duck_home_projection_rank4.json");
    Check(rankFour.LoadFresh(),
          "rank-4 Duck Home projection harness initializes");
    constexpr uint8_t DUCK_D4 = 35U;
    rankFour.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | MaskFor(DUCK_D4));
    rankFour.coordinator.Update();
    Check(modules::HomeStateDuckSquareForPresentation(
              rankFour.coordinator.GetGameVariantStateSnapshot(),
              rankFour.coordinator.GetInitialDuckCandidateSquare()) ==
              DUCK_D4,
          "Home treats a rank-4 opening extra as provisional Duck identity");
}

void TestNewGameDiscardsStagedDuckTurn() {
    SetTime(1800U);
    Harness harness("build/test_duck_coord_new_game_boundary.json");
    Check(harness.LoadFresh(), "new-game boundary harness initializes");
    constexpr uint8_t DUCK = 24U;
    LatchDuck(harness, DUCK);

    uint64_t physical = INITIAL_OCCUPANCY | MaskFor(DUCK);
    physical &= ~MaskFor(52U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(52U), physical);
    physical |= MaskFor(36U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(36U), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
    Check(harness.coordinator.GetDuckTurnPhase() ==
              modules::DuckTurnPhase::DuckRelocation,
          "fixture stages a Duck turn before reset");

    harness.coordinator.ResetEngine();
    Check(harness.coordinator.GetGameVariant() ==
                  modules::GameVariant::Undecided &&
              harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::ChessMove &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U &&
              std::strcmp(harness.engine.Fen(), modules::INITIAL_FEN) == 0,
          "New Game discards the staged Duck turn and its history");
    Check(!harness.coordinator.RequestUndo(1U),
          "Undo cannot recover a staged Duck turn across New Game");
}

void TestClockTimeoutAbandonsStagedDuckTurn() {
    SetTime(3000U);
    Harness harness("build/test_duck_coord_clock_timeout.json");
    Check(harness.LoadFresh(true), "clock timeout Duck harness initializes");
    harness.settings.Doc()["TIMER"] = 2U; // 3 minutes + 2-second increment.
    constexpr uint8_t DUCK = 24U;
    LatchDuck(harness, DUCK);
    fxe::ManualMillis::AdvanceMs(750U);
    harness.coordinator.Update();

    uint64_t physical = INITIAL_OCCUPANCY | MaskFor(DUCK);
    physical &= ~MaskFor(52U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(52U), physical);
    harness.coordinator.Update();
    physical |= MaskFor(36U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(36U), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
    Check(harness.coordinator.GetDuckTurnPhase() ==
              modules::DuckTurnPhase::DuckRelocation,
          "timeout fixture has a staged Duck relocation");

    const uint32_t whiteBefore =
        harness.coordinator.GetGameClock().GetRemainingMs(
            runtime::ClockSide::White);
    const uint32_t blackBefore =
        harness.coordinator.GetGameClock().GetRemainingMs(
            runtime::ClockSide::Black);
    fxe::ManualMillis::AdvanceMs(whiteBefore);
    harness.coordinator.Update();

    Check(harness.coordinator.HasClockTerminalOutcome() &&
              harness.coordinator.GetClockTerminalWinner() ==
                  modules::GameWinner::Black,
          "Duck timeout latches the opponent as clock winner");
    Check(harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::ChessMove &&
              std::strcmp(harness.engine.Fen(), modules::INITIAL_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U &&
              harness.coordinator.GetGameClock().GetActiveSide() ==
                  runtime::ClockSide::White,
          "Duck timeout rolls back the staged half without history, increment, or side switch");
    Check(harness.coordinator.GetGameClock().GetRemainingMs(
              runtime::ClockSide::White) == 0U &&
              harness.coordinator.GetGameClock().GetRemainingMs(
                  runtime::ClockSide::Black) == blackBefore,
          "Duck timeout applies neither increment nor opponent clock mutation");
    Check(!harness.stateMachine.HasQueuedCpuMove() &&
              !harness.stateMachine.IsCpuThinkingActive() &&
              !harness.stateMachine.IsHintThinkingActive() &&
              !harness.stateMachine.IsUndoPromptActive(),
          "Duck timeout clears queued CPU, hint, and undo interaction work");

    physical &= ~MaskFor(DUCK);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK), physical);
    physical |= MaskFor(25U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(25U), physical);
    harness.coordinator.Update();
    Check(std::strcmp(harness.engine.Fen(), modules::INITIAL_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "late Duck interaction cannot commit after timeout");
    Check(harness.coordinator.FlushToSettingsNow(),
          "terminal lifecycle flush ignores late executable interaction work");
    modules::GamePersistenceState savedTerminal;
    Check(modules::ReadGamePersistenceState(harness.settings.Doc(), savedTerminal) &&
              savedTerminal.clockState.clock.timedOut &&
              savedTerminal.interactionState.Empty() && !savedTerminal.cpuMoveValid,
          "terminal drain stores stopped authority without resumable tokens");
}

void TestClockTimeoutClearsExternalizedDuckAction() {
    SetTime(4000U);
    Harness harness("build/test_duck_coord_clock_timeout_externalized.json");
    Check(harness.LoadFresh(true),
          "externalized timeout Duck harness initializes");
    constexpr uint8_t DUCK = 24U;
    LatchDuck(harness, DUCK);
    fxe::ManualMillis::AdvanceMs(750U);
    harness.coordinator.Update();
    harness.coordinator.SetInteractionEventsExternalized(true);

    uint64_t physical = INITIAL_OCCUPANCY | MaskFor(DUCK);
    physical &= ~MaskFor(52U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(52U), physical);
    harness.coordinator.Update();
    physical |= MaskFor(36U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(36U), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();

    const runtime::TaggedInteractionState& awaiting =
        harness.stateMachine.GetChessInteractionModel().phase;
    const uint32_t staleActionToken = awaiting.pendingActionToken;
    Check(awaiting.tag == runtime::ChessInteractionPhase::AwaitingAction &&
              staleActionToken != 0U,
          "externalized Duck half waits for its recorded action result");

    const uint32_t whiteBefore =
        harness.coordinator.GetGameClock().GetRemainingMs(
            runtime::ClockSide::White);
    const uint32_t blackBefore =
        harness.coordinator.GetGameClock().GetRemainingMs(
            runtime::ClockSide::Black);
    fxe::ManualMillis::AdvanceMs(whiteBefore);
    harness.coordinator.Update();

    harness.stateMachine.HandleInteractionActionResult(
        staleActionToken, true);
    Check(harness.coordinator.ResumeExternalizedDuckActionAfterCheckpoint(),
          "stale externalized action result is safely consumable");
    harness.coordinator.Update();
    Check(harness.coordinator.HasClockTerminalOutcome() &&
              harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::ChessMove &&
              std::strcmp(harness.engine.Fen(), modules::INITIAL_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U &&
              harness.coordinator.GetGameClock().GetActiveSide() ==
                  runtime::ClockSide::White &&
              harness.coordinator.GetGameClock().GetRemainingMs(
                  runtime::ClockSide::White) == 0U &&
              harness.coordinator.GetGameClock().GetRemainingMs(
                  runtime::ClockSide::Black) == blackBefore,
          "timeout prevents stale externalized Duck continuation and all clock mutation");
}

void CheckRejectedCandidate(const char* path,
                            const uint64_t extraMask,
                            const char* context) {
    SetTime();
    Harness harness(path);
    Check(harness.LoadFresh(), "rejection harness initializes");
    const uint64_t occupancy = INITIAL_OCCUPANCY | extraMask;
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(occupancy);
    Check(!harness.coordinator.ShouldDeferInitialPlacementForVariantDetection(
              occupancy),
          context);
    fxe::ManualMillis::AdvanceMs(250U);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() != modules::GameVariant::Duck,
          context);
    Check(harness.stateMachine.TryEnterPlacementMode(), context);
    Check(harness.stateMachine.GetPlacementExtraMask() == extraMask,
          "rejected candidate remains ordinary extra-piece correction");
}

void TestInvalidActivationMatrix() {
    CheckRejectedCandidate("build/test_duck_coord_two_extra.json",
                           MaskFor(18U) | MaskFor(29U),
                           "34-piece setup never activates Duck");

    SetTime();
    Harness missing("build/test_duck_coord_missing_piece.json");
    Check(missing.LoadFresh(), "missing-piece harness initializes");
    constexpr uint8_t EXPECTED_E2 = 52U;
    constexpr uint8_t EXTRA_C6 = 18U;
    const uint64_t occupancy =
        (INITIAL_OCCUPANCY & ~MaskFor(EXPECTED_E2)) | MaskFor(EXTRA_C6);
    missing.stateMachine.ApplyPhysicalOccupancySnapshot(occupancy);
    Check(!missing.coordinator.ShouldDeferInitialPlacementForVariantDetection(
              occupancy),
          "candidate with a missing expected piece is not a fresh Duck setup");
    fxe::ManualMillis::AdvanceMs(250U);
    missing.coordinator.Update();
    Check(missing.coordinator.GetGameVariant() != modules::GameVariant::Duck,
          "missing-plus-extra setup never activates Duck");
    Check(missing.coordinator.GetInitialDuckCandidateSquare() == EXTRA_C6,
          "eligible extra retains one provisional Duck identity during correction");
    modules::SetupPresentationEvent setupEvent;
    Check(!missing.coordinator.TryTakeSetupPresentationEvent(setupEvent),
          "stable provisional Duck does not emit a board-icon presentation");
    Check(missing.stateMachine.TryEnterPlacementMode(),
          "missing-plus-extra setup enters correction");
    Check(missing.stateMachine.GetPlacementMissingMask() ==
                  MaskFor(EXPECTED_E2) &&
              missing.stateMachine.GetPlacementExtraMask() == MaskFor(EXTRA_C6),
          "correction preserves both missing and extra setup errors");

    missing.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | MaskFor(EXTRA_C6));
    fxe::ManualMillis::AdvanceMs(200U);
    missing.coordinator.Update();
    Check(missing.coordinator.GetGameVariant() == modules::GameVariant::Duck &&
              missing.coordinator.GetDuckSquare() == EXTRA_C6,
          "placing the 32nd standard piece latches the established Duck");
    Check(missing.coordinator.TryTakeSetupPresentationEvent(setupEvent) &&
              setupEvent == modules::SetupPresentationEvent::Ready,
          "32nd standard piece emits the sole setup-completion presentation");
}

void TestFirstCompletedMoveClassicLatch() {
    SetTime();
    Harness harness("build/test_duck_coord_classic_latch.json");
    Check(harness.LoadFresh(), "Classic-latch harness initializes");
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(INITIAL_OCCUPANCY);
    fxe::ManualMillis::AdvanceMs(250U);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() ==
              modules::GameVariant::Undecided,
          "stable 32-piece start remains undecided before interaction");
    harness.stateMachine.HandleSensorChange(52U, false);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Undecided,
          "first White piece lift does not lock Classic");
    modules::SetupPresentationEvent staleReady;
    Check(!harness.coordinator.TryTakeSetupPresentationEvent(staleReady),
          "first changed observation discards an undisplayed Ready event");
    harness.stateMachine.HandleSensorChange(52U, true);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Undecided,
          "returning the first pickup leaves variant selection open");

    constexpr uint8_t DUCK_C6 = 18U;
    harness.stateMachine.HandleSensorChange(DUCK_C6, true);
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Duck,
          "Duck can still be enabled after a returned chess-piece pickup");
    harness.stateMachine.HandleSensorChange(DUCK_C6, false);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Undecided,
          "Duck can be disabled again before the first completed move");

    harness.stateMachine.HandleSensorChange(52U, false);
    harness.stateMachine.HandleSensorChange(36U, true);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Classic,
          "first completed chess move locks Classic");
    Check(harness.coordinator.GetDuckSquare() == modules::INVALID_DUCK_SQUARE,
          "Classic latch has no duck square");

    const uint64_t laterExtra =
        (INITIAL_OCCUPANCY & ~MaskFor(52U)) |
        MaskFor(36U) | MaskFor(DUCK_C6);
    harness.stateMachine.HandleBoardObservation(
        MaskFor(DUCK_C6), laterExtra, fxe::Millis(),
        harness.stateMachine.GetLastBoardObservationSequence() + 1U);
    fxe::ManualMillis::AdvanceMs(250U);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() ==
              modules::GameVariant::Classic,
          "later valid-area extra cannot reclassify latched Classic game");
}

void TestUnstableCandidateSurvivesReturnedPickup() {
    SetTime();
    Harness harness("build/test_duck_coord_unstable_candidate.json");
    Check(harness.LoadFresh(), "unstable-candidate harness initializes");
    constexpr uint8_t CANDIDATE = 18U;
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(
        INITIAL_OCCUPANCY | MaskFor(CANDIDATE));
    fxe::ManualMillis::AdvanceMs(100U);
    harness.stateMachine.HandleSensorChange(52U, false);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() ==
              modules::GameVariant::Undecided,
          "first chess lift before candidate stability keeps setup reversible");
    harness.stateMachine.HandleSensorChange(52U, true);
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() ==
              modules::GameVariant::Duck,
          "returned pickup lets the stable initial Duck candidate latch");
}

void TestLogicalOrientationActivation() {
    SetTime();
    Harness harness("build/test_duck_coord_orientation.json");
    Check(harness.LoadFresh(), "orientation harness initializes");
    constexpr uint8_t LOGICAL_DUCK_F6 = 21U;
    const uint8_t physicalDuck =
        modules::MapSquareToPhysical(LOGICAL_DUCK_F6, true);
    const uint64_t physical = INITIAL_OCCUPANCY | MaskFor(physicalDuck);
    const uint64_t logical = MapPhysicalMaskToLogical(physical, true);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(logical);
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
    Check(physicalDuck == 42U,
          "rotated physical candidate lies on apparent rank 3");
    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Duck &&
              harness.coordinator.GetDuckSquare() == LOGICAL_DUCK_F6,
          "classification uses logical rank after orientation mapping");
}

void TestFirstMoveDuckObstructionGrantsFreeRelocation() {
    SetTime(3000U);
    Harness harness("build/test_duck_coord_first_move_obstruction.json");
    Check(harness.LoadFresh(true),
          "first-move obstruction harness initializes");
    harness.settings.Doc()["TIMER"] = 2U;
    constexpr uint8_t DUCK_E3 = 44U;
    constexpr uint8_t WHITE_PAWN_E2 = 52U;
    constexpr uint8_t RELOCATED_DUCK_D4 = 35U;
    LatchDuck(harness, DUCK_E3);
    const uint32_t whiteBefore =
        harness.coordinator.GetGameClock().GetRemainingMs(
            runtime::ClockSide::White);
    const uint32_t blackBefore =
        harness.coordinator.GetGameClock().GetRemainingMs(
            runtime::ClockSide::Black);

    uint64_t physical = INITIAL_OCCUPANCY | MaskFor(DUCK_E3);
    physical &= ~MaskFor(WHITE_PAWN_E2);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(WHITE_PAWN_E2), physical);
    harness.coordinator.UpdateLegalDestinationsMask();
    Check(harness.coordinator.IsInitialDuckObstructionRelocationPromptActive(),
          "a first selected piece with only Duck-blocked moves requests Vex first");
    Check(harness.stateMachine.HasHumanIllegal() &&
              harness.stateMachine.GetValidDestMask() == 0ULL,
          "blocked chess pickup must be returned before free Duck relocation");

    physical |= MaskFor(WHITE_PAWN_E2);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(WHITE_PAWN_E2), physical);
    harness.coordinator.Update();
    Check(harness.stateMachine.IsDuckRelocationActive() &&
              harness.coordinator.IsInitialDuckObstructionRelocationPromptActive(),
          "returning the selected piece opens one selectable free Duck relocation");

    physical &= ~MaskFor(DUCK_E3);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_E3), physical);
    physical |= MaskFor(RELOCATED_DUCK_D4);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(RELOCATED_DUCK_D4), physical);
    harness.coordinator.Update();
    Check(!harness.coordinator.IsInitialDuckObstructionRelocationPromptActive() &&
              harness.coordinator.GetDuckSquare() == RELOCATED_DUCK_D4,
          "legal free relocation clears the prompt and updates Vex authority");
    Check(std::strcmp(harness.engine.Fen(), modules::INITIAL_FEN) == 0 &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U &&
              harness.stateMachine.GetMovedSquaresMask() == 0ULL,
          "free relocation consumes no chess turn, move history, or moved-square state");
    Check(harness.coordinator.GetGameClock().GetRemainingMs(
              runtime::ClockSide::White) == whiteBefore &&
              harness.coordinator.GetGameClock().GetRemainingMs(
                  runtime::ClockSide::Black) == blackBefore &&
              harness.coordinator.GetGameClock().GetActiveSide() ==
                  runtime::ClockSide::White,
          "free relocation neither starts nor charges either clock");

    physical &= ~MaskFor(WHITE_PAWN_E2);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(WHITE_PAWN_E2), physical);
    harness.coordinator.UpdateLegalDestinationsMask();
    Check((harness.stateMachine.GetValidDestMask() & MaskFor(DUCK_E3)) != 0ULL,
          "the same first piece can be retried after Vex moves away");
}

uint64_t StageQuietMove(Harness& harness,
                        const uint8_t duckSquare,
                        const bool checkLegalMask) {
    uint64_t physical = INITIAL_OCCUPANCY | MaskFor(duckSquare);
    physical &= ~MaskFor(52U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(52U), physical);
    harness.coordinator.Update();
    harness.engine.legalQueryCalls = 0U;
    harness.coordinator.UpdateLegalDestinationsMask();
    if (checkLegalMask) {
        const uint64_t destinations =
            harness.stateMachine.GetValidDestMask();
        Check((destinations & MaskFor(44U)) != 0ULL &&
                  (destinations & MaskFor(36U)) != 0ULL,
              "Duck pickup exposes pseudo-legal e3/e4 mask");
        Check(harness.engine.legalQueryCalls == 0U,
              "Duck pickup bypasses orthodox engine legality");
    }
    physical |= MaskFor(36U);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(36U), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
    return physical;
}

void TestCombinedTurnTransaction() {
    SetTime();
    Harness harness("build/test_duck_coord_combined_turn.json");
    Check(harness.LoadFresh(true), "combined-turn harness initializes");
    constexpr uint8_t DUCK_FROM = 24U;
    constexpr uint8_t DUCK_TO = 25U;
    LatchDuck(harness, DUCK_FROM);
    fxe::ManualMillis::AdvanceMs(750U);
    harness.coordinator.Update();
    const uint32_t hintsAfterLatch = harness.engine.thinkStartCalls;
    const uint64_t chessMoved = StageQuietMove(harness, DUCK_FROM, true);

    Check(harness.coordinator.GetDuckTurnPhase() ==
              modules::DuckTurnPhase::DuckRelocation,
          "quiet chess move starts mandatory DuckRelocation");
    Check(std::strcmp(harness.engine.Fen(), modules::INITIAL_FEN) == 0,
          "engine FEN remains pre-move while duck relocation is pending");
    Check(harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "history remains unchanged while duck relocation is pending");
    Check(harness.coordinator.GetGameClock().GetActiveSide() ==
              runtime::ClockSide::White,
          "clock side remains White during combined turn");
    Check(harness.engine.thinkStartCalls == hintsAfterLatch,
          "Duck turn bypasses the orthodox hint-search entry point");

    runtime::RuntimeHealthSnapshot pausedHealth;
    pausedHealth.state = runtime::RuntimeHealthState::InputPaused;
    pausedHealth.causes = runtime::RuntimeHealthCauseMask(
        runtime::RuntimeHealthCause::BoardScanUncertain);
    pausedHealth.advice = runtime::RuntimeHealthAdvice{false, false, false};
    harness.coordinator.SetRuntimeHealthSnapshot(pausedHealth);
    const uint32_t healthPauseBefore =
        harness.coordinator.GetGameClock().GetRemainingMs(
            runtime::ClockSide::White);
    fxe::ManualMillis::AdvanceMs(500U);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameClock().IsPaused() &&
              harness.coordinator.GetGameClock().GetRemainingMs(
                  runtime::ClockSide::White) == healthPauseBefore &&
              harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::DuckRelocation &&
              harness.coordinator.GetGameClock().GetActiveSide() ==
                  runtime::ClockSide::White &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "health pause freezes a staged Duck turn without changing stage or side");

    harness.coordinator.SetRuntimeHealthSnapshot(
        runtime::RuntimeHealthSnapshot{});
    harness.coordinator.Update();
    Check(!harness.coordinator.GetGameClock().IsPaused() &&
              harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::DuckRelocation &&
              harness.coordinator.GetGameClock().GetActiveSide() ==
                  runtime::ClockSide::White &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "health recovery resumes the same staged Duck turn without increment");

    const uint32_t recoverySequence =
        harness.stateMachine.GetLastBoardObservationSequence() + 1U;
    harness.stateMachine.HandleRecoveryBoardObservation(
        0ULL, chessMoved, fxe::Millis(), recoverySequence);
    harness.coordinator.Update();
    Check(harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::DuckRelocation &&
              harness.stateMachine.GetChessInteractionModel().phase.tag ==
                  runtime::ChessInteractionPhase::DuckRelocation &&
              harness.coordinator.GetGameClock().GetActiveSide() ==
                  runtime::ClockSide::White &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "authoritative unchanged resync preserves the staged Duck authority");

    uint64_t duckLifted = chessMoved & ~MaskFor(DUCK_FROM);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_FROM), duckLifted);
    const uint32_t before = harness.coordinator.GetGameClock().GetRemainingMs(
        runtime::ClockSide::White);
    fxe::ManualMillis::AdvanceMs(500U);
    harness.coordinator.Update();
    const uint32_t after = harness.coordinator.GetGameClock().GetRemainingMs(
        runtime::ClockSide::White);
    Check(!harness.coordinator.GetGameClock().IsPaused() && after < before,
          "moving-side clock keeps running while duck is physically lifted");

    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_FROM), chessMoved);
    harness.coordinator.Update();
    Check(harness.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::DuckRelocation &&
              harness.coordinator.GetUndoHistoryMoveCount() == 0U,
          "returning duck to its source does not commit the turn");

    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_FROM), duckLifted);
    const uint64_t relocated = duckLifted | MaskFor(DUCK_TO);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_TO), relocated);
    harness.coordinator.Update();
    Check(std::strcmp(
              harness.engine.Fen(),
              "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1") == 0,
          "different-square duck placement commits staged engine FEN");
    Check(harness.coordinator.GetUndoHistoryMoveCount() == 1U &&
              harness.coordinator.GetDuckSquare() == DUCK_TO,
          "combined turn appends one history entry and updates duck square");
    Check(harness.coordinator.GetGameClock().GetActiveSide() ==
              runtime::ClockSide::Black,
          "clock switches only after duck relocation commits");
}

void TestRebootDuringDuckRelocation() {
    static constexpr char PATH[] =
        "build/test_duck_coord_reboot_relocation.json";
    SetTime();
    Harness before(PATH);
    Check(before.LoadFresh(), "pre-reboot harness initializes");
    constexpr uint8_t DUCK_FROM = 20U;
    constexpr uint8_t DUCK_TO = 28U;
    LatchDuck(before, DUCK_FROM);
    const uint64_t chessMoved = StageQuietMove(before, DUCK_FROM, false);
    Check(before.coordinator.FlushToSettingsNow(),
          "pending duck turn flushes before reboot");

    Harness after(PATH, false);
    Check(after.LoadExisting(), "pending duck turn restores after reboot");
    Check(after.coordinator.GetGameVariant() == modules::GameVariant::Duck &&
              after.coordinator.GetDuckTurnPhase() ==
                  modules::DuckTurnPhase::DuckRelocation,
          "reboot restores DuckRelocation phase");
    Check(std::strcmp(after.engine.Fen(), modules::INITIAL_FEN) == 0 &&
              after.coordinator.GetUndoHistoryMoveCount() == 0U,
          "reboot keeps staged chess move out of engine/history");
    after.stateMachine.ApplyPhysicalOccupancySnapshot(chessMoved);
    const uint64_t lifted = chessMoved & ~MaskFor(DUCK_FROM);
    duck_coordinator_test::Observe(
        after.stateMachine, MaskFor(DUCK_FROM), lifted);
    duck_coordinator_test::Observe(
        after.stateMachine, MaskFor(DUCK_TO), lifted | MaskFor(DUCK_TO));
    after.coordinator.Update();
    Check(after.coordinator.GetDuckSquare() == DUCK_TO &&
              after.coordinator.GetUndoHistoryMoveCount() == 1U,
          "restored relocation commits the original combined turn once");
}

void TestFowlingOutcome() {
    SetTime();
    Harness harness("build/test_duck_coord_fowling.json");
    static constexpr char FEN[] =
        "8/8/8/8/8/8/4P3/p7 w - - 0 1";
    constexpr uint8_t DUCK_FROM = 24U;
    constexpr uint8_t DUCK_TO = 25U;
    Check(duck_coordinator_test::SeedPersistedDuckPosition(
              harness, FEN, DUCK_FROM),
          "fowling Duck position persists");
    Check(harness.coordinator.LoadOrInitialize(),
          "fowling Duck position loads");
    uint64_t physical = duck_coordinator_test::FenOccupancy(FEN) |
                        MaskFor(DUCK_FROM);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    physical &= ~MaskFor(52U);
    duck_coordinator_test::Observe(harness.stateMachine, MaskFor(52U), physical);
    harness.coordinator.UpdateLegalDestinationsMask();
    physical |= MaskFor(44U);
    duck_coordinator_test::Observe(harness.stateMachine, MaskFor(44U), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();
    physical &= ~MaskFor(DUCK_FROM);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_FROM), physical);
    physical |= MaskFor(DUCK_TO);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_TO), physical);
    harness.coordinator.Update();
    const modules::VariantOutcome outcome =
        harness.coordinator.GetVariantOutcome();
    Check(outcome.reason == modules::VariantOutcomeReason::Fowled &&
              outcome.winner == modules::GameWinner::Black,
          "side beginning turn without a pseudo-legal move wins by fowling");
}

} // namespace

int main() {
    std::puts("[DUCK_COORDINATOR_INTEGRATION] start");
    fxe::ScopedMillisProviderOverride millisGuard(
        &fxe::ManualMillisProvider);
    TestValidActivationAfterStability();
    TestHomeStateOwnsProvisionalDuckBeforeLatch();
    TestNewGameDiscardsStagedDuckTurn();
    TestClockTimeoutAbandonsStagedDuckTurn();
    TestClockTimeoutClearsExternalizedDuckAction();
    TestInvalidActivationMatrix();
    TestFirstCompletedMoveClassicLatch();
    TestUnstableCandidateSurvivesReturnedPickup();
    TestLogicalOrientationActivation();
    TestFirstMoveDuckObstructionGrantsFreeRelocation();
    TestCombinedTurnTransaction();
    g_failures += RunDuckCoordinatorEarlyLiftTests();
    TestRebootDuringDuckRelocation();
    g_failures += RunDuckKingCaptureOutcomeTest();
    TestFowlingOutcome();
    g_failures += RunDuckCoordinatorCombinedTurnUndoTests();
    g_failures += RunDuckCoordinatorRecoveryCapacityTests();
    g_failures += RunDuckPseudoLegalHistoryReplayTests();
    g_failures += RunDuckCoordinatorSurfaceBlockerTests();
    if (g_failures == 0) {
        std::puts("[DUCK_COORDINATOR_INTEGRATION] ok");
        return 0;
    }
    std::printf("[DUCK_COORDINATOR_INTEGRATION] failures=%d\n", g_failures);
    return 1;
}
