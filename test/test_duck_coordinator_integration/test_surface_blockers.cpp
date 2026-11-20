#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "duck_coordinator_test_support.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/led_layout.hpp"
#include "modules/correction_feedback.hpp"
#include "modules/home_state_notification.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"
#include "runtime/interaction_model_presentation.hpp"
#include "runtime/square_mapping.hpp"

namespace {

using duck_coordinator_test::Harness;
using duck_coordinator_test::INITIAL_OCCUPANCY;

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        ++g_failures;
    }
}

constexpr uint64_t MaskFor(const uint8_t square) {
    return 1ULL << square;
}

bool RgbEq(const modules::Rgb lhs, const modules::Rgb rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

template <size_t N>
bool RgbFramesEqual(const std::array<modules::Rgb, N>& lhs,
                    const std::array<modules::Rgb, N>& rhs) {
    for (size_t i = 0U; i < N; ++i) {
        if (!RgbEq(lhs[i], rhs[i])) {
            return false;
        }
    }
    return true;
}

modules::Rgb PieceLedAt(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& frame,
    const uint8_t physicalSquare) {
    return frame[hardware::LedIndexForSquare(
        physicalSquare, hardware::SquareLedSlot::Piece)];
}

runtime::InteractionModelSnapshot BuildSnapshot(Harness& harness) {
    runtime::InteractionModelSnapshotBuildInputs inputs;
    inputs.stateMachine = &harness.stateMachine;
    inputs.derived = &harness.coordinator.GetEngineDerivedState();
    inputs.authoritativeFen = harness.coordinator.GetAuthoritativeFen();
    inputs.gameClock = &harness.coordinator.GetGameClock();
    inputs.variant = harness.coordinator.GetGameVariant();
    inputs.duckSquare = harness.coordinator.GetDuckSquare();
    inputs.initialDuckCandidateSquare =
        harness.coordinator.GetInitialDuckCandidateSquare();
    inputs.initialVariantValidationActive =
        harness.coordinator.IsInitialVariantValidationPending();
    inputs.duckTurnPhase = harness.coordinator.GetDuckTurnPhase();
    inputs.variantOutcome = harness.coordinator.GetVariantOutcome();
    return runtime::BuildInteractionModelSnapshot(inputs);
}

void ComposeSnapshot(
    Harness& harness,
    const runtime::InteractionModelSnapshot& snapshot,
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>& outEdge,
    std::array<modules::Rgb, 64>& outGrid) {
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &harness.settings;
    inputs.interactionSnapshot = &snapshot;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> frame{};
    modules::led_frame_composer::Compose(
        inputs, frame, outEdge, outGrid);
}

void DispatchObservation(Harness& harness,
                         const uint64_t changedMask,
                         const uint64_t occupiedMask) {
    runtime::InteractionDispatcher dispatcher(
        harness.stateMachine, harness.coordinator);
    dispatcher.DispatchBoardObservation({
        .tMs = fxe::Millis(),
        .sequence =
            harness.stateMachine.GetLastBoardObservationSequence() + 1U,
        .changedMask = changedMask,
        .occupiedMask = occupiedMask,
    });
}

void CheckInitialCandidateRenderBeforeLatch() {
    fxe::ManualMillis::SetNowMs(1000U);
    Harness harness("build/test_duck_coord_candidate_render.json");
    Check(harness.LoadFresh(), "candidate-render harness initializes");

    constexpr uint8_t CANDIDATE = 18U; // c6, logical coordinates.
    DispatchObservation(
        harness, MaskFor(CANDIDATE),
        INITIAL_OCCUPANCY | MaskFor(CANDIDATE));
    Check(!harness.stateMachine.HasHumanIllegal() &&
              !harness.stateMachine.HasCpuIllegal(),
          "valid Duck addition enters validation before illegal feedback");
    fxe::ManualMillis::AdvanceMs(199U);
    harness.coordinator.Update();

    const runtime::InteractionModelSnapshot snapshot =
        BuildSnapshot(harness);
    Check(snapshot.game.variant == modules::GameVariant::Undecided &&
              snapshot.game.duckSquare == modules::INVALID_DUCK_SQUARE,
          "candidate render does not prematurely latch Duck");
    Check(snapshot.game.initialDuckCandidateSquare == CANDIDATE,
          "valid pre-latch candidate reaches the render snapshot");
    Check(modules::ShouldPublishHomeStateNotification(
              harness.coordinator.GetGameVariantStateSnapshot(),
              harness.stateMachine.GetPhysicalOccupancyMask(),
              harness.coordinator.IsInitialVariantSelectionReversible()),
          "coherent provisional Duck placement remains publishable");

    for (const bool flipSides : {false, true}) {
        modules::led_frame_composer::ComposeInputs inputs;
        inputs.settings = &harness.settings;
        inputs.interactionSnapshot = &snapshot;

        modules::led_frame_composer::internal::ComposeContext context;
        Check(modules::led_frame_composer::internal::BuildComposeContext(
                  inputs, context),
              "pre-latch candidate compose context builds");
        Check(!context.invalidBoard.IsActive(),
              "pre-latch candidate is suppressed from generic mismatch cues");

        modules::BoardLedFrame board{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> edge{};
        std::array<modules::Rgb, 64> grid{};
        modules::led_frame_composer::Compose(inputs, board, edge, grid);
        const uint8_t physicalCandidate =
            runtime::MapSquareIndex(CANDIDATE, flipSides);
        const auto physical = hardware::MapSemanticLedFrame(
            {hardware::BoardRevision::V2S3, flipSides, false}, board, edge);
        Check(RgbEq(modules::RoundedPieceAverage(board[CANDIDATE]),
                    modules::RGB_YELLOW) &&
                  RgbEq(physical[hardware::LedIndexForSquare(
                            physicalCandidate, hardware::SquareLedSlot::Piece)],
                        modules::RGB_YELLOW),
              flipSides
                  ? "flipped pre-latch candidate is canonical yellow and maps to the physical center"
                  : "pre-latch candidate is canonical yellow and maps to the physical center");
    }
}

void CheckInitialDuckRemovalDeactivatesVariant() {
    fxe::ManualMillis::SetNowMs(1500U);
    Harness harness("build/test_duck_coord_initial_removal.json");
    Check(harness.LoadFresh(), "initial-removal harness initializes");
    harness.settings.Doc()["P1_AUTO_HINTS"] = false;
    harness.settings.Doc()["P2_AUTO_HINTS"] = false;
    constexpr uint8_t DUCK = 25U; // b5, from the reported untouched opening.
    const uint64_t withDuck = INITIAL_OCCUPANCY | MaskFor(DUCK);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(withDuck);
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Duck,
          "initial-removal fixture latches Duck");

    DispatchObservation(harness, MaskFor(DUCK), INITIAL_OCCUPANCY);
    const runtime::InteractionModelSnapshot validatingRemoval =
        BuildSnapshot(harness);
    modules::led_frame_composer::ComposeInputs validatingInputs;
    validatingInputs.settings = &harness.settings;
    validatingInputs.interactionSnapshot = &validatingRemoval;
    modules::led_frame_composer::internal::ComposeContext validatingContext;
    Check(modules::led_frame_composer::internal::BuildComposeContext(
              validatingInputs, validatingContext) &&
              validatingRemoval.game.initialVariantValidationActive &&
              !validatingContext.invalidBoard.IsActive() &&
              !harness.stateMachine.HasHumanIllegal() &&
              !harness.stateMachine.HasCpuIllegal(),
          "initial Duck removal validates before missing-piece feedback");
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> removalFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> removalEdge{};
    std::array<modules::Rgb, 64> removalGrid{};
    modules::led_frame_composer::Compose(
        validatingInputs, removalFrame, removalEdge, removalGrid);
    Check(RgbEq(PieceLedAt(removalFrame, DUCK), modules::RGB_BLACK),
          "initial Duck center turns off in the first post-removal frame");
    const modules::GameVariantStateSnapshot validatingVariant =
        harness.coordinator.GetGameVariantStateSnapshot();
    Check(!modules::ShouldPublishHomeStateNotification(
              validatingVariant,
              harness.stateMachine.GetPhysicalOccupancyMask(),
              harness.coordinator.IsInitialVariantSelectionReversible()),
          "initial Duck removal withholds the classification-pending Home snapshot");

    uint32_t queuedSaves = 0U;
    harness.settings.SetAsyncSaveRequestCallback(
        [&queuedSaves](const JsonDocument&) {
            ++queuedSaves;
            return true;
    });
    // Firmware publishes again from ReduceInteraction during the reset. A
    // captured old expected square must stay calm for the animation task's
    // entire hold, not merely at the callback's first instant.
    struct RetirementCapture {
        Harness* harness;
        uint32_t frames = 0U;
    } retirement{&harness};
    harness.stateMachine.SetHintPresentationObserver(nullptr, [](void* context) {
        auto& capture = *static_cast<RetirementCapture*>(context);
        auto snapshot = BuildSnapshot(*capture.harness);
        if (snapshot.game.variant != modules::GameVariant::Undecided ||
            snapshot.board.expectedOccupiedMask == INITIAL_OCCUPANCY) return;
        ++capture.frames;
        snapshot.game.setupLifecycle = runtime::SetupLifecycle::FreshSetup;
        runtime::InteractionModelPresentation published;
        runtime::CaptureInteractionModelPresentation(capture.harness->stateMachine,
                                                    snapshot, published);
        for (const uint32_t elapsed : {0U, modules::CORRECTION_QUIET_MS,
                                      modules::CORRECTION_STRONG_MS}) {
            const auto sampled = runtime::SampleInteractionModelPresentation(
                published, snapshot.system.nowMs + elapsed);
            modules::led_frame_composer::ComposeInputs inputs;
            inputs.settings = &capture.harness->settings;
            inputs.interactionSnapshot = &sampled;
            modules::BoardLedFrame board{};
            std::array<modules::Rgb, modules::EDGE_LED_COUNT> edge{};
            std::array<modules::Rgb, 64U> grid{};
            modules::led_frame_composer::Compose(inputs, board, edge, grid);
            const auto& oldSquare = board[25U];
            const auto& normalSquare = board[18U];
            Check(RgbEq(oldSquare.left, normalSquare.left) &&
                      RgbEq(oldSquare.right, normalSquare.right) &&
                      RgbEq(oldSquare.top, modules::RGB_BLACK) &&
                      RgbEq(oldSquare.bottom, modules::RGB_BLACK),
                  "retained initial Vex-removal frame never ages into correction");
            std::printf("[INITIAL_VEX_REMOVAL] age=%u physical=%016llx expected=%016llx validation=%u hint=%u b5=%u,%u,%u/%u,%u,%u mode=FreshSetup/Idle result=accepted next=arrange-or-play expiry=none\n",
                elapsed, static_cast<unsigned long long>(sampled.board.physicalOccupiedMask),
                static_cast<unsigned long long>(sampled.board.expectedOccupiedMask),
                static_cast<unsigned>(sampled.game.initialVariantValidationActive),
                static_cast<unsigned>(sampled.render.hintDisplayRequested),
                oldSquare.left.r,oldSquare.left.g,oldSquare.left.b,oldSquare.right.r,oldSquare.right.g,oldSquare.right.b);
        }
    }, &retirement);
    harness.coordinator.Update();
    harness.stateMachine.SetHintPresentationObserver(nullptr, nullptr, nullptr);
    Check(retirement.frames != 0U,
          "initial Vex retirement exercises firmware's reentrant publication seam");
    Check(queuedSaves == 0U,
          "initial Duck removal defers persistence into the shared debounce window");
    const modules::GameVariantStateSnapshot removedVariant =
        harness.coordinator.GetGameVariantStateSnapshot();
    Check(modules::ShouldPublishHomeStateNotification(
              removedVariant,
              harness.stateMachine.GetPhysicalOccupancyMask(),
              harness.coordinator.IsInitialVariantSelectionReversible()) &&
              removedVariant.variant == modules::GameVariant::Undecided &&
              removedVariant.duckSquare == modules::INVALID_DUCK_SQUARE &&
              harness.stateMachine.GetPhysicalOccupancyMask() ==
                  INITIAL_OCCUPANCY,
          "the next publishable Home snapshot is coherent Classic-ready setup");
    Check(harness.coordinator.GetGameVariant() ==
                  modules::GameVariant::Undecided &&
              harness.coordinator.GetDuckSquare() ==
                  modules::INVALID_DUCK_SQUARE,
          "removing the initial duck returns to undecided Classic-ready setup");
    Check(harness.stateMachine.GetExpectedOccupancyMask() ==
                  INITIAL_OCCUPANCY &&
              !harness.stateMachine.IsPlacementModeActive() &&
              !harness.stateMachine.HasHumanIllegal(),
          "initial duck removal clears the obsolete missing-square correction");

    harness.stateMachine.HandleSensorChange(52U, false);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Undecided,
          "first chess pickup after duck removal keeps setup reversible");
    harness.stateMachine.HandleSensorChange(52U, true);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK), withDuck);
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Duck,
          "returning the piece and replacing the duck re-enables Duck");
    Check(queuedSaves == 0U,
          "Duck add/remove burst remains inside the debounce window");
    fxe::ManualMillis::AdvanceMs(
        harness.settingsConfig.mustSaveDelayMs - 1U);
    harness.settings.Update();
    Check(queuedSaves == 0U,
          "Duck add/remove burst waits through 99 ms");
    fxe::ManualMillis::AdvanceMs(1U);
    harness.settings.Update();
    Check(queuedSaves == 1U,
          "Duck add/remove burst coalesces into one 100 ms save request");
}

void CheckInitialDuckRemovalBeforeLatchNeverFlashesInvalid() {
    fxe::ManualMillis::SetNowMs(1650U);
    Harness harness("build/test_duck_coord_candidate_quick_removal.json");
    Check(harness.LoadFresh(), "quick-removal harness initializes");
    constexpr uint8_t DUCK = 18U;
    const uint64_t withDuck = INITIAL_OCCUPANCY | MaskFor(DUCK);

    DispatchObservation(harness, MaskFor(DUCK), withDuck);
    Check(harness.coordinator.IsInitialVariantValidationPending(),
          "new Duck candidate is owned by validation");
    DispatchObservation(harness, MaskFor(DUCK), INITIAL_OCCUPANCY);

    const runtime::InteractionModelSnapshot snapshot = BuildSnapshot(harness);
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &harness.settings;
    inputs.interactionSnapshot = &snapshot;
    modules::led_frame_composer::internal::ComposeContext context;
    Check(modules::led_frame_composer::internal::BuildComposeContext(
              inputs, context) &&
              snapshot.game.initialVariantValidationActive &&
              !context.invalidBoard.IsActive() &&
              !harness.stateMachine.HasHumanIllegal() &&
              !harness.stateMachine.HasCpuIllegal(),
          "removing a pre-latch Duck candidate has no invalid frame");
    Check(harness.coordinator.GetGameVariant() ==
                  modules::GameVariant::Undecided &&
              harness.stateMachine.GetPhysicalOccupancyMask() ==
                  INITIAL_OCCUPANCY,
          "quick candidate removal restores untouched fresh setup");
}

void CheckInitialDuckReplacementPublishesAtomically() {
    fxe::ManualMillis::SetNowMs(1725U);
    Harness harness("build/test_duck_coord_initial_replacement.json");
    Check(harness.LoadFresh(), "initial-replacement harness initializes");
    constexpr uint8_t OLD_DUCK = 18U; // c6, logical coordinates.
    constexpr uint8_t NEW_DUCK = 27U; // d5, logical coordinates.
    uint64_t physical = INITIAL_OCCUPANCY | MaskFor(OLD_DUCK);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Duck &&
              harness.coordinator.GetDuckSquare() == OLD_DUCK,
          "initial-replacement fixture latches the original Duck");

    physical |= MaskFor(NEW_DUCK);
    DispatchObservation(harness, MaskFor(NEW_DUCK), physical);
    physical &= ~MaskFor(OLD_DUCK);
    DispatchObservation(harness, MaskFor(OLD_DUCK), physical);

    const modules::GameVariantStateSnapshot staleVariant =
        harness.coordinator.GetGameVariantStateSnapshot();
    Check(!harness.coordinator.IsInitialVariantValidationPending() &&
              harness.coordinator.IsInitialVariantSelectionReversible(),
          "replacement removal exposes the wider reversible boundary");
    Check(!modules::ShouldPublishHomeStateNotification(
              staleVariant,
              harness.stateMachine.GetPhysicalOccupancyMask(),
              harness.coordinator.IsInitialVariantSelectionReversible()),
          "replacement removal withholds the stale old-Duck Home snapshot");

    harness.coordinator.Update();
    const modules::GameVariantStateSnapshot provisionalVariant =
        harness.coordinator.GetGameVariantStateSnapshot();
    Check(provisionalVariant.variant == modules::GameVariant::Undecided &&
              provisionalVariant.duckSquare ==
                  modules::INVALID_DUCK_SQUARE &&
              harness.coordinator.GetInitialDuckCandidateSquare() ==
                  NEW_DUCK &&
              modules::HomeStateDuckSquareForPresentation(
                  provisionalVariant,
                  harness.coordinator.GetInitialDuckCandidateSquare()) ==
                  NEW_DUCK &&
              modules::ShouldPublishHomeStateNotification(
                  provisionalVariant,
                  harness.stateMachine.GetPhysicalOccupancyMask(),
                  harness.coordinator.IsInitialVariantSelectionReversible()) &&
              !harness.stateMachine.HasHumanIllegal() &&
              !harness.stateMachine.HasCpuIllegal(),
          "first publishable replacement snapshot owns the provisional new Duck");

    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Duck &&
              harness.coordinator.GetDuckSquare() == NEW_DUCK,
          "stable replacement Duck latches without an intermediate correction");
}

void CheckValidCandidateSupersedesSetupCorrection() {
    fxe::ManualMillis::SetNowMs(1750U);
    Harness harness("build/test_duck_coord_validation_cutover.json");
    Check(harness.LoadFresh(), "validation-cutover harness initializes");
    constexpr uint8_t FIRST_EXTRA = 35U;
    constexpr uint8_t VALID_RANK3 = 42U;
    const uint64_t invalidSetup = INITIAL_OCCUPANCY |
                                  MaskFor(FIRST_EXTRA) |
                                  MaskFor(VALID_RANK3);
    DispatchObservation(
        harness, MaskFor(FIRST_EXTRA) | MaskFor(VALID_RANK3), invalidSetup);
    Check(!harness.coordinator.IsInitialVariantValidationPending() &&
              harness.stateMachine.HasHumanIllegal(),
          "multiple opening extras retain ordinary correction feedback");
    Check(modules::ShouldPublishHomeStateNotification(
              harness.coordinator.GetGameVariantStateSnapshot(),
              harness.stateMachine.GetPhysicalOccupancyMask(),
              harness.coordinator.IsInitialVariantSelectionReversible()),
          "ordinary invalid setup extras remain publishable");
    Check(harness.stateMachine.ForceEnterPlacementMode(),
          "invalid setup fixture enters placement correction");

    const uint64_t validSetup = INITIAL_OCCUPANCY | MaskFor(VALID_RANK3);
    DispatchObservation(
        harness,
        MaskFor(FIRST_EXTRA),
        validSetup);
    Check(harness.coordinator.IsInitialVariantValidationPending() &&
              !harness.stateMachine.IsPlacementModeActive() &&
              !harness.stateMachine.HasHumanIllegal() &&
              !harness.stateMachine.HasCpuIllegal(),
          "valid candidate transitions out of prior setup correction");

    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();
    Check(harness.coordinator.GetGameVariant() == modules::GameVariant::Duck &&
              harness.coordinator.GetDuckSquare() == VALID_RANK3,
          "validated candidate latches after correction cutover");
}

void CheckDuckRelocationPreservesTurnPresentation() {
    fxe::ManualMillis::SetNowMs(2250U);
    Harness harness("build/test_duck_coord_turn_presentation.json");
    Check(harness.LoadFresh(), "turn-presentation harness initializes");
    constexpr uint8_t DUCK = 24U;
    constexpr uint8_t MOVE_FROM = 52U;
    constexpr uint8_t MOVE_TO = 36U;
    uint64_t physical = INITIAL_OCCUPANCY | MaskFor(DUCK);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    fxe::ManualMillis::AdvanceMs(200U);
    harness.coordinator.Update();

    physical &= ~MaskFor(MOVE_FROM);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(MOVE_FROM), physical);
    physical |= MaskFor(MOVE_TO);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(MOVE_TO), physical);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    harness.coordinator.Update();

    const runtime::InteractionModelSnapshot snapshot = BuildSnapshot(harness);
    Check(snapshot.game.duckTurnPhase ==
                  modules::DuckTurnPhase::DuckRelocation &&
              snapshot.render.whiteLastMoveValid &&
              snapshot.render.whiteLastMoveFromSquare == MOVE_FROM &&
              snapshot.render.whiteLastMoveToSquare == MOVE_TO &&
              snapshot.render.lastMoveMostRecentKnown,
          "staged Duck chess move remains the highlighted last move");

    std::array<modules::Rgb, modules::EDGE_LED_COUNT> duckEdge{};
    std::array<modules::Rgb, 64> duckGrid{};
    ComposeSnapshot(harness, snapshot, duckEdge, duckGrid);
    runtime::InteractionModelSnapshot idleSnapshot = snapshot;
    idleSnapshot.interaction.taggedState = runtime::TaggedInteractionState{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> idleEdge{};
    std::array<modules::Rgb, 64> idleGrid{};
    ComposeSnapshot(harness, idleSnapshot, idleEdge, idleGrid);
    Check(RgbFramesEqual(duckEdge, idleEdge),
          "duck relocation keeps the moving player's idle-turn edge animation");

    runtime::InteractionModelSnapshot noLastMove = snapshot;
    noLastMove.render.whiteLastMoveValid = false;
    noLastMove.render.lastMoveMostRecentKnown = false;
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> noLastEdge{};
    std::array<modules::Rgb, 64> noLastGrid{};
    ComposeSnapshot(harness, noLastMove, noLastEdge, noLastGrid);
    Check(!RgbEq(duckGrid[MOVE_FROM], noLastGrid[MOVE_FROM]) ||
              !RgbEq(duckGrid[MOVE_TO], noLastGrid[MOVE_TO]),
          "duck relocation visibly retains the staged move highlight");

    constexpr uint8_t DUCK_TO = 25U;
    physical &= ~MaskFor(DUCK);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK), physical);
    physical |= MaskFor(DUCK_TO);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_TO), physical);
    harness.coordinator.Update();
    Check(harness.coordinator.GetDuckSquare() == DUCK_TO &&
              harness.coordinator.GetUndoHistoryMoveCount() == 1U,
          "first compound Duck turn commits before irreversible correction coverage");

    physical &= ~MaskFor(DUCK_TO);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(DUCK_TO), physical);
    const modules::GameVariantStateSnapshot irreversibleVariant =
        harness.coordinator.GetGameVariantStateSnapshot();
    Check(!harness.coordinator.IsInitialVariantSelectionReversible() &&
              modules::ShouldPublishHomeStateNotification(
                  irreversibleVariant,
                  harness.stateMachine.GetPhysicalOccupancyMask(),
                  harness.coordinator.IsInitialVariantSelectionReversible()) &&
              irreversibleVariant.variant == modules::GameVariant::Duck &&
              irreversibleVariant.duckSquare == DUCK_TO &&
              (harness.stateMachine.GetPhysicalOccupancyMask() &
               MaskFor(DUCK_TO)) == 0ULL,
          "missing Duck after the first turn remains a publishable real correction");
}

void CheckTerminalOutcomeRejectsLosingPiecePickup(
    const modules::VariantOutcomeReason reason,
    const char* path) {
    fxe::ManualMillis::SetNowMs(2000U);
    Harness harness(path);
    static constexpr char FEN[] =
        "r7/8/8/8/8/8/8/4K3 b - - 0 1";
    constexpr uint8_t DUCK = 24U;
    constexpr uint8_t LOSING_ROOK = 0U;
    constexpr uint8_t ROOK_DESTINATION = 8U;
    const modules::VariantOutcome terminal{
        reason, modules::GameWinner::White};

    Check(duck_coordinator_test::SeedPersistedDuckPosition(
              harness, FEN, DUCK, terminal) &&
              harness.coordinator.LoadOrInitialize(),
          "terminal Duck fixture loads");
    uint64_t physical =
        duck_coordinator_test::FenOccupancy(FEN) | MaskFor(DUCK);
    harness.stateMachine.ApplyPhysicalOccupancySnapshot(physical);
    physical &= ~MaskFor(LOSING_ROOK);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(LOSING_ROOK), physical);
    Check(harness.stateMachine.IsHumanPieceInHand(),
          "remaining losing-side piece can be physically lifted");

    harness.engine.legalQueryCalls = 0U;
    harness.coordinator.UpdateLegalDestinationsMask();
    Check(harness.stateMachine.GetValidDestMask() == 0ULL &&
              harness.engine.legalQueryCalls == 0U,
          "terminal Duck pickup is gated before legal generation");
    Check((harness.stateMachine.GetHumanIllegalMask() &
           MaskFor(LOSING_ROOK)) != 0ULL &&
              !harness.stateMachine.IsHumanPieceInHand(),
          "terminal Duck pickup is canceled into return-piece correction");
    Check(harness.coordinator.GetUndoHistoryMoveCount() == 0U &&
              std::strcmp(harness.engine.Fen(), FEN) == 0,
          "terminal pickup cannot stage or commit choreography");

    physical |= MaskFor(LOSING_ROOK);
    duck_coordinator_test::Observe(
        harness.stateMachine, MaskFor(LOSING_ROOK), physical);
    Check(!harness.stateMachine.HasHumanIllegal() &&
              harness.coordinator.GetVariantOutcome().reason == reason,
          "returning the blocked piece restores the frozen terminal board");

    const uint64_t atomicMove =
        (physical & ~MaskFor(LOSING_ROOK)) | MaskFor(ROOK_DESTINATION);
    duck_coordinator_test::Observe(
        harness.stateMachine,
        MaskFor(LOSING_ROOK) | MaskFor(ROOK_DESTINATION),
        atomicMove);
    fxe::ManualMillis::AdvanceMs(10U);
    harness.stateMachine.Tick();
    Check(harness.stateMachine.HasCommittedMovePending(),
          "atomic terminal move reaches the coordinator action boundary");
    harness.coordinator.Update();
    Check(harness.coordinator.GetUndoHistoryMoveCount() == 0U &&
              std::strcmp(harness.engine.Fen(), FEN) == 0 &&
              !harness.stateMachine.IsPromotionActive(),
          "atomic terminal move is rejected before choreography commit");
    Check(harness.coordinator.GetVariantOutcome().reason == reason,
          "atomic terminal move preserves the terminal outcome");
}

} // namespace

int RunDuckCoordinatorSurfaceBlockerTests() {
    CheckInitialCandidateRenderBeforeLatch();
    CheckInitialDuckRemovalDeactivatesVariant();
    CheckInitialDuckRemovalBeforeLatchNeverFlashesInvalid();
    CheckInitialDuckReplacementPublishesAtomically();
    CheckValidCandidateSupersedesSetupCorrection();
    CheckDuckRelocationPreservesTurnPresentation();
    CheckTerminalOutcomeRejectsLosingPiecePickup(
        modules::VariantOutcomeReason::KingCaptured,
        "build/test_duck_coord_terminal_king_capture.json");
    CheckTerminalOutcomeRejectsLosingPiecePickup(
        modules::VariantOutcomeReason::Fowled,
        "build/test_duck_coord_terminal_fowled.json");
    return g_failures;
}
