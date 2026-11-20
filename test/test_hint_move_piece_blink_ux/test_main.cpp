#include <array>
#include <cstdint>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"
#include "../support/led_frame_composer_snapshot_adapter.hpp"

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

bool RgbEq(const modules::Rgb a, const modules::Rgb b) {
    return (a.r == b.r) && (a.g == b.g) && (a.b == b.b);
}

} // namespace

int main() {
    // Validate: while a best-move hint is displayed (Idle + hintDisplayRequested),
    // blink the mover piece LED with the CPU's existing 600 ms on / 200 ms off cue.

    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_hint_move_piece_blink_ux.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    // Fixed wheels for deterministic piece color.
    settings.Doc()["P1_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_PIECE_WHEEL"] = 128;
    settings.Doc()["WHITE_SQUARES_ON"] = false;

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 0;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::LitchMaxEngine engine;
    engine.Reset();

    modules::EngineDerivedState derived;

    // Minimal position: a single white pawn on e2.
    const char* fen = "8/8/8/8/8/8/4P3/8 w - - 0 1";
    Check(engine.SetFen(fen), "SetFen pawn e2");
    derived.RefreshFromEngine(engine);

    // Square indices: 0=a8 ... 63=h1. e2 = 52, e4 = 36.
    static constexpr uint8_t FROM_E2 = 52;
    static constexpr uint8_t TO_E4 = 36;

    const uint64_t occ64 = (1ULL << FROM_E2);
    sm.SetExpectedOccupancyMask(occ64);
    sm.SetExpectedMoverAndOpponentOccupancyMasks(occ64, 0ULL);
    sm.DebugSetPhysicalOccupancyMask(occ64);

    sm.SetHintMoveSquares(FROM_E2, TO_E4);
    sm.SetHintDisplayRequested(true);

    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "state is Idle");

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};

    const std::array<bool, 64> occupiedPhysical = Occ64ToOccupiedPhysical(occ64);

    modules::led_frame_composer::ComposeInputs in{};
    in.settings = &settings;
    in.firstPlayerIsP2 = false;
    in.menuOverlay = nullptr;
    in.menuActive = false;

    // Supporting renderer seam; the raw-control VirtualLitch journey owns input proof.
    // t=0ms => source piece retains its ordinary color in the ON phase.
    fxe::ManualMillis::SetNowMs(0);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    const modules::Rgb startRgb = PieceLedAtSquare(ledFrame, FROM_E2);
    Check(startRgb.r > 0U && startRgb.g == 0U && startRgb.b == 0U,
          "hint mover piece LED keeps the actual P1 red palette in the CPU cue ON phase");
    const auto targetAtStart = ledFrame[hardware::LedIndexForSquare(
        TO_E4, hardware::SquareLedSlot::Square2)];
    const auto targetPieceAtStart = PieceLedAtSquare(ledFrame, TO_E4);
    Check(RgbEq(ledFrame[hardware::LedIndexForSquare(
              37U, hardware::SquareLedSlot::Square0)], modules::RGB_BLACK) &&
              !RgbEq(PieceLedAtSquare(ledFrame, TO_E4), modules::RGB_BLACK),
          "Hint has a lit actual landing marker without the retired neighboring cross");

    // t=700ms => the same OFF phase used by a queued CPU move.
    fxe::ManualMillis::SetNowMs(700);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
    Check(RgbEq(PieceLedAtSquare(ledFrame, FROM_E2), modules::RGB_BLACK),
          "hint mover piece LED uses the CPU cue OFF phase");
    const auto targetLeft = ledFrame[hardware::LedIndexForSquare(TO_E4, hardware::SquareLedSlot::Square0)];
    const auto targetRight = ledFrame[hardware::LedIndexForSquare(TO_E4, hardware::SquareLedSlot::Square2)];
    const auto targetPiece = PieceLedAtSquare(ledFrame, TO_E4);
    // e2-e4 takes 960 ms over six emitter nodes. At 700 ms the incoming e4
    // emitter has 165/256 head weight, followed by the 81/255 shared envelope.
    // Its available-color floor keeps green in the rounded Top/Bottom mean;
    // P1's red head must raise red and lower green, without erasing that floor.
    Check(targetPieceAtStart.r > 0U && targetPieceAtStart.g > 0U && targetPieceAtStart.b == 0U &&
              targetLeft.r > 0U && targetLeft.g > 0U && targetLeft.b == 0U &&
              targetRight.r > 0U && targetRight.g > 0U && targetRight.b == 0U &&
              targetRight.g < targetAtStart.g &&
              targetPiece.r > targetPieceAtStart.r && targetPiece.r <= startRgb.r &&
              targetPiece.g > 0U && targetPiece.g < targetPieceAtStart.g && targetPiece.b == 0U,
          "independent P1 travel advances beyond the static landing floor while retaining both visible rails");

    // Renderer-seam regressions: identical physical evidence must produce
    // identical frames whether the background worker is active or idle.
    // These snapshots deliberately bypass physical event generation.
    auto snapshot = test_support::BuildInteractionSnapshot(sm, derived, &engine);
    snapshot.render.hintMoveValid = false;
    snapshot.render.hintDisplayRequested = true;
    snapshot.system.nowMs = 100U; // Before the first 500 ms suggestion window.
    in.interactionSnapshot = &snapshot;
    const auto framesEqual = [](const auto& a, const auto& b) {
        for (size_t i = 0; i < a.size(); ++i) {
            if (!RgbEq(a[i], b[i])) return false;
        }
        return true;
    };
    const auto checkBackgroundInvisible = [&]() {
        snapshot.game.hintThinking = false;
        modules::led_frame_composer::Compose(in, ledFrame, edgeFrame, gridFrame);
        const auto board = ledFrame;
        const auto edge = edgeFrame;
        snapshot.game.hintThinking = true;
        modules::led_frame_composer::Compose(in, ledFrame, edgeFrame, gridFrame);
        Check(framesEqual(board, ledFrame), "background Hint preserves physical board cues");
        Check(framesEqual(edge, edgeFrame), "background Hint preserves the edge frame, including before the first result");
    };
    checkBackgroundInvisible();
    snapshot.game.hintThinking = false;
    modules::led_frame_composer::Compose(in, ledFrame, edgeFrame, gridFrame);
    const auto quietBoard = ledFrame;
    for (unsigned cue = 0; cue < 3; ++cue) {
        snapshot.interaction.humanIllegalMask = cue == 0 ? (1ULL << FROM_E2) : 0ULL;
        snapshot.interaction.illegalOn = cue != 2;
        snapshot.interaction.illegalPeriodMs = 600U;
        snapshot.interaction.illegalElapsedMs = 1500U;
        snapshot.interaction.kingSafetyPromptActive = cue == 1;
        snapshot.interaction.kingSafetyKingSquare = FROM_E2;
        snapshot.interaction.kingSafetyAttackerSquare = TO_E4;
        snapshot.interaction.captureReturnMask = cue == 2 ? (1ULL << TO_E4) : 0ULL;
        snapshot.interaction.captureReturnOn = cue == 2;
        checkBackgroundInvisible();
        Check(!framesEqual(quietBoard, ledFrame), "required correction is visibly distinct from quiet board");
    }

    if (g_failures == 0) {
        return 0;
    }

    std::printf("[HINT_MOVE_PIECE_BLINK_UX] failures=%d\n", g_failures);
    return 1;
}
