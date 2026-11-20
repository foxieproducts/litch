#include "bounded_hint_engine.hpp"
#include <array>
#include <cstdint>
#include <cstdio>

#include "../support/led_frame_composer_snapshot_adapter.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "modules/settle_animation_setting.hpp"
#include "modules/ui_color_wheel.hpp"
#include "runtime/directed_physical_prompt.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/square_mapping.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

std::array<bool, 64> Occ64ToOccupiedPhysical(const uint64_t occ64) {
    std::array<bool, 64> out{};
    for (uint8_t sq = 0; sq < 64; ++sq) {
        out[sq] = ((occ64 >> sq) & 1ULL) != 0ULL;
    }
    return out;
}

modules::Rgb PieceLedAtSquare(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
                              const uint8_t sq) {
    return ledFrame[hardware::LedIndexForSquare(sq, hardware::SquareLedSlot::Piece)];
}

modules::Rgb SquareLedAtSquare(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
                               const uint8_t sq) {
    return ledFrame[hardware::LedIndexForSquare(sq, hardware::SquareLedSlot::Square0)];
}

bool RgbEq(const modules::Rgb a, const modules::Rgb b) {
    return (a.r == b.r) && (a.g == b.g) && (a.b == b.b);
}

bool HasBrightNeutralPixel(const std::array<modules::Rgb, 64>& grid) {
    for (const modules::Rgb pixel : grid) {
        if (pixel.r >= 200U && pixel.g >= 200U && pixel.b >= 200U) {
            return true;
        }
    }
    return false;
}

void PrintRgb(const char* label, const modules::Rgb c) {
    std::printf("[DBG] %s = (%u,%u,%u)\n", label, static_cast<unsigned>(c.r), static_cast<unsigned>(c.g),
                static_cast<unsigned>(c.b));
}

} // namespace

int main() {
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);

    // Keep this test focused: it validates that piece colors and prompts remain
    // readable when engine and physical board diverge during castling.

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_castling_led_ux.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    // Use fixed wheels so prompt colors are deterministic.
    settings.Doc()["P1_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_PIECE_WHEEL"] = 128;

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    test_support::BoundedHintEngine engine;
    engine.Reset();

    modules::EngineDerivedState derived;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};

    // Regression: drive the real king-first sequence through the coordinator.
    // Once the king reaches g1, the still-pending rook relocation is legal
    // castling progress, not a generic expected-vs-physical board mismatch.
    {
        const char* preCastleFen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";
        const uint64_t preCastleOcc64 =
            (1ULL << 0) | (1ULL << 4) | (1ULL << 7) |
            (1ULL << 56) | (1ULL << 60) | (1ULL << 63);

        modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
        Check(coordinator.LoadOrInitialize(), "castling feedback persistence init");
        sm.DebugSetPhysicalOccupancyMask(preCastleOcc64);
        Check(coordinator.LoadExternalFen(preCastleFen),
              "castling feedback seed FEN");

        const uint64_t kingLiftedOcc = preCastleOcc64 & ~(1ULL << 60U);
        const uint64_t kingPlacedOcc = kingLiftedOcc | (1ULL << 62U);
        sm.HandleBoardObservation(1ULL << 60U, kingLiftedOcc, 1000U, 1U);
        coordinator.Update();

        // A lifted king shows castling as a coordinated king+rook intention,
        // not as an ordinary legal square that happens to be two files away.
        derived.RefreshFromEngine(engine);
        runtime::InteractionModelSnapshot snapshot =
            test_support::BuildInteractionSnapshot(sm, derived, &engine);
        Check((snapshot.interaction.castlingKingDestinationMask &
               (1ULL << 62U)) != 0ULL,
              "king pickup identifies the kingside castle destination");
        Check((snapshot.interaction.castlingKingDestinationMask &
               (1ULL << 58U)) != 0ULL,
              "king pickup identifies the queenside castle destination");
        Check((snapshot.interaction.castlingRookSourceMask &
               ((1ULL << 56U) | (1ULL << 63U))) ==
                  ((1ULL << 56U) | (1ULL << 63U)),
              "castle intent pairs each destination with its rook");

        modules::led_frame_composer::ComposeInputs in;
        in.settings = &settings;
        in.interactionSnapshot = &snapshot;
        modules::led_frame_composer::Compose(
            in, ledFrame, edgeFrame, gridFrame);
        const modules::Rgb castleDestinationEarly = gridFrame[62U];
        const modules::Rgb ordinaryKingDestination = gridFrame[61U];
        const modules::Rgb rookSourceEarly = gridFrame[63U];
        Check(!RgbEq(castleDestinationEarly, ordinaryKingDestination),
              "castle destination is visually distinct from an ordinary king move");

        fxe::ManualMillis::SetNowMs(1300U);
        snapshot = test_support::BuildInteractionSnapshot(sm, derived, &engine);
        in.interactionSnapshot = &snapshot;
        modules::led_frame_composer::Compose(
            in, ledFrame, edgeFrame, gridFrame);
        Check(!RgbEq(castleDestinationEarly, gridFrame[62U]),
              "castle destination carries a restrained rhythmic cue");
        Check(!RgbEq(rookSourceEarly, gridFrame[63U]),
              "castle rook answers the king destination in the paired cue");

        fxe::ManualMillis::SetNowMs(1001U);
        sm.HandleBoardObservation(1ULL << 62U, kingPlacedOcc, 1001U, 2U);
        derived.RefreshFromEngine(engine);

        snapshot = test_support::BuildInteractionSnapshot(sm, derived, &engine);
        in.interactionSnapshot = &snapshot;
        const uint32_t rookActionToken = snapshot.interaction.taggedState.pendingActionToken;
        modules::led_frame_composer::internal::ComposeContext ctx;
        Check(modules::led_frame_composer::internal::BuildComposeContext(in, ctx),
              "castling feedback compose context");
        const modules::led_frame_composer::internal::InvalidBoardState invalidBoard =
            modules::led_frame_composer::internal::ComputeInvalidBoardState(ctx);
        Check(snapshot.interaction.taggedState.requiredAction ==
                  runtime::RequiredPhysicalAction::LiftSecondary,
              "king placement enters the legal rook follow-up prompt");
        Check(!invalidBoard.IsActive(),
              "legal king-first castling progress has no invalid-board feedback");
        Check(!sm.IsMoveSettlingActive(),
              "partial castling feedback does not own the move settle timer");
        Check(snapshot.render.settleAfterglowActive &&
                  snapshot.render.settleAfterglowSquare == 62U &&
                  !snapshot.render.settleAfterglowAbort,
              "king relocation starts positive render-only settle feedback");
        Check(ctx.partialCastlingTargetOccValid &&
                  (ctx.partialCastlingTargetOcc64 & (1ULL << 61U)) != 0ULL,
              "legal king-first progress exposes positive rook-destination guidance");

        // The king has completed its relocation. Only the rook should remain
        // animated; the king must not be reclassified as a piece that still
        // needs to leave its square.
        modules::led_frame_composer::Compose(in, ledFrame, edgeFrame, gridFrame);
        const modules::Rgb kingPromptEarly = PieceLedAtSquare(ledFrame, 62U);
        const modules::Rgb kingSettleEarly = SquareLedAtSquare(ledFrame, 62U);
        fxe::ManualMillis::SetNowMs(1101U);
        snapshot = test_support::BuildInteractionSnapshot(sm, derived, &engine);
        in.interactionSnapshot = &snapshot;
        Check(modules::led_frame_composer::internal::BuildComposeContext(in, ctx),
              "king-feedback castling compose context");
        modules::led_frame_composer::Compose(in, ledFrame, edgeFrame, gridFrame);
        Check(!RgbEq(kingSettleEarly, SquareLedAtSquare(ledFrame, 62U)),
              "king settle feedback animates while the rook prompt is active");
        Check(snapshot.interaction.taggedState.requiredAction ==
                  runtime::RequiredPhysicalAction::LiftSecondary &&
                  ctx.partialCastlingTargetOccValid &&
                  (ctx.partialCastlingTargetOcc64 & (1ULL << 61U)) != 0ULL,
              "king feedback never serializes or delays the rook handoff");
        const uint32_t styleMs = modules::settle_animation_setting::DurationMs(
            modules::settle_animation_setting::Read(settings),
            settings.ReadValue("MINIMAL_ANIMATIONS", false));
        fxe::ManualMillis::SetNowMs(1001U + styleMs);
        snapshot = test_support::BuildInteractionSnapshot(sm, derived, &engine);
        in.interactionSnapshot = &snapshot;
        modules::led_frame_composer::Compose(in, ledFrame, edgeFrame, gridFrame);
        Check(snapshot.interaction.taggedState.requiredAction ==
                  runtime::RequiredPhysicalAction::LiftSecondary,
              "selected placement style expiry preserves the required rook next action");
        // The raw render envelope accommodates the longest supported style.
        // Its later expiry cannot revive feedback or change the rook relocation.
        const uint32_t afterglowMs = modules::settle_animation_setting::DurationMs(
            modules::SettleAnimationStyle::RainbowOrbit, false);
        fxe::ManualMillis::SetNowMs(1001U + afterglowMs);
        snapshot = test_support::BuildInteractionSnapshot(sm, derived, &engine);
        in.interactionSnapshot = &snapshot;
        modules::led_frame_composer::Compose(in, ledFrame, edgeFrame, gridFrame);
        const modules::Rgb kingPromptLater = PieceLedAtSquare(ledFrame, 62U);
        Check(RgbEq(kingPromptEarly, kingPromptLater),
              "completed castling king stays steady during the rook follow-up prompt");
        Check(!RgbEq(kingPromptLater, modules::RGB_BLACK),
              "completed castling king stays lit during the rook follow-up prompt");
        runtime::DirectedPhysicalPrompt rookPrompt;
        Check(!snapshot.render.settleAfterglowActive &&
                  runtime::TryGetDirectedPhysicalPrompt(snapshot.interaction.taggedState, rookPrompt) &&
                  rookPrompt.fromSquare == 63U && rookPrompt.toSquare == 61U &&
                  rookPrompt.action == runtime::RequiredPhysicalAction::LiftSecondary &&
                  snapshot.interaction.taggedState.pendingActionToken == rookActionToken &&
                  snapshot.interaction.humanIllegalMask == 0ULL &&
                  snapshot.interaction.cpuIllegalMask == 0ULL &&
                  modules::led_frame_composer::internal::BuildComposeContext(in, ctx) &&
                  !modules::led_frame_composer::internal::ComputeInvalidBoardState(ctx).IsActive(),
              "king feedback expiry leaves the same rook prompt authoritative");
        fxe::ManualMillis::SetNowMs(1001U);

        const uint64_t rookLiftedOcc = kingPlacedOcc & ~(1ULL << 63U);
        sm.HandleBoardObservation(1ULL << 63U, rookLiftedOcc, 1002U, 3U);
        snapshot = test_support::BuildInteractionSnapshot(sm, derived, &engine);
        in.interactionSnapshot = &snapshot;
        Check(modules::led_frame_composer::internal::BuildComposeContext(in, ctx),
              "rook-lift castling feedback compose context");
        const auto rookLiftInvalidBoard =
            modules::led_frame_composer::internal::ComputeInvalidBoardState(ctx);
        Check(snapshot.interaction.humanIllegalMask == 0ULL &&
                  snapshot.interaction.cpuIllegalMask == 0ULL,
              "lifting the prompted castling rook does not latch illegal feedback");
        Check(!rookLiftInvalidBoard.IsActive(),
              "lifting the prompted castling rook has no mismatch-rainbow feedback");

        const uint64_t castleCompleteOcc = rookLiftedOcc | (1ULL << 61U);
        sm.HandleBoardObservation(1ULL << 61U, castleCompleteOcc, 1003U, 4U);
        fxe::ManualMillis::SetNowMs(1020U);
        sm.Tick();
        coordinator.Update();

        fxe::ManualMillis::SetNowMs(1040U);
        sm.Tick();

        Check(coordinator.RequestUndo(1U), "completed castle can enter undo");
        coordinator.Update();
        derived.RefreshFromEngine(engine);
        snapshot = test_support::BuildInteractionSnapshot(sm, derived, &engine);
        in.interactionSnapshot = &snapshot;
        Check(modules::led_frame_composer::internal::BuildComposeContext(in, ctx),
              "castling undo entry compose context");
        Check(snapshot.interaction.undoPromptActive,
              "completed castle undo exposes a physical undo prompt");
        Check(!modules::led_frame_composer::internal::ComputeInvalidBoardState(ctx).IsActive(),
              "castling undo entry is guidance rather than an invalid-board state");

        const uint64_t undoKingLiftedOcc = castleCompleteOcc & ~(1ULL << 62U);
        sm.HandleBoardObservation(1ULL << 62U, undoKingLiftedOcc, 1041U, 5U);
        snapshot = test_support::BuildInteractionSnapshot(sm, derived, &engine);
        in.interactionSnapshot = &snapshot;
        Check(modules::led_frame_composer::internal::BuildComposeContext(in, ctx),
              "castling undo king-lift compose context");
        Check(snapshot.interaction.undoPromptActive &&
                  !modules::led_frame_composer::internal::ComputeInvalidBoardState(ctx).IsActive(),
              "legal castling undo king lift remains positive guidance");
        runtime::DirectedPhysicalPrompt undoPrompt;
        Check(runtime::TryGetDirectedPhysicalPrompt(
                  snapshot.interaction.taggedState, undoPrompt) &&
                  undoPrompt.relocationIndex == 0U &&
                  undoPrompt.fromSquare == 62U &&
                  undoPrompt.toSquare == 60U,
              "castling undo king pickup keeps the king-return target active");

        const uint64_t undoKingReturnedOcc = undoKingLiftedOcc | (1ULL << 60U);
        sm.HandleBoardObservation(1ULL << 60U, undoKingReturnedOcc, 1042U, 6U);
        snapshot = test_support::BuildInteractionSnapshot(sm, derived, &engine);
        in.interactionSnapshot = &snapshot;
        Check(modules::led_frame_composer::internal::BuildComposeContext(in, ctx),
              "castling undo king-return compose context");
        Check(snapshot.interaction.undoPromptActive &&
                  !modules::led_frame_composer::internal::ComputeInvalidBoardState(ctx).IsActive(),
              "legal castling undo king return remains positive guidance");
        Check(runtime::TryGetDirectedPhysicalPrompt(
                  snapshot.interaction.taggedState, undoPrompt) &&
                  undoPrompt.relocationIndex == 1U &&
                  undoPrompt.fromSquare == 61U &&
                  undoPrompt.toSquare == 63U,
              "castling undo advances from the returned king to the rook");
        modules::BoardLedFrame overlayOnlyLeds{};
        std::array<modules::Rgb, 64> overlayOnlyGrid{};
        modules::led_frame_composer::internal::ApplyCenterTextOverlay(
            ctx, overlayOnlyLeds, overlayOnlyGrid);
        Check(!HasBrightNeutralPixel(overlayOnlyGrid),
              "castling undo rook relocation uses spatial guidance without a board-letter glyph");
        modules::led_frame_composer::Compose(
            in, ledFrame, edgeFrame, gridFrame);
        const modules::Rgb rookReturnCue = gridFrame[63U];
        Check(!RgbEq(rookReturnCue, gridFrame[60U]),
              "castling undo rook step does not retain the king-return cue");
        modules::led_frame_composer::Compose(
            in, ledFrame, edgeFrame, gridFrame);
        const auto rotatedUndoPhysical = hardware::MapSemanticLedFrame(
            {hardware::BoardRevision::V2S3, true, true}, ledFrame, edgeFrame);
        Check(RgbEq(rotatedUndoPhysical[hardware::LedIndexForSquare(
                        runtime::MapSquareIndex(63U, true),
                        hardware::SquareLedSlot::Square0)], rookReturnCue),
              "rotated castling undo maps the rook-return cue physically");
        Check(!RgbEq(rotatedUndoPhysical[hardware::LedIndexForSquare(
                         runtime::MapSquareIndex(60U, true),
                         hardware::SquareLedSlot::Square0)], rookReturnCue),
              "rotated castling undo leaves the completed king return inactive");

        const uint64_t undoRookLiftedOcc = undoKingReturnedOcc & ~(1ULL << 61U);
        sm.HandleBoardObservation(1ULL << 61U, undoRookLiftedOcc, 1043U, 7U);
        snapshot = test_support::BuildInteractionSnapshot(sm, derived, &engine);
        in.interactionSnapshot = &snapshot;
        Check(modules::led_frame_composer::internal::BuildComposeContext(in, ctx),
              "castling undo rook-lift compose context");
        Check(snapshot.interaction.undoPromptActive &&
                  !modules::led_frame_composer::internal::ComputeInvalidBoardState(ctx).IsActive(),
              "legal castling undo rook lift remains positive guidance");
        Check(runtime::TryGetDirectedPhysicalPrompt(
                  snapshot.interaction.taggedState, undoPrompt) &&
                  undoPrompt.relocationIndex == 1U &&
                  undoPrompt.toSquare == 63U,
              "castling undo rook pickup keeps the rook-return target active");

        const uint64_t undoCompleteOcc = undoRookLiftedOcc | (1ULL << 63U);
        sm.HandleBoardObservation(1ULL << 63U, undoCompleteOcc, 1044U, 8U);
        snapshot = test_support::BuildInteractionSnapshot(sm, derived, &engine);
        in.interactionSnapshot = &snapshot;
        Check(modules::led_frame_composer::internal::BuildComposeContext(in, ctx),
              "castling undo completion compose context");
        Check(!snapshot.interaction.undoPromptActive,
              "castling undo prompt completes after both pieces return");
        Check(!modules::led_frame_composer::internal::ComputeInvalidBoardState(ctx).IsActive(),
              "completed castling undo does not emit a trailing invalid-board cue");
    }

    // Case 1: King-first castling partial state.
    // Engine is still pre-castle (king at e1, rook at h1), but physically the
    // king is already at g1 and rook has not moved yet.
    {
        const char* preCastleFen = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";
        Check(engine.SetFen(preCastleFen), "SetFen pre-castle");
        derived.RefreshFromEngine(engine);

        // Physical occupancy: black a8/e8/h8, white a1/h1, and king already on g1.
        const uint64_t physOcc64 = (1ULL << 0) | (1ULL << 4) | (1ULL << 7) | (1ULL << 56) | (1ULL << 62) | (1ULL << 63);
        const uint64_t preCastleOcc64 =
            (1ULL << 0) | (1ULL << 4) | (1ULL << 7) |
            (1ULL << 56) | (1ULL << 60) | (1ULL << 63);
        // Start the fixture's clock before producing its immutable plan. The
        // old backwards reset after the events underflowed its owner elapsed.
        fxe::ManualMillis::SetNowMs(0U);
        sm.Reset();
        runtime::ChessPositionState expected;
        expected.occupiedMask = preCastleOcc64;
        expected.moverOccupiedMask =
            (1ULL << 56) | (1ULL << 60) | (1ULL << 63);
        expected.opponentOccupiedMask =
            (1ULL << 0) | (1ULL << 4) | (1ULL << 7);
        expected.kingMask = (1ULL << 4) | (1ULL << 60);
        expected.rookMask =
            (1ULL << 0) | (1ULL << 7) | (1ULL << 56) | (1ULL << 63);
        sm.DebugSetExpectedPositionModel(expected);
        sm.DebugSetPhysicalOccupancyMask(preCastleOcc64);
        const uint64_t liftedOcc64 = preCastleOcc64 & ~(1ULL << 60);
        sm.HandleBoardObservation(1ULL << 60, liftedOcc64, 0U, 1U);
        sm.HandleBoardObservation(1ULL << 62, physOcc64, 1U, 2U);
        Check(sm.GetPhysicalOccupancyMask() == physOcc64,
              "king-first physical occupancy captured");

        const std::array<bool, 64> occupiedPhysical = Occ64ToOccupiedPhysical(physOcc64);

        modules::led_frame_composer::ComposeInputs in;
        in.settings = &settings;
        in.firstPlayerIsP2 = false;

        runtime::InteractionModelSnapshot snapshot =
            test_support::BuildInteractionSnapshot(sm, derived, &engine);
        snapshot.board.expectedOccupiedMask = physOcc64;
        in.interactionSnapshot = &snapshot;
        modules::led_frame_composer::Compose(in, ledFrame, edgeFrame, gridFrame);

        // The king on g1 should render in the same (white) piece color as the rook
        // still on h1, even though the engine reports PieceSide::None on g1.
        {
            const modules::Rgb kingG1 = PieceLedAtSquare(ledFrame, 62);
            const modules::Rgb rookH1 = PieceLedAtSquare(ledFrame, 63);
            if (!RgbEq(kingG1, rookH1)) {
                PrintRgb("king g1 piece", kingG1);
                PrintRgb("rook h1 piece", rookH1);
            }
            Check(RgbEq(kingG1, rookH1), "king g1 piece color matches rook h1 during partial castling");
        }

        // Prompt should clearly indicate the missing rook placement square (f1=61).
        const modules::UiOwner activeOwner =
            modules::led_frame_composer::OwnerForSideToMove(derived.sideToMove, /*firstPlayerIsP2=*/false);
        const uint8_t activeWheel = (activeOwner == modules::UiOwner::P2) ? 128 : 0;
        const uint8_t availablePromptWheel = static_cast<uint8_t>(activeWheel + 16U);
        modules::Rgb expectedPrompt = modules::ScaleRgbSaturatingBy256(
            modules::ui_color_wheel::WheelToRgb(availablePromptWheel), 96U);
        // led_frame_composer applies a global +25% boost across the entire grid.
        static constexpr uint16_t GLOBAL_BRIGHTNESS_BOOST_SCALE256 = 320; // 1.25 in Q8.8
        const auto scale_channel_saturating = [](const uint8_t v, const uint16_t scale256) -> uint8_t {
            const uint32_t scaled = static_cast<uint32_t>(v) *
                                    static_cast<uint32_t>(scale256);
            const uint32_t rounded = (scaled + 128U) >> 8U;
            return (rounded > 255U) ? 255U : static_cast<uint8_t>(rounded);
        };
        expectedPrompt = modules::Rgb{scale_channel_saturating(expectedPrompt.r, GLOBAL_BRIGHTNESS_BOOST_SCALE256),
                                      scale_channel_saturating(expectedPrompt.g, GLOBAL_BRIGHTNESS_BOOST_SCALE256),
                                      scale_channel_saturating(expectedPrompt.b, GLOBAL_BRIGHTNESS_BOOST_SCALE256)};

        if (!RgbEq(gridFrame[61], expectedPrompt)) {
            PrintRgb("grid f1", gridFrame[61]);
            PrintRgb("expected f1", expectedPrompt);
        }
        Check(RgbEq(gridFrame[61], expectedPrompt), "rookTo f1 retains the exact available floor during partial castling");
        Check(RgbEq(SquareLedAtSquare(ledFrame, 61U), expectedPrompt),
              "physical f1 LEDs expose the precise waiting rook destination floor");
        if (!RgbEq(gridFrame[60U], modules::RGB_BLACK)) {
            PrintRgb("grid e1 after king placement", gridFrame[60U]);
        }
        Check(!RgbEq(gridFrame[60U], expectedPrompt) &&
                  !RgbEq(SquareLedAtSquare(ledFrame, 60U), expectedPrompt),
              "king origin e1 has no castling prompt after king placement");

        // Ensure we are not spuriously highlighting the kingTo square, which is already occupied.
        Check(!RgbEq(gridFrame[62], expectedPrompt), "kingTo g1 not highlighted when occupied");

        // Opposite player-end placement preserves logical-to-physical
        // targeting while the visible color stays on its physical end.
        in.firstPlayerIsP2 = true;
        modules::led_frame_composer::Compose(in, ledFrame, edgeFrame, gridFrame);
        const auto oppositeEndPhysical = hardware::MapSemanticLedFrame(
            {hardware::BoardRevision::V2S3, true, true}, ledFrame, edgeFrame);
        const uint8_t physicalF1 = runtime::MapSquareIndex(61U, true);
        const uint8_t physicalE1 = runtime::MapSquareIndex(60U, true);
        const uint8_t physicalG1 = runtime::MapSquareIndex(62U, true);
        const uint8_t physicalH1 = runtime::MapSquareIndex(63U, true);
        const modules::Rgb selectedEndPrompt = modules::ScaleRgbSaturatingBy256(
            modules::ui_color_wheel::WheelToRgb(144U), 96U);
        const modules::Rgb expectedSelectedEndPrompt = modules::Rgb{
            scale_channel_saturating(selectedEndPrompt.r,
                                     GLOBAL_BRIGHTNESS_BOOST_SCALE256),
            scale_channel_saturating(selectedEndPrompt.g,
                                     GLOBAL_BRIGHTNESS_BOOST_SCALE256),
            scale_channel_saturating(selectedEndPrompt.b,
                                     GLOBAL_BRIGHTNESS_BOOST_SCALE256)};
        const auto physicalPiece = [&](const uint8_t square) {
            return oppositeEndPhysical[hardware::LedIndexForSquare(
                square, hardware::SquareLedSlot::Piece)];
        };
        const auto physicalSquare = [&](const uint8_t square) {
            return oppositeEndPhysical[hardware::LedIndexForSquare(
                square, hardware::SquareLedSlot::Square0)];
        };
        // Both horizontal target rails wait on the exact available floor.
        // Rotation puts the source's incoming Right head on physical Left.
        const auto steadyTarget = oppositeEndPhysical[hardware::LedIndexForSquare(
            physicalF1, hardware::SquareLedSlot::Square2)];
        const auto travelingTarget = physicalSquare(physicalF1);
        Check(RgbEq(steadyTarget, expectedSelectedEndPrompt),
              "opposite-end P1/White maps the waiting rook target rail with the exact physical-end available floor");
        Check(RgbEq(travelingTarget, expectedSelectedEndPrompt),
              "both opposite-end rook target rails keep their exact waiting floor");
        const auto selectedEndPiece = modules::ScaleRgbSaturatingBy256(
            modules::ui_color_wheel::WheelToRgb(128U),320U);
        Check(RgbEq(physicalSquare(physicalH1),selectedEndPiece),
              "opposite-end mapped incoming source head uses the mover's exact physical-end piece color");
        Check(!RgbEq(physicalSquare(physicalE1), expectedSelectedEndPrompt),
              "opposite-end P1/White board does not retain a king-origin prompt");
        Check(RgbEq(physicalPiece(physicalG1), physicalPiece(physicalH1)),
              "opposite-end P1/White king and rook use the same owner color");
        in.firstPlayerIsP2 = false;

        // Search activity cannot suppress a valid plan rendered from its
        // immutable published snapshot. Even a different live engine position
        // cannot replace that snapshot's king ownership or rook prompts.
        modules::led_frame_composer::Compose(in, ledFrame, edgeFrame, gridFrame);
        const modules::Rgb publishedKing = PieceLedAtSquare(ledFrame, 62U);
        const modules::Rgb publishedRookSource = SquareLedAtSquare(ledFrame, 63U);
        Check(!RgbEq(publishedKing, modules::RGB_BLACK),
              "published partial-castle king is lit before search");
        sm.SetCpuThinkingActive(true);
        snapshot.game.cpuThinking = true;
        Check(engine.SetFen("4k3/8/8/8/8/8/8/4K3 b - - 0 1"),
              "live engine differs from the immutable castling snapshot");
        modules::led_frame_composer::Compose(in, ledFrame, edgeFrame, gridFrame);
        Check(RgbEq(gridFrame[61], expectedPrompt) &&
                  RgbEq(SquareLedAtSquare(ledFrame, 61U), expectedPrompt) &&
                  RgbEq(SquareLedAtSquare(ledFrame, 63U), publishedRookSource),
              "immutable snapshot keeps both rook prompts during CPU search");
        Check(RgbEq(PieceLedAtSquare(ledFrame, 62U), publishedKing),
              "immutable snapshot keeps the placed king's color during CPU search");
        Check(engine.SetFen(preCastleFen), "restore isolated pre-castle engine fixture");
        sm.SetCpuThinkingActive(false);
    }

    // Case 2: Engine already post-castle, but physically rook is still on its origin.
    // This covers the "wrong rook color until moved" class of issues.
    {
        const char* postCastleFen = "r3k2r/8/8/8/8/8/8/R4RK1 b kq - 1 1";
        Check(engine.SetFen(postCastleFen), "SetFen post-castle");
        derived.RefreshFromEngine(engine);

        // Physical occupancy: king at g1, rook still at h1 (not yet at f1).
        const uint64_t physOcc64 = (1ULL << 0) | (1ULL << 4) | (1ULL << 7) | (1ULL << 56) | (1ULL << 62) | (1ULL << 63);
        sm.DebugSetPhysicalOccupancyMask(physOcc64);

        const std::array<bool, 64> occupiedPhysical = Occ64ToOccupiedPhysical(physOcc64);

        modules::led_frame_composer::ComposeInputs in;
        in.settings = &settings;

        fxe::ManualMillis::SetNowMs(0);

        runtime::InteractionModelSnapshot snapshot =
            test_support::BuildInteractionSnapshot(sm, derived, &engine);
        snapshot.board.expectedOccupiedMask = physOcc64;
        snapshot.interaction.taggedState.tag =
            runtime::ChessInteractionPhase::ExecutingPlan;
        runtime::PhysicalTransitionPlan& plan =
            snapshot.interaction.taggedState.plan;
        plan.kind = runtime::PhysicalTransitionKind::Castling;
        plan.source = runtime::PhysicalTransitionSource::Human;
        plan.beforeOccupiedMask =
            (physOcc64 & ~((1ULL << 62) | (1ULL << 63))) |
            (1ULL << 60) | (1ULL << 63);
        plan.afterOccupiedMask =
            (plan.beforeOccupiedMask & ~((1ULL << 60) | (1ULL << 63))) |
            (1ULL << 62) | (1ULL << 61);
        plan.relocations[0] = runtime::PieceRelocation{60U, 62U};
        plan.relocations[1] = runtime::PieceRelocation{63U, 61U};
        plan.relocationCount = 2U;
        Check(snapshot.board.enginePiecesValid,
              "post-castle snapshot has engine piece placement");
        Check(snapshot.board.enginePieces[62U] == 'K',
              "post-castle snapshot has white king on g1");
        Check(snapshot.board.enginePieces[61U] == 'R',
              "post-castle snapshot has white rook on f1");
        modules::LitchMaxEngine::PieceSide inferredRookSide =
            modules::LitchMaxEngine::PieceSide::None;
        Check(modules::led_frame_composer::TryInferTransitionPieceSide(
                  snapshot, physOcc64, 63U, inferredRookSide),
              "post-castle snapshot infers rook side from relocation plan");
        Check(inferredRookSide == modules::LitchMaxEngine::PieceSide::White,
              "post-castle relocation inference identifies a white rook");
        in.interactionSnapshot = &snapshot;
        modules::led_frame_composer::Compose(in, ledFrame, edgeFrame, gridFrame);

        // The rook physically on h1 should still be rendered as white (same as king on g1)
        // even though the engine thinks h1 is empty.
        {
            const modules::Rgb kingG1 = PieceLedAtSquare(ledFrame, 62);
            const modules::Rgb rookH1 = PieceLedAtSquare(ledFrame, 63);
            if (!RgbEq(rookH1, kingG1)) {
                PrintRgb("king g1 piece (post)", kingG1);
                PrintRgb("rook h1 piece (post)", rookH1);
            }
            Check(RgbEq(rookH1, kingG1), "rook h1 piece color matches king g1 when engine is post-castle");
        }

        // Ensure a real piece LED is lit for the rook square.
        Check(!RgbEq(PieceLedAtSquare(ledFrame, 63), SquareLedAtSquare(ledFrame, 63)),
              "rook h1 piece LED differs from square LED");

        // Deletion-test characterization: a stale plan payload under Idle is
        // inert. The tagged phase, not plan-shaped leftovers, owns rendering.
        runtime::InteractionModelSnapshot idleSnapshot = snapshot;
        idleSnapshot.interaction.taggedState.tag =
            runtime::ChessInteractionPhase::Idle;
        modules::LitchMaxEngine::PieceSide stalePlanSide =
            modules::LitchMaxEngine::PieceSide::None;
        Check(!modules::led_frame_composer::TryInferTransitionPieceSide(
                  idleSnapshot, physOcc64, 63U, stalePlanSide),
              "idle tagged phase rejects stale castling plan inference");
        in.interactionSnapshot = &idleSnapshot;
        modules::led_frame_composer::Compose(
            in, ledFrame, edgeFrame, gridFrame);
        Check(!RgbEq(PieceLedAtSquare(ledFrame, 63U),
                     PieceLedAtSquare(ledFrame, 62U)),
              "public composer ignores stale castling plan under idle tag");
    }

    if (g_failures == 0) {
        return 0;
    }

    std::printf("[CASTLING_LED_UX] failures=%d\n", g_failures);
    return 1;
}
