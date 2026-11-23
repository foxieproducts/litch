#include <array>
#include <cstdint>
#include <cstdio>

#include "hardware/led_layout.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/game_variant.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "modules/led_render_policy.hpp"
#include "runtime/interaction_domain_types.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"
#include "runtime/square_mapping.hpp"
#include "../../src/virtual_litch_session_internal.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

bool RgbEq(const modules::Rgb lhs, const modules::Rgb rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

uint16_t RgbEnergy(const modules::Rgb rgb) {
    return static_cast<uint16_t>(rgb.r) +
           static_cast<uint16_t>(rgb.g) +
           static_cast<uint16_t>(rgb.b);
}

modules::Rgb LedAt(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& frame,
                   const uint8_t physicalSquare,
                   const hardware::SquareLedSlot slot) {
    return frame[hardware::LedIndexForSquare(physicalSquare, slot)];
}

runtime::InteractionModelSnapshot MakeSnapshot(const uint64_t expectedMask,
                                               const uint64_t physicalMask) {
    runtime::InteractionModelSnapshot snapshot;
    snapshot.board.expectedOccupiedMask = expectedMask;
    snapshot.board.physicalOccupiedMask = physicalMask;
    snapshot.system.nowMs = 0U;
    return snapshot;
}

modules::led_frame_composer::ComposeInputs MakeInputs(
    const fxe::SettingsStore& settings,
    const runtime::InteractionModelSnapshot& snapshot,
    const bool flipSides) {
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &settings;
    inputs.interactionSnapshot = &snapshot;
    return inputs;
}

void Compose(const modules::led_frame_composer::ComposeInputs& inputs,
             std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame) {
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};
    modules::led_frame_composer::Compose(
        inputs, ledFrame, edgeFrame, gridFrame);
}

modules::led_frame_composer::internal::InvalidBoardState InvalidBoardFor(
    const modules::led_frame_composer::ComposeInputs& inputs) {
    modules::led_frame_composer::internal::ComposeContext context;
    if (!modules::led_frame_composer::internal::BuildComposeContext(
            inputs, context)) {
        Check(false, "compose context builds");
        return {};
    }
    return context.invalidBoard;
}

void CheckDuckYellowAtBothOrientations(const fxe::SettingsStore& settings) {
    static constexpr uint8_t CHESS_SQUARE = 0U;
    static constexpr uint8_t DUCK_SQUARE = 18U; // c6 in logical coordinates.
    const uint64_t expectedMask = 1ULL << CHESS_SQUARE;
    runtime::InteractionModelSnapshot snapshot =
        MakeSnapshot(expectedMask, expectedMask | (1ULL << DUCK_SQUARE));
    snapshot.board.enginePiecesValid = true;
    snapshot.board.enginePieces[CHESS_SQUARE] = 'r';
    snapshot.game.variant = modules::GameVariant::Duck;
    snapshot.game.duckSquare = DUCK_SQUARE;

    for (const bool flipSides : {false, true}) {
        const auto inputs = MakeInputs(settings, snapshot, flipSides);
        const auto invalidBoard = InvalidBoardFor(inputs);
        Check(!invalidBoard.IsActive(),
              flipSides ? "flipped duck is not an extra-piece mismatch"
                        : "duck is not an extra-piece mismatch");

        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
        Compose(inputs, ledFrame);
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> emptyStatus{};
        const auto physical = hardware::MapSemanticLedFrame(
            {hardware::BoardRevision::V2S3, flipSides, flipSides},
            ledFrame, emptyStatus);
        const uint8_t physicalDuckSquare =
            runtime::MapSquareIndex(DUCK_SQUARE, flipSides);
        Check(RgbEq(physical[hardware::LedIndexForSquare(
                          physicalDuckSquare,
                          hardware::SquareLedSlot::Piece)],
                    modules::RGB_YELLOW),
              flipSides ? "flipped logical duck center LED is yellow"
                        : "logical duck center LED is yellow");
    }
}

void CheckClassicAndUnclassifiedExtrasRemainFaults(
    const fxe::SettingsStore& settings) {
    static constexpr uint8_t MATCHED_SQUARE = 0U;
    static constexpr uint8_t RANK_4_EXTRA = 32U; // a4.
    static constexpr uint8_t RANK_3_EXTRA = 40U; // a3.
    const uint64_t expectedMask = 1ULL << MATCHED_SQUARE;

    for (const uint8_t extraSquare : {RANK_4_EXTRA, RANK_3_EXTRA}) {
        runtime::InteractionModelSnapshot snapshot =
            MakeSnapshot(expectedMask,
                         expectedMask | (1ULL << extraSquare));
        snapshot.game.variant = modules::GameVariant::Undecided;

        const auto inputs = MakeInputs(settings, snapshot, false);
        const auto invalidBoard = InvalidBoardFor(inputs);
        Check(invalidBoard.IsActive(),
              "an extra without authoritative Vex identity remains invalid");
        Check((invalidBoard.extraMask & (1ULL << extraSquare)) != 0ULL,
              "undecided setup extra remains in mismatch mask");
    }

    runtime::InteractionModelSnapshot classicSnapshot =
        MakeSnapshot(expectedMask,
                     expectedMask | (1ULL << RANK_4_EXTRA));
    Check(classicSnapshot.game.variant == modules::GameVariant::Classic,
          "legacy snapshot defaults to Classic rendering");
    const auto classicInputs = MakeInputs(settings, classicSnapshot, false);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    Compose(classicInputs, ledFrame);
    Check(!RgbEq(LedAt(ledFrame,
                       RANK_4_EXTRA,
                       hardware::SquareLedSlot::Piece),
                 modules::RGB_YELLOW),
          "Classic extra keeps mismatch rainbow instead of duck yellow");
}

void CheckMissingExpectedDuckRemainsACorrection(
    const fxe::SettingsStore& settings) {
    static constexpr uint8_t CHESS_SQUARE = 0U;
    static constexpr uint8_t DUCK_SQUARE = 18U;
    const uint64_t chessMask = 1ULL << CHESS_SQUARE;
    runtime::InteractionModelSnapshot snapshot =
        MakeSnapshot(chessMask | (1ULL << DUCK_SQUARE), chessMask);
    snapshot.game.variant = modules::GameVariant::Duck;
    snapshot.game.duckSquare = DUCK_SQUARE;

    const auto invalidBoard =
        InvalidBoardFor(MakeInputs(settings, snapshot, false));
    Check(invalidBoard.IsActive(),
          "missing expected duck remains a correction mismatch");
    Check((invalidBoard.missingMask & (1ULL << DUCK_SQUARE)) != 0ULL,
          "missing expected duck remains in missing mask");
}

void CheckOrthodoxThreatIntentSuppressedForDuck(
    const fxe::SettingsStore& settings) {
    static constexpr uint8_t KING_SQUARE = 60U; // e1, a dark square.
    static constexpr uint64_t CHECKERS = (UINT64_C(1) << 4U) | (UINT64_C(1) << 33U);
    static constexpr uint64_t OCCUPIED = CHECKERS | (UINT64_C(1) << KING_SQUARE) | (UINT64_C(1) << 7U);
    static constexpr const char* FEN = "4r2k/8/8/8/1b6/8/8/4K3 w - - 0 1";
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    modules::LitchMaxEngine engine;
    Check(engine.SetFen(FEN), "real double-check position loads");
    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);
    runtime::GameStateMachine state;
    state.Reset();
    state.SetExpectedOccupancyMask(OCCUPIED);
    state.SetExpectedMoverAndOpponentOccupancyMasks(UINT64_C(1) << KING_SQUARE,
                                                   OCCUPIED & ~(UINT64_C(1) << KING_SQUARE));
    state.DebugSetPhysicalOccupancyMask(OCCUPIED);
    runtime::InteractionModelSnapshotBuildInputs snapshotInputs;
    snapshotInputs.stateMachine = &state;
    snapshotInputs.derived = &derived;
    snapshotInputs.authoritativeFen = FEN;
    auto classic = runtime::BuildInteractionModelSnapshot(snapshotInputs);
    classic.system.nowMs = 190U;

    runtime::InteractionModelSnapshot duck = classic;
    duck.game.variant = modules::GameVariant::Duck;

    runtime::InteractionModelSnapshot classicPlain = classic;
    runtime::InteractionModelSnapshot duckPlain = duck;
    classicPlain.game.inCheck = false;
    duckPlain.game.inCheck = false;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> classicFrame{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> duckFrame{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> classicPlainFrame{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> duckPlainFrame{};
    Compose(MakeInputs(settings, classic, false), classicFrame);
    Compose(MakeInputs(settings, duck, false), duckFrame);
    Compose(MakeInputs(settings, classicPlain, false), classicPlainFrame);
    Compose(MakeInputs(settings, duckPlain, false), duckPlainFrame);

    Check(classic.game.checkingPiecesMask == CHECKERS && classic.game.checkStartValid &&
              !RgbEq(LedAt(classicFrame, KING_SQUARE, hardware::SquareLedSlot::Piece),
                 LedAt(classicPlainFrame, KING_SQUARE, hardware::SquareLedSlot::Piece)),
          "real double-check mask reaches the immutable snapshot and begins with the king's white call");
    for (uint8_t square = 0U; square < 64U; ++square) {
        for (const auto slot : {hardware::SquareLedSlot::Square0,
                                hardware::SquareLedSlot::Piece,
                                hardware::SquareLedSlot::Square2}) {
            Check(RgbEq(LedAt(duckFrame, square, slot),
                        LedAt(duckPlainFrame, square, slot)),
                  "Duck suppresses every orthodox check emitter");
            if (square != KING_SQUARE || slot != hardware::SquareLedSlot::Piece) {
                Check(RgbEq(LedAt(classicFrame, square, slot),
                            LedAt(classicPlainFrame, square, slot)),
                      "Classic check leaves square LEDs and neighboring pieces unchanged");
            }
        }
    }
    Check(RgbEq(LedAt(duckFrame, KING_SQUARE, hardware::SquareLedSlot::Square0),
                modules::RGB_BLACK),
          "Duck checked king stays on its normal dark-square background");

    classic.system.nowMs = 750U;
    Compose(MakeInputs(settings, classic, false), classicFrame);
    Check(!RgbEq(LedAt(classicFrame, 4U, hardware::SquareLedSlot::Piece),
                 LedAt(classicPlainFrame, 4U, hardware::SquareLedSlot::Piece)) &&
              !RgbEq(LedAt(classicFrame, 33U, hardware::SquareLedSlot::Piece),
                     LedAt(classicPlainFrame, 33U, hardware::SquareLedSlot::Piece)) &&
              RgbEq(LedAt(classicFrame, KING_SQUARE, hardware::SquareLedSlot::Piece),
                    LedAt(classicPlainFrame, KING_SQUARE, hardware::SquareLedSlot::Piece)),
          "both actual checkers answer together in white after the king returns to its ordinary color");

    // Supporting white-palette injection reuses the actual compositor, V3.3
    // mapping and output pipeline; the existing raw Check journey owns inputs.
    const auto whiteOutput = [&](const uint32_t elapsed, const bool minimal) {
        auto sample = classic;
        sample.system.nowMs = elapsed;
        modules::led_frame_composer::internal::ComposeContext context;
        Check(modules::led_frame_composer::internal::BuildComposeContext(
            MakeInputs(settings, sample, false), context), "white-palette context builds");
        context.p1PieceRgb = modules::RGB_WHITE;
        context.p2PieceRgb = modules::RGB_WHITE;
        context.minimalAnimations = minimal;
        modules::BoardLedFrame board{};
        std::array<modules::Rgb, 64U> grid{};
        modules::led_frame_composer::internal::ComposeBoard(context, board, grid);
        modules::led_frame_composer::internal::ApplyGlobalBrightnessBoost(board);
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
        const auto physical = hardware::MapSemanticLedFrame(
            {hardware::BoardRevision::V3_3S3, false, false}, board, status);
        runtime::virtual_litch_internal::CaptureLedRenderer renderer(300U, false);
        modules::PowerBudgetPolicy policy(modules::led_render_policy::MakePowerBudgetPolicyConfig({true, false}));
        auto pipelineConfig = modules::led_render_policy::MakePipelineConfig(false, 32U);
        pipelineConfig.channelMaxCurrentMa = 20U;
        pipelineConfig.outputBrightnessScale256 = hardware::BoardHardwareProfileFor(
            hardware::BoardRevision::V3_3S3).ledOutputScale256;
        modules::BudgetAwareLedPipeline pipeline(renderer, policy, pipelineConfig);
        pipeline.SetBrightnessScale01(0.5f);
        pipeline.RenderFrame(physical.data(), 300U);
        std::array<virtual_litch_color, 3U> result{};
        const std::array<uint8_t, 3U> squares{KING_SQUARE, 4U, 33U};
        for (uint8_t index = 0U; index < squares.size(); ++index) {
            const auto targets = hardware::MapSemanticSquareLedTargets(
                hardware::BoardRevision::V3_3S3, squares[index], hardware::SquareLedSlot::Piece, false);
            result[index] = renderer.Pixels()[targets.indices[0U]];
        }
        return result;
    };
    const auto whiteDip = whiteOutput(0U, false);
    const auto whiteKing = whiteOutput(190U, false);
    const auto whiteAnswer = whiteOutput(750U, false);
    const auto whiteRest = whiteOutput(1100U, false);
    const auto whiteMinimal = whiteOutput(0U, true);
    Check(whiteDip[0U].r > 0U && whiteDip[0U].r < whiteRest[0U].r &&
              whiteKing[0U].r > whiteDip[0U].r && whiteAnswer[1U].r > whiteDip[0U].r &&
              whiteAnswer[1U].r == whiteAnswer[2U].r &&
              whiteMinimal[0U].r < whiteRest[0U].r &&
              whiteMinimal[1U].r < whiteMinimal[0U].r && whiteMinimal[1U].r > 0U,
          "actual mapped white pieces keep a nonzero contrast dip and distinct static king/checker levels after output gains");
    std::printf("[CHECK_WHITE_V33_OUTPUT] user_scale=128 ordinary=%u dip=%u king_call=%u checker_answer=%u minimal_king=%u minimal_checker=%u physical_appearance=pending\n",
        whiteRest[0U].r, whiteDip[0U].r, whiteKing[0U].r, whiteAnswer[1U].r,
        whiteMinimal[0U].r, whiteMinimal[1U].r);

    classic.game.inCheckmate = true;
    duck.game.inCheckmate = true;
    const auto classicIntent =
        modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &classic});
    const auto duckIntent =
        modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &duck});
    Check(classicIntent.celebration ==
              modules::led_frame_composer::internal::CelebrationIntent::Checkmate,
          "Classic still renders orthodox checkmate intent");
    Check(duckIntent.celebration ==
              modules::led_frame_composer::internal::CelebrationIntent::None,
          "Duck suppresses orthodox checkmate intent");

    classic.game.inCheckmate = false;
    classic.game.inDraw = true;
    duck.game.inCheckmate = false;
    duck.game.inDraw = true;
    const auto classicDrawIntent =
        modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &classic});
    const auto duckDrawIntent =
        modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &duck});
    Check(classicDrawIntent.celebration ==
              modules::led_frame_composer::internal::CelebrationIntent::Draw,
          "Classic still renders orthodox draw intent");
    Check(duckDrawIntent.celebration ==
              modules::led_frame_composer::internal::CelebrationIntent::None,
          "Duck suppresses orthodox draw intent");

    duck.game.variantOutcome = {
        modules::VariantOutcomeReason::Fowled,
        modules::GameWinner::White};
    const auto duckVictoryIntent =
        modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &duck});
    Check(duckVictoryIntent.celebration ==
              modules::led_frame_composer::internal::CelebrationIntent::VariantVictory,
          "Duck terminal outcome selects the explicit variant victory intent");
}

runtime::InteractionModelSnapshot MakePendingDuckTurnSnapshot(
    const bool duckLifted,
    const uint32_t nowMs) {
    static constexpr uint8_t BLACK_KING = 4U;
    static constexpr uint8_t DUCK = 18U;
    static constexpr uint8_t MOVE_FROM = 52U;
    static constexpr uint8_t MOVE_TO = 36U;
    static constexpr uint8_t WHITE_KING = 60U;

    const uint64_t whiteAfter = (1ULL << WHITE_KING) |
                                (1ULL << MOVE_TO);
    const uint64_t blackAfter = 1ULL << BLACK_KING;
    const uint64_t withDuck = whiteAfter | blackAfter | (1ULL << DUCK);
    runtime::InteractionModelSnapshot snapshot = MakeSnapshot(
        withDuck, duckLifted ? (withDuck & ~(1ULL << DUCK)) : withDuck);
    snapshot.system.nowMs = nowMs;
    snapshot.board.enginePiecesValid = true;
    snapshot.board.enginePieces[BLACK_KING] = 'k';
    snapshot.board.enginePieces[MOVE_FROM] = 'P';
    snapshot.board.enginePieces[WHITE_KING] = 'K';
    // The reducer has projected the post-chess side masks, but the transaction
    // intentionally keeps the engine/FEN on White's pre-move position.
    snapshot.board.moverOccupiedMask = blackAfter;
    snapshot.board.opponentOccupiedMask = whiteAfter;
    snapshot.game.sideToMove = modules::SideToMove::White;
    snapshot.game.variant = modules::GameVariant::Duck;
    snapshot.game.duckSquare = DUCK;
    snapshot.game.duckTurnPhase = modules::DuckTurnPhase::DuckRelocation;

    auto& interaction = snapshot.interaction.taggedState;
    interaction.tag = runtime::ChessInteractionPhase::DuckRelocation;
    interaction.requiredAction = duckLifted
                                     ? runtime::RequiredPhysicalAction::PlacePrimary
                                     : runtime::RequiredPhysicalAction::LiftPrimary;
    interaction.plan.kind = runtime::PhysicalTransitionKind::DuckRelocation;
    interaction.plan.source = runtime::PhysicalTransitionSource::Human;
    interaction.plan.beforeOccupiedMask = withDuck;
    interaction.plan.afterOccupiedMask = withDuck;
    interaction.plan.moveFrom = DUCK;
    interaction.plan.relocationCount = 1U;
    interaction.plan.relocations[0] = {
        DUCK, runtime::CHESS_INTERACTION_INVALID_SQUARE};
    interaction.plan.progress.relocationsLiftedMask = duckLifted ? 1U : 0U;
    return snapshot;
}

void CheckPendingChessPieceRemainsLit(const fxe::SettingsStore& settings) {
    static constexpr uint8_t MOVE_TO = 36U;
    static constexpr uint8_t WHITE_KING = 60U;
    runtime::InteractionModelSnapshot snapshot =
        MakePendingDuckTurnSnapshot(false, 0U);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    Compose(MakeInputs(settings, snapshot, false), ledFrame);

    const modules::Rgb movedPiece =
        LedAt(ledFrame, MOVE_TO, hardware::SquareLedSlot::Piece);
    const modules::Rgb whiteReference =
        LedAt(ledFrame, WHITE_KING, hardware::SquareLedSlot::Piece);
    Check(!RgbEq(movedPiece, modules::RGB_BLACK),
          "staged chess mover stays lit during mandatory duck relocation");
    Check(RgbEq(movedPiece, whiteReference),
          "staged chess mover keeps its player's normal piece color");
}

void CheckDuckRelocationAttention(const fxe::SettingsStore& settings) {
    static constexpr uint8_t DUCK = 18U;
    runtime::InteractionModelSnapshot pulseLow =
        MakePendingDuckTurnSnapshot(false, 0U);
    runtime::InteractionModelSnapshot pulseHigh =
        MakePendingDuckTurnSnapshot(false, 72U);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> lowFrame{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> highFrame{};
    Compose(MakeInputs(settings, pulseLow, false), lowFrame);
    Compose(MakeInputs(settings, pulseHigh, false), highFrame);

    const modules::Rgb lowPiece =
        LedAt(lowFrame, DUCK, hardware::SquareLedSlot::Piece);
    const modules::Rgb highPiece =
        LedAt(highFrame, DUCK, hardware::SquareLedSlot::Piece);
    Check(RgbEnergy(highPiece) > RgbEnergy(lowPiece) + 180U,
          "required duck center LED has a strong yellow pulse");

    runtime::InteractionModelSnapshot heldA =
        MakePendingDuckTurnSnapshot(true, 0U);
    runtime::InteractionModelSnapshot heldB =
        MakePendingDuckTurnSnapshot(true, 600U);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> heldFrameA{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> heldFrameB{};
    Compose(MakeInputs(settings, heldA, false), heldFrameA);
    Compose(MakeInputs(settings, heldB, false), heldFrameB);
    const modules::Rgb heldOriginA =
        LedAt(heldFrameA, DUCK, hardware::SquareLedSlot::Square0);
    const modules::Rgb heldOriginB =
        LedAt(heldFrameB, DUCK, hardware::SquareLedSlot::Square0);
    Check(RgbEq(heldOriginA, modules::RGB_YELLOW),
          "lifted duck leaves a solid yellow origin square");
    Check(RgbEq(heldOriginA, heldOriginB),
          "lifted duck origin does not pulse while choosing a destination");

    runtime::InteractionModelSnapshot hinted = heldA;
    hinted.render.hintDisplayRequested = true;
    hinted.render.hintMoveValid = true;
    hinted.render.hintMoveFromSquare = DUCK;
    hinted.render.hintMoveToSquare = 27U;
    const auto hintedIntent =
        modules::led_frame_composer::internal::BuildRenderIntent(
            {.snapshot = &hinted, .hintMoveValid = true});
    Check(hintedIntent.hintPromptVisible,
          "human Duck relocation can render its separate placement hint");

    static constexpr uint8_t FIXED_DESTINATION = 27U;
    runtime::InteractionModelSnapshot undoPrompt = heldA;
    undoPrompt.interaction.taggedState.plan.moveTo = FIXED_DESTINATION;
    undoPrompt.interaction.taggedState.plan.relocations[0].to =
        FIXED_DESTINATION;
    undoPrompt.interaction.taggedState.plan.source =
        runtime::PhysicalTransitionSource::Human;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> undoFrame{};
    Compose(MakeInputs(settings, undoPrompt, false), undoFrame);
    const modules::Rgb undoTarget = LedAt(
        undoFrame, FIXED_DESTINATION,
        hardware::SquareLedSlot::Square0);
    Check(undoTarget.r > undoTarget.b &&
              undoTarget.g > undoTarget.b,
          "fixed human Duck undo visibly prompts the prior destination");

    runtime::InteractionModelSnapshot cpuPrompt = undoPrompt;
    cpuPrompt.interaction.taggedState.plan.source =
        runtime::PhysicalTransitionSource::Cpu;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> cpuFrame{};
    Compose(MakeInputs(settings, cpuPrompt, false), cpuFrame);
    const modules::Rgb cpuTarget = LedAt(
        cpuFrame, FIXED_DESTINATION,
        hardware::SquareLedSlot::Square0);
    Check(cpuTarget.r > cpuTarget.b &&
              cpuTarget.g > cpuTarget.b,
          "fixed CPU Duck choreography visibly prompts its destination");
}

void CheckCompoundDuckHint(const fxe::SettingsStore& settings) {
    static constexpr uint8_t CHESS_FROM = 52U;
    static constexpr uint8_t CHESS_TO = 36U;
    static constexpr uint8_t DUCK_FROM = 24U;
    static constexpr uint8_t DUCK_TO = 35U;
    const uint64_t occupancy =
        (1ULL << CHESS_FROM) | (1ULL << DUCK_FROM);
    runtime::InteractionModelSnapshot snapshot =
        MakeSnapshot(1ULL << CHESS_FROM, occupancy);
    snapshot.board.enginePiecesValid = true;
    snapshot.board.enginePieces[CHESS_FROM] = 'P';
    snapshot.game.variant = modules::GameVariant::Duck;
    snapshot.game.duckSquare = DUCK_FROM;
    snapshot.game.sideToMove = modules::SideToMove::White;
    snapshot.interaction.taggedState.tag =
        runtime::ChessInteractionPhase::Idle;
    snapshot.render.hintDisplayRequested = true;
    snapshot.render.hintMoveValid = true;
    snapshot.render.hintMoveFromSquare = CHESS_FROM;
    snapshot.render.hintMoveToSquare = CHESS_TO;
    snapshot.render.hintDuckMoveValid = true;
    snapshot.render.hintDuckMoveFromSquare = DUCK_FROM;
    snapshot.render.hintDuckMoveToSquare = DUCK_TO;
    snapshot.system.nowMs = 384U;
    snapshot.interaction.lastInteractionElapsedMs = 384U;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> frame{};
    Compose(MakeInputs(settings, snapshot, false), frame);
    auto chessOnly = snapshot;
    chessOnly.render.hintDuckMoveValid = false;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> chessOnlyFrame{};
    Compose(MakeInputs(settings, chessOnly, false), chessOnlyFrame);
    const modules::Rgb chessSource =
        LedAt(frame, CHESS_FROM, hardware::SquareLedSlot::Square0);
    const modules::Rgb chessDestination =
        LedAt(frame, CHESS_TO, hardware::SquareLedSlot::Square0);
    Check(RgbEnergy(chessSource) != 0U &&
              RgbEnergy(chessDestination) != 0U,
          "cached compound Duck Hint first shows only its chess movement");
    for (const uint8_t square : {DUCK_FROM, DUCK_TO, uint8_t{25U}}) {
        for (const auto slot : {hardware::SquareLedSlot::Square0,
                                hardware::SquareLedSlot::Piece,
                                hardware::SquareLedSlot::Square2}) {
            Check(RgbEq(LedAt(frame, square, slot),
                        LedAt(chessOnlyFrame, square, slot)),
                  "unplayed chess Hint leaves Duck endpoints and route at their ordinary baseline");
        }
    }

    snapshot.board.expectedOccupiedMask =
        (1ULL << CHESS_TO) | (1ULL << DUCK_FROM);
    snapshot.board.physicalOccupiedMask = snapshot.board.expectedOccupiedMask;
    snapshot.board.enginePieces[CHESS_FROM] = '\0';
    snapshot.board.enginePieces[CHESS_TO] = 'P';
    snapshot.game.duckTurnPhase = modules::DuckTurnPhase::DuckRelocation;
    auto& interaction = snapshot.interaction.taggedState;
    interaction.tag = runtime::ChessInteractionPhase::DuckRelocation;
    interaction.requiredAction = runtime::RequiredPhysicalAction::LiftPrimary;
    interaction.selectedSquare = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    interaction.plan.kind = runtime::PhysicalTransitionKind::DuckRelocation;
    interaction.plan.source = runtime::PhysicalTransitionSource::Human;
    interaction.plan.moveFrom = DUCK_FROM;
    interaction.plan.moveTo = runtime::CHESS_INTERACTION_INVALID_SQUARE;
    snapshot.render.hintMoveFromSquare = DUCK_FROM;
    snapshot.render.hintMoveToSquare = DUCK_TO;
    snapshot.render.hintDuckMoveValid = false;
    Compose(MakeInputs(settings, snapshot, false), frame);
    const modules::Rgb duckSource =
        LedAt(frame, DUCK_FROM, hardware::SquareLedSlot::Square0);
    const modules::Rgb duckDestination =
        LedAt(frame, DUCK_TO, hardware::SquareLedSlot::Square0);
    const modules::Rgb duckRoute =
        LedAt(frame, 25U, hardware::SquareLedSlot::Piece);
    Check(duckSource.r > duckSource.b && duckSource.g > duckSource.b &&
              duckDestination.r > duckDestination.b &&
              duckDestination.g > duckDestination.b &&
              duckRoute.r > 0U && duckRoute.r == duckRoute.g && duckRoute.b == 0U,
          "after chess acknowledgement the Duck-only Hint traces a5-to-d4 through yellow b5");
    auto ordinary = snapshot;
    ordinary.render.hintDisplayRequested = false;
    ordinary.render.hintMoveValid = false;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ordinaryFrame{};
    Compose(MakeInputs(settings, ordinary, false), ordinaryFrame);
    for (const uint8_t square : {CHESS_FROM, CHESS_TO}) {
        for (const auto slot : {hardware::SquareLedSlot::Square0,
                                hardware::SquareLedSlot::Piece,
                                hardware::SquareLedSlot::Square2}) {
            Check(RgbEq(LedAt(frame, square, slot),
                        LedAt(ordinaryFrame, square, slot)),
                  "Duck relocation does not replay the completed chess Hint endpoints");
        }
    }
}

void CheckHumanAndCpuSettlePresentationMatches(
    const fxe::SettingsStore& settings) {
    static constexpr uint8_t FROM = 52U;
    static constexpr uint8_t TO = 36U;
    runtime::InteractionModelSnapshot human =
        MakeSnapshot(1ULL << TO, 1ULL << TO);
    human.system.nowMs = 1000U;
    human.board.enginePiecesValid = true;
    human.board.enginePieces[FROM] = 'P';
    human.game.sideToMove = modules::SideToMove::White;
    human.interaction.taggedState.tag =
        runtime::ChessInteractionPhase::Settling;
    human.interaction.taggedState.plan.kind =
        runtime::PhysicalTransitionKind::QuietMove;
    human.interaction.taggedState.plan.source =
        runtime::PhysicalTransitionSource::Human;
    human.interaction.taggedState.plan.moveFrom = FROM;
    human.interaction.taggedState.plan.moveTo = TO;
    human.interaction.taggedState.plan.relocationCount = 1U;
    human.interaction.taggedState.plan.relocations[0] = {FROM, TO};
    human.render.settleSquare = TO;
    human.render.settleOn = false;
    human.render.settleTimingValid = false;
    human.render.settleVisualElapsedMs = 1000U;
    human.render.settlePresentationDurationMs = 2000U;

    runtime::InteractionModelSnapshot cpu = human;
    cpu.board.enginePieces.fill('\0');
    cpu.board.enginePieces[TO] = 'P';
    cpu.game.sideToMove = modules::SideToMove::Black;
    cpu.interaction.taggedState = runtime::TaggedInteractionState{};
    cpu.render.cpuMoveSettlingActive = true;
    cpu.render.settleTimingValid = true;
    cpu.render.settleElapsedMs = 1000U;
    cpu.render.settleDurationMs = 2000U;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> humanFrame{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> cpuFrame{};
    Compose(MakeInputs(settings, human, false), humanFrame);
    Compose(MakeInputs(settings, cpu, false), cpuFrame);
    Check(RgbEq(LedAt(humanFrame, TO, hardware::SquareLedSlot::Piece),
                LedAt(cpuFrame, TO, hardware::SquareLedSlot::Piece)),
          "O-confirm human settle uses the CPU destination settle presentation");
    Check(RgbEq(LedAt(humanFrame, TO, hardware::SquareLedSlot::Square0),
                LedAt(cpuFrame, TO, hardware::SquareLedSlot::Square0)),
          "human and CPU destination settle square animations match");

    runtime::InteractionModelSnapshot timedHuman = human;
    timedHuman.render.settleTimingValid = true;
    timedHuman.render.settleElapsedMs = 1000U;
    timedHuman.render.settleDurationMs = 5000U;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> timedHumanFrame{};
    Compose(MakeInputs(settings, timedHuman, false), timedHumanFrame);
    Check(RgbEq(LedAt(timedHumanFrame, TO, hardware::SquareLedSlot::Piece),
                LedAt(cpuFrame, TO, hardware::SquareLedSlot::Piece)),
          "5-second human settle preserves the CPU destination presentation");
    Check(RgbEq(LedAt(timedHumanFrame, TO, hardware::SquareLedSlot::Square0),
                LedAt(cpuFrame, TO, hardware::SquareLedSlot::Square0)),
          "5-second policy does not stretch the settle animation cadence");
}

} // namespace

int main() {
    std::puts("[DUCK_LED_RENDER] start");

    modules::SettingsStoreConfig config;
    config.path = "build/test_duck_led_render.json";
    modules::SettingsStore settings(config);
    (void)settings.Load();
    settings.Doc()["WHITE_SQUARES_ON"] = false;
    settings.Doc()["P1_CHECK_CUE"] = true;
    settings.Doc()["P2_CHECK_CUE"] = true;

    CheckDuckYellowAtBothOrientations(settings);
    CheckClassicAndUnclassifiedExtrasRemainFaults(settings);
    CheckMissingExpectedDuckRemainsACorrection(settings);
    CheckOrthodoxThreatIntentSuppressedForDuck(settings);
    CheckPendingChessPieceRemainsLit(settings);
    CheckDuckRelocationAttention(settings);
    CheckCompoundDuckHint(settings);
    CheckHumanAndCpuSettlePresentationMatches(settings);

    if (g_failures == 0) {
        std::puts("[DUCK_LED_RENDER] ok");
        return 0;
    }

    std::printf("[DUCK_LED_RENDER] failures=%d\n", g_failures);
    return 1;
}
