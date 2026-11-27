#include <array>
#include <cstdint>
#include <cstdio>

#include "../support/led_frame_composer_snapshot_adapter.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "modules/ui_color_wheel.hpp"
#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckRgbEq(const modules::Rgb actual, const modules::Rgb expected, const char* msg) {
    if (actual.r == expected.r && actual.g == expected.g && actual.b == expected.b) {
        return;
    }

    std::printf("[FAIL] %s (actual=%u,%u,%u expected=%u,%u,%u)\n",
                msg,
                static_cast<unsigned>(actual.r),
                static_cast<unsigned>(actual.g),
                static_cast<unsigned>(actual.b),
                static_cast<unsigned>(expected.r),
                static_cast<unsigned>(expected.g),
                static_cast<unsigned>(expected.b));
    ++g_failures;
}

bool RgbEq(const modules::Rgb a, const modules::Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

uint8_t ScaleU8Saturating(const uint8_t v, const uint16_t scale256) {
    const uint32_t scaled = static_cast<uint32_t>(v) * static_cast<uint32_t>(scale256);
    const uint32_t out = (scaled + 128U) >> 8;
    return (out > 255U) ? 255U : static_cast<uint8_t>(out);
}

modules::Rgb ScaleRgbSaturating(const modules::Rgb c, const uint16_t scale256) {
    return modules::Rgb{ScaleU8Saturating(c.r, scale256), ScaleU8Saturating(c.g, scale256),
                        ScaleU8Saturating(c.b, scale256)};
}

modules::Rgb ApplyComposerPostScalesToPiece(const modules::Rgb c) {
    static constexpr uint16_t PIECE_BRIGHTER_SCALE256 = 304;          // ~1.19 in Q8.8
    static constexpr uint16_t GLOBAL_BRIGHTNESS_BOOST_SCALE256 = 320; // 1.25 in Q8.8

    return ScaleRgbSaturating(ScaleRgbSaturating(c, PIECE_BRIGHTER_SCALE256), GLOBAL_BRIGHTNESS_BOOST_SCALE256);
}

modules::Rgb ApplyComposerGlobalBoostOnly(const modules::Rgb c) {
    static constexpr uint16_t GLOBAL_BRIGHTNESS_BOOST_SCALE256 = 320; // 1.25 in Q8.8
    return ScaleRgbSaturating(c, GLOBAL_BRIGHTNESS_BOOST_SCALE256);
}

modules::Rgb PieceLedAtSquare(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
                              const uint8_t physicalSq) {
    return ledFrame[hardware::LedIndexForSquare(physicalSq, hardware::SquareLedSlot::Piece)];
}

modules::Rgb SquareLedAtSquare(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
                               const uint8_t physicalSq) {
    return ledFrame[hardware::LedIndexForSquare(physicalSq, hardware::SquareLedSlot::Square0)];
}

} // namespace

int main() {
    std::puts("[SETTLE_COLOR_OWNER_INVARIANT] start");

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_settle_color_owner_invariant.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    // Exercise the radial recipe regardless of the default placement effect.
    settings.Doc()["SETTLE_ANIM_STYLE"] = "ripple";
    settings.Doc()["P1_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_PIECE_WHEEL"] = 128;
    settings.Doc()["P1_END_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_END_PIECE_WHEEL"] = 128;
    settings.Doc()["GAME_FIRST_PLAYER"] = 1U;

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 2000;
    smCfg.settleFlashMs = 200;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::LitchMaxEngine engine;
    engine.Reset();

    modules::EngineDerivedState derived;
    derived.sideToMove = modules::SideToMove::White;

    std::array<bool, 64> occupiedPhysical{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};

    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    runtime::ManualClock clock;
    clock.SetNowMs(0);

    modules::led_frame_composer::ComposeInputs in;
    in.settings = &settings;
    in.firstPlayerIsP2 = false;

    // Squares in 0=a8..63=h1.
    static constexpr uint8_t FROM = 52; // e2
    static constexpr uint8_t TO = 36;   // e4

    // ----- Case 1: Human settling (engine sideToMove is still the mover) -----
    sm.SetExpectedOccupancyMask(1ULL << FROM);
    sm.DebugSetPhysicalOccupancyMask(1ULL << FROM);
    occupiedPhysical[FROM] = true;

    occupiedPhysical[FROM] = false;
    sm.HandleSensorChange(FROM, false);

    occupiedPhysical[TO] = true;
    sm.HandleSensorChange(TO, true);

    Check(sm.IsMoveSettlingActive(), "human: entered Settling");
    Check(!sm.IsCpuMoveSettlingActive(), "human: not CPU settle");

    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    const modules::Rgb settlePiece = PieceLedAtSquare(ledFrame, TO);

    const modules::Rgb expectedP1Piece = ApplyComposerPostScalesToPiece(modules::ui_color_wheel::WheelToRgb(0));
    const modules::Rgb expectedP2Piece = ApplyComposerPostScalesToPiece(modules::ui_color_wheel::WheelToRgb(128));
    CheckRgbEq(settlePiece, expectedP1Piece, "human: settle piece uses mover (P1) color");

    // A completed capture can remain AwaitingAction while the coordinator
    // quiesces an asynchronous engine owner. During that bounded handoff the
    // authoritative FEN still contains the captured opponent on the occupied
    // destination, but the physical piece there is already the mover.
    sm.Reset();
    occupiedPhysical.fill(false);
    fxe::ManualMillis::SetNowMs(0U);

    static constexpr uint8_t CAPTURE_FROM = 36U; // e4
    static constexpr uint8_t CAPTURE_TO = 27U;   // d5
    static constexpr uint8_t WHITE_KING = 60U;   // e1
    static constexpr uint8_t BLACK_KING = 6U;    // g8
    const uint64_t captureMoverOcc =
        (1ULL << CAPTURE_FROM) | (1ULL << WHITE_KING);
    const uint64_t captureOpponentOcc =
        (1ULL << CAPTURE_TO) | (1ULL << BLACK_KING);
    const uint64_t captureBefore = captureMoverOcc | captureOpponentOcc;
    Check(engine.SetFen("6k1/8/8/3p4/4Q3/8/8/4K3 w - - 0 1"),
          "capture handoff: seed pre-capture engine FEN");
    sm.SetExpectedOccupancyMask(captureBefore);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(captureMoverOcc,
                                                 captureOpponentOcc);
    sm.SetLegalCaptureTargetsMask(1ULL << CAPTURE_TO, true);
    sm.DebugSetPhysicalOccupancyMask(captureBefore);

    sm.HandleSensorChange(CAPTURE_TO, false);
    sm.HandleSensorChange(CAPTURE_FROM, false);
    sm.HandleSensorChange(CAPTURE_TO, true);
    Check(sm.IsMoveSettlingActive(),
          "capture handoff: physical capture enters settling");

    fxe::ManualMillis::AdvanceMs(2000U);
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "capture handoff: settle expiry awaits engine action");
    Check(sm.GetPhysicalOccupancyMask() ==
              ((captureBefore & ~(1ULL << CAPTURE_FROM)) | (1ULL << CAPTURE_TO)),
          "capture handoff: mover physically occupies captured destination");

    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    CheckRgbEq(PieceLedAtSquare(ledFrame, CAPTURE_TO),
               expectedP1Piece,
               "capture handoff: destination keeps mover color before engine acknowledgement");

    // Undo runs after the engine has already rebuilt the pre-move FEN. Once
    // the capturing queen is back on e4 but before the captured pawn is
    // restored to d5, the reverse relocation source must not lend the pawn's
    // side to the queen.
    sm.Reset();
    fxe::ManualMillis::SetNowMs(0U);
    Check(engine.SetFen("6k1/8/8/3p4/4Q3/8/8/4K3 w - - 0 1"),
          "capture undo: seed rebuilt pre-move engine FEN");
    const uint64_t captureAfter =
        (1ULL << CAPTURE_TO) | (1ULL << WHITE_KING) |
        (1ULL << BLACK_KING);
    sm.SetExpectedOccupancyMask(captureAfter);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(
        1ULL << BLACK_KING,
        (1ULL << CAPTURE_TO) | (1ULL << WHITE_KING));
    sm.DebugSetPhysicalOccupancyMask(captureAfter);
    Check(sm.BeginUndoPrompt(CAPTURE_FROM,
                             CAPTURE_TO,
                             modules::PersistedSide::White,
                             'Q',
                             'p',
                             CAPTURE_TO),
          "capture undo: begin restore choreography");
    sm.HandleSensorChange(CAPTURE_TO, false);
    sm.HandleSensorChange(CAPTURE_FROM, true);
    const runtime::PhysicalTransitionPlan& undoPlan =
        sm.GetChessInteractionModel().phase.plan;
    Check(undoPlan.kind == runtime::PhysicalTransitionKind::Undo &&
              (undoPlan.progress.relocationsPlacedMask & 0x01U) != 0U &&
              undoPlan.restorationRequired &&
              !undoPlan.progress.restorationComplete,
          "capture undo: mover is restored before the captured pawn");
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    const modules::Rgb undoRestoredMover =
        PieceLedAtSquare(ledFrame, CAPTURE_FROM);
    Check(undoRestoredMover.r > undoRestoredMover.g &&
              undoRestoredMover.r > undoRestoredMover.b,
          "capture undo: restored mover keeps P1 hue through restore overlay");

    // A directed CPU capture may be performed source-first even though the
    // victim is still on the destination. Until placement progress is
    // recorded, the pending victim remains detected but its Piece LEDs are off.
    sm.Reset();
    fxe::ManualMillis::SetNowMs(0U);
    static constexpr uint8_t CPU_FROM = 12U; // e7 rook
    static constexpr uint8_t CPU_TO = 28U;   // e5
    Check(engine.SetFen("6k1/4r3/8/4Q3/8/8/8/4K3 b - - 0 1"),
          "directed capture: seed victim-present engine FEN");
    const uint64_t cpuMoverOcc =
        (1ULL << CPU_FROM) | (1ULL << BLACK_KING);
    const uint64_t cpuOpponentOcc =
        (1ULL << CPU_TO) | (1ULL << WHITE_KING);
    const uint64_t cpuBefore = cpuMoverOcc | cpuOpponentOcc;
    sm.SetExpectedOccupancyMask(cpuBefore);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(cpuMoverOcc,
                                                 cpuOpponentOcc);
    sm.DebugSetPhysicalOccupancyMask(cpuBefore);
    Check(sm.QueueCpuMove(CPU_FROM, CPU_TO, true, CPU_TO, '\0'),
          "directed capture: queue CPU capture");
    sm.HandleSensorChange(CPU_FROM, false);
    const runtime::PhysicalTransitionPlan& cpuPlan =
        sm.GetChessInteractionModel().phase.plan;
    Check(cpuPlan.source == runtime::PhysicalTransitionSource::Directed &&
              (cpuPlan.progress.relocationsLiftedMask & 0x01U) != 0U &&
              (cpuPlan.progress.relocationsPlacedMask & 0x01U) == 0U &&
              (sm.GetPhysicalOccupancyMask() & (1ULL << CPU_TO)) != 0ULL,
          "directed capture: source is lifted while victim remains");
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    CheckRgbEq(PieceLedAtSquare(ledFrame, CPU_TO),
               modules::RGB_BLACK,
               "directed capture: pending victim Piece LEDs are zero until physically removed");
    Check(!RgbEq(PieceLedAtSquare(ledFrame, CPU_TO), expectedP2Piece),
          "directed capture: victim does not borrow CPU mover color");

    // Restore the quiet-move fixture for the remaining settle checks.
    sm.Reset();
    engine.Reset();
    fxe::ManualMillis::SetNowMs(0U);
    sm.SetExpectedOccupancyMask(1ULL << FROM);
    sm.DebugSetPhysicalOccupancyMask(1ULL << FROM);
    occupiedPhysical.fill(false);
    occupiedPhysical[FROM] = true;
    occupiedPhysical[FROM] = false;
    sm.HandleSensorChange(FROM, false);
    occupiedPhysical[TO] = true;
    sm.HandleSensorChange(TO, true);

    clock.AdvanceMs(220U);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> normalRippleFrame{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> minimalRippleFrame{};
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, normalRippleFrame, edgeFrame, gridFrame);
    settings.Doc()["MINIMAL_ANIMATIONS"] = true;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, minimalRippleFrame, edgeFrame, gridFrame);
    Check(!RgbEq(SquareLedAtSquare(normalRippleFrame, TO + 1U),
                 SquareLedAtSquare(minimalRippleFrame, TO + 1U)),
          "human: placement sends a subtle ripple beyond the destination");
    CheckRgbEq(PieceLedAtSquare(minimalRippleFrame, TO),
               expectedP1Piece,
               "human: Minimal Animations keeps the placed piece readable");
    settings.Doc()["MINIMAL_ANIMATIONS"] = false;

    // ----- Case 2: Abort settling triggers afterglow using mover color -----
    occupiedPhysical[TO] = false;
    sm.HandleSensorChange(TO, false);

    Check(sm.IsHumanPieceInHand(), "abort: returns to PickedUp");

    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    const modules::Rgb afterglowSquare = SquareLedAtSquare(ledFrame, TO);

    const modules::Rgb expectedAfterglow = ApplyComposerGlobalBoostOnly(modules::ui_color_wheel::WheelToRgb(0));
    CheckRgbEq(afterglowSquare, expectedAfterglow, "abort: afterglow uses mover (P1) color");

    clock.AdvanceMs(220U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, normalRippleFrame, edgeFrame, gridFrame);
    settings.Doc()["MINIMAL_ANIMATIONS"] = true;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, minimalRippleFrame, edgeFrame, gridFrame);
    Check(!RgbEq(SquareLedAtSquare(normalRippleFrame, TO + 1U),
                 SquareLedAtSquare(minimalRippleFrame, TO + 1U)),
          "abort: lifted destination leaves a subtle radial afterglow");
    settings.Doc()["MINIMAL_ANIMATIONS"] = false;

    occupiedPhysical[FROM] = true;
    sm.HandleSensorChange(FROM, true);
    Check(!sm.IsHumanPieceInHand(),
          "restore: returning the lifted piece reaches Idle");
    clock.AdvanceMs(220U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, normalRippleFrame, edgeFrame, gridFrame);
    settings.Doc()["MINIMAL_ANIMATIONS"] = true;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, minimalRippleFrame, edgeFrame, gridFrame);
    Check(!RgbEq(SquareLedAtSquare(normalRippleFrame, FROM + 1U),
                 SquareLedAtSquare(minimalRippleFrame, FROM + 1U)),
          "restore: returning a piece blooms subtly from its restored square");
    settings.Doc()["MINIMAL_ANIMATIONS"] = false;

    // ----- Case 3: CPU settling captures identity before acknowledgement -----
    sm.Reset();
    occupiedPhysical.fill(false);

    // Minimal expected occupancy: one piece at FROM.
    sm.SetExpectedOccupancyMask(1ULL << FROM);
    sm.SetExpectedSideToMove(modules::PersistedSide::White);
    sm.DebugSetPhysicalOccupancyMask(1ULL << FROM);

    occupiedPhysical[FROM] = true;

    Check(sm.QueueCpuMove(FROM, TO, false, 0, '\0'), "cpu: queued move");

    occupiedPhysical[FROM] = false;
    sm.HandleSensorChange(FROM, false);
    occupiedPhysical[TO] = true;
    sm.HandleSensorChange(TO, true);

    // Presentation may already show the next player when composition happens,
    // but the production settle start reads the pre-action acknowledged side.
    derived.sideToMove = modules::SideToMove::Black;

    sm.Tick();

    Check(sm.IsMoveSettlingActive(), "cpu: entered Settling");
    Check(sm.IsCpuMoveSettlingActive(), "cpu: CPU settle active");
    Check(sm.GetRenderState().settleMoverSideKnown &&
              !sm.GetRenderState().settleMoverIsBlack,
          "cpu: acknowledged White mover is latched before action receipt");

    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    const modules::Rgb cpuSettlePiece = PieceLedAtSquare(ledFrame, TO);

    CheckRgbEq(cpuSettlePiece, expectedP1Piece, "cpu: settle piece uses CPU mover (P1/White) color");

    // ----- Case 4: CPU castling hands the prompt over to the rook without red fault styling -----
    sm.Reset();
    occupiedPhysical.fill(false);

    settings.Doc()["GAME_FIRST_PLAYER"] = 2U;
    in.firstPlayerIsP2 = true;
    derived.sideToMove = modules::SideToMove::White;

    static constexpr uint8_t WHITE_KING_FROM = 60; // e1
    static constexpr uint8_t WHITE_KING_TO = 62;   // g1
    static constexpr uint8_t WHITE_ROOK_FROM = 63; // h1
    static constexpr uint8_t WHITE_ROOK_TO = 61;   // f1

    const uint64_t whiteOcc = (1ULL << 56) | (1ULL << WHITE_KING_FROM) | (1ULL << WHITE_ROOK_FROM);
    const uint64_t blackOcc = (1ULL << 0) | (1ULL << 4) | (1ULL << 7);
    const uint64_t castleOcc = whiteOcc | blackOcc;

    Check(engine.SetFen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"), "cpu castle: seed engine fen");
    sm.SetExpectedOccupancyMask(castleOcc);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(whiteOcc, blackOcc);
    sm.DebugSetPhysicalOccupancyMask(castleOcc);

    occupiedPhysical[0] = true;
    occupiedPhysical[4] = true;
    occupiedPhysical[7] = true;
    occupiedPhysical[56] = true;
    occupiedPhysical[WHITE_KING_FROM] = true;
    occupiedPhysical[WHITE_ROOK_FROM] = true;

    Check(sm.QueueCpuMove(WHITE_KING_FROM, WHITE_KING_TO, false, 0, '\0'), "cpu castle: queue castling move");

    const modules::Rgb expectedSelectedEndAvailablePrompt =
        ApplyComposerGlobalBoostOnly(modules::ui_color_wheel::WheelToRgb(144U));
    const auto sourcePrompt = ApplyComposerGlobalBoostOnly(
        modules::ui_color_wheel::WheelToRgb(160U));
    const auto targetInvitation = expectedSelectedEndAvailablePrompt;

    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    CheckRgbEq(SquareLedAtSquare(ledFrame, WHITE_KING_FROM),
               ApplyComposerGlobalBoostOnly(modules::ui_color_wheel::WheelToRgb(128U)),
               "cpu castle: king source travel uses the mover's actual physical-end piece palette");
    CheckRgbEq(ledFrame[hardware::LedIndexForSquare(WHITE_KING_TO, hardware::SquareLedSlot::Square2)],
               targetInvitation,
               "cpu castle: waiting king target rail keeps the exact full selected-end landing invitation");
    Check(!RgbEq(SquareLedAtSquare(ledFrame, WHITE_ROOK_TO),
                 expectedSelectedEndAvailablePrompt),
          "cpu castle: does not prompt the rook destination before the king");

    occupiedPhysical[WHITE_KING_FROM] = false;
    sm.HandleSensorChange(WHITE_KING_FROM, false);
    Check(sm.GetChessInteractionModel().phase.requiredAction ==
              runtime::RequiredPhysicalAction::PlacePrimary,
          "cpu castle: king pickup advances to king placement");
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    CheckRgbEq(ledFrame[hardware::LedIndexForSquare(WHITE_KING_TO, hardware::SquareLedSlot::Square2)],
               targetInvitation,
               "cpu castle: held king target rail retains the exact full landing invitation");
    Check(!RgbEq(SquareLedAtSquare(ledFrame, WHITE_ROOK_TO),
                 expectedSelectedEndAvailablePrompt),
          "cpu castle: rook destination stays inactive while king is held");

    occupiedPhysical[WHITE_KING_TO] = true;
    sm.HandleSensorChange(WHITE_KING_TO, true);

    runtime::InteractionModelSnapshot cpuCastleKingSnapshot =
        test_support::BuildInteractionSnapshot(sm, derived, &engine);
    Check(cpuCastleKingSnapshot.render.settleAfterglowActive &&
              cpuCastleKingSnapshot.render.settleAfterglowSquare ==
                  WHITE_KING_TO &&
              !cpuCastleKingSnapshot.render.settleAfterglowAbort,
          "cpu castle: king placement starts positive render-only feedback");
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    const modules::Rgb cpuCastleKingFeedbackEarly =
        SquareLedAtSquare(ledFrame, WHITE_KING_TO);

    CheckRgbEq(SquareLedAtSquare(ledFrame, WHITE_ROOK_FROM),
               sourcePrompt,
               "cpu castle: waiting unselected rook source rail uses the full active prompt after king placement");
    CheckRgbEq(SquareLedAtSquare(ledFrame, WHITE_ROOK_TO),
               targetInvitation,
               "cpu castle: waiting rook destination rail uses the exact full landing invitation after king placement");
    CheckRgbEq(PieceLedAtSquare(ledFrame, WHITE_KING_TO),
               ApplyComposerPostScalesToPiece(
                   modules::ui_color_wheel::WheelToRgb(128U)),
               "cpu castle: placed white king keeps the selected physical-end palette");

    fxe::ManualMillis::AdvanceMs(100U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    Check(!RgbEq(cpuCastleKingFeedbackEarly,
                 SquareLedAtSquare(ledFrame, WHITE_KING_TO)),
          "cpu castle: king feedback animates without delaying the rook prompt");
    Check(sm.GetChessInteractionModel().phase.requiredAction ==
              runtime::RequiredPhysicalAction::LiftSecondary &&
              sm.GetChessInteractionModel().phase.pendingActionToken == 0U,
          "cpu castle: partial feedback exposes no engine action");

    occupiedPhysical[WHITE_ROOK_FROM] = false;
    sm.HandleSensorChange(WHITE_ROOK_FROM, false);
    occupiedPhysical[WHITE_ROOK_TO] = true;
    sm.HandleSensorChange(WHITE_ROOK_TO, true);
    const uint32_t cpuCastleActionToken =
        sm.GetChessInteractionModel().phase.pendingActionToken;
    Check(sm.IsCpuMoveSettlingActive() && cpuCastleActionToken != 0U,
          "cpu castle: rook placement starts final feedback and one action");
    derived.sideToMove = modules::SideToMove::Black;
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    CheckRgbEq(PieceLedAtSquare(ledFrame, WHITE_ROOK_TO),
               ApplyComposerPostScalesToPiece(
                   modules::ui_color_wheel::WheelToRgb(128U)),
               "cpu castle: final rook feedback keeps the mover palette");
    Check(!RgbEq(SquareLedAtSquare(ledFrame, WHITE_KING_TO),
                 modules::RGB_BLACK) &&
              !RgbEq(SquareLedAtSquare(ledFrame, WHITE_ROOK_TO),
                     modules::RGB_BLACK),
          "cpu castle: king afterglow and rook settle may overlap");
    fxe::ManualMillis::AdvanceMs(200U);
    sm.Tick();
    Check(sm.GetChessInteractionModel().phase.pendingActionToken ==
              cpuCastleActionToken,
          "cpu castle: feedback expiry cannot emit a second action");

    // ----- Case 5: MOVE_CONFIRM=5s stretches one progressive visual presentation -----
    fxe::ManualMillis::SetNowMs(0U);
    runtime::GameStateMachine settle5Sm(smCfg);
    settle5Sm.Reset();
    settle5Sm.SetMoveConfirmPolicy(5000U, false);
    settle5Sm.SetExpectedOccupancyMask(1ULL << FROM);
    settle5Sm.SetExpectedMoverAndOpponentOccupancyMasks(1ULL << FROM,
                                                        0ULL);
    settle5Sm.DebugSetPhysicalOccupancyMask(1ULL << FROM);
    settle5Sm.HandleSensorChange(FROM, false);
    settle5Sm.SetValidDestMask(1ULL << TO);
    settle5Sm.HandleSensorChange(TO, true);
    Check(settle5Sm.IsMoveSettlingActive(),
          "settle_5s visual: entered settling");
    Check(settle5Sm.GetSettlePresentationDurationMs() == 5000U,
          "settle_5s visual: progression follows the selected 5s interval");

    in.firstPlayerIsP2 = false;
    derived.sideToMove = modules::SideToMove::White;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> settleFrameA{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> settleFrameB{};

    fxe::ManualMillis::SetNowMs(100U);
    test_support::ComposeWithSnapshot(
        in, settle5Sm, derived, nullptr,
        settleFrameA, edgeFrame, gridFrame);
    fxe::ManualMillis::SetNowMs(2100U);
    test_support::ComposeWithSnapshot(
        in, settle5Sm, derived, nullptr,
        settleFrameB, edgeFrame, gridFrame);
    Check(!RgbEq(SquareLedAtSquare(settleFrameB, TO),
                 SquareLedAtSquare(settleFrameA, TO)),
          "settle_5s visual: early progression does not restart at +2s");

    fxe::ManualMillis::SetNowMs(1900U);
    test_support::ComposeWithSnapshot(
        in, settle5Sm, derived, nullptr,
        settleFrameA, edgeFrame, gridFrame);
    fxe::ManualMillis::SetNowMs(3900U);
    test_support::ComposeWithSnapshot(
        in, settle5Sm, derived, nullptr,
        settleFrameB, edgeFrame, gridFrame);
    Check(!RgbEq(SquareLedAtSquare(settleFrameB, TO),
                 SquareLedAtSquare(settleFrameA, TO)),
          "settle_5s visual: later progression does not restart at +2s");

    if (g_failures == 0) {
        std::puts("[SETTLE_COLOR_OWNER_INVARIANT] ok");
        return 0;
    }

    std::printf("[SETTLE_COLOR_OWNER_INVARIANT] failures=%d\n", g_failures);
    return 1;
}
