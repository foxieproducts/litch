#include <array>
#include <cstdint>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
#include "runtime/app_event.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/clock.hpp"
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

void AdvanceAndTick(runtime::ManualClock& clock, runtime::GameStateMachine& sm, const uint32_t deltaMs) {
    clock.AdvanceMs(deltaMs);
    runtime::DispatchAppEventToStateMachine(sm, runtime::AppEvent::MakeTick(clock.NowMs()));
}

void Sensor(runtime::ManualClock& clock, runtime::GameStateMachine& sm, const uint8_t square, const bool occ) {
    runtime::DispatchAppEventToStateMachine(sm, runtime::AppEvent::MakeSensorDelta(clock.NowMs(), square, occ));
}

void PressP1(runtime::ManualClock& clock, runtime::GameStateMachine& sm) {
    runtime::DispatchAppEventToStateMachine(
        sm, runtime::AppEvent::MakeButton(clock.NowMs(), modules::UserButton::P1X, fxe::Button::Event::Press));
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

uint16_t Intensity(const modules::Rgb c) {
    return static_cast<uint16_t>(c.r) + static_cast<uint16_t>(c.g) + static_cast<uint16_t>(c.b);
}

bool NearWhite(const modules::Rgb c) {
    return c.r >= 220 && c.g >= 220 && c.b >= 220;
}

bool HasBrightNeutralPixel(const std::array<modules::Rgb, 64>& grid) {
    for (const modules::Rgb pixel : grid) {
        if (pixel.r >= 200U && pixel.g >= 200U && pixel.b >= 200U) {
            return true;
        }
    }
    return false;
}

uint8_t ChannelSpread(const modules::Rgb c) {
    const uint8_t maxCh =
        (c.r > c.g) ? ((c.r > c.b) ? c.r : c.b) : ((c.g > c.b) ? c.g : c.b);
    const uint8_t minCh =
        (c.r < c.g) ? ((c.r < c.b) ? c.r : c.b) : ((c.g < c.b) ? c.g : c.b);
    return static_cast<uint8_t>(maxCh - minCh);
}

} // namespace

int main() {
    std::puts("[UNDO_CAPTURE_RESTORE_PHASE_UX] start");

    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_undo_capture_restore_phase_ux.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    // Deterministic wheels.
    settings.Doc()["P1_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_PIECE_WHEEL"] = 128;

    runtime::GameStateMachineConfig smCfg;
    smCfg.settleDurationMs = 20;
    smCfg.settleFlashMs = 10;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    runtime::ManualClock clock;

    static constexpr uint8_t SAME_TYPE_CANDIDATE_SQUARE = 8;

    // Simulate a move 12 -> 28 (treat as capture-like for undo UX).
    sm.SetExpectedOccupancyMask((1ULL << 12) |
                                (1ULL << SAME_TYPE_CANDIDATE_SQUARE));
    sm.DebugSetPhysicalOccupancyMask((1ULL << 12) |
                                     (1ULL << SAME_TYPE_CANDIDATE_SQUARE));

    Sensor(clock, sm, 12, false);
    sm.SetValidDestMask(1ULL << 28);
    Sensor(clock, sm, 28, true);
    AdvanceAndTick(clock, sm, 25);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "forward settle exposes its action");

    const uint32_t forwardToken =
        sm.GetChessInteractionModel().phase.pendingActionToken;
    Check(forwardToken != 0U, "forward move exposes action token");
    sm.HandleInteractionActionResult(forwardToken, true);

    // Request an explicit reducer-owned undo prompt.
    Check(sm.BeginUndoPrompt(12U, 28U,
                             modules::PersistedSide::White,
                             'P', 'p', 28U),
          "capture undo descriptor accepted");
    Check(sm.IsUndoPromptActive(), "undo prompt active");

    modules::LitchMaxEngine engine;
    engine.Reset();
    Check(engine.SetFen("4k3/p7/8/4P3/8/8/8/4K3 w - - 0 1"),
          "engine set forward fen with restore candidate");

    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};

    // Phase 1: mover not returned yet (piece still on 28).
    {
        const uint64_t physOcc64 = sm.GetPhysicalOccupancyMask();
        const std::array<bool, 64> occupiedPhysical = Occ64ToOccupiedPhysical(physOcc64);

        modules::led_frame_composer::ComposeInputs in;
        in.settings = &settings;
        in.firstPlayerIsP2 = false;

        fxe::ManualMillis::SetNowMs(0);
        test_support::ComposeWithSnapshot(
            in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);

        Check(!NearWhite(gridFrame[12]), "phase1: return-to square is not near-white");
        Check(ChannelSpread(gridFrame[12]) >= 24U, "phase1: return-to square uses saturated player color");
        Check(!NearWhite(gridFrame[28]), "phase1: restore square not shown as restore prompt yet");
    }

    // Phase 2: mover returned (12 occupied, 28 empty). Restore square must be obvious.
    {
        sm.DebugSetPhysicalOccupancyMask((1ULL << 12) | (1ULL << SAME_TYPE_CANDIDATE_SQUARE));
        const uint64_t physOcc64 = sm.GetPhysicalOccupancyMask();
        const uint32_t pulseBaseMs = clock.NowMs();
        const std::array<bool, 64> occupiedPhysical = Occ64ToOccupiedPhysical(physOcc64);

        modules::led_frame_composer::ComposeInputs in;
        in.settings = &settings;
        in.firstPlayerIsP2 = false;

        fxe::ManualMillis::SetNowMs(pulseBaseMs);
        test_support::ComposeWithSnapshot(
            in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);

        modules::led_frame_composer::internal::ComposeContext restoreCtx;
        runtime::InteractionModelSnapshot contextSnapshot0;
        Check(test_support::BuildComposeContextWithSnapshot(
                  in, sm, derived, &engine, restoreCtx, contextSnapshot0),
              "phase2: build capture-restore compose context");
        const auto restoreRuntime =
            modules::led_frame_composer::internal::BuildBoardComposeRuntime(
                restoreCtx);
        Check(restoreCtx.undoRestorePhase,
              "phase2: capture Undo enters restore guidance");
        Check((restoreRuntime.undoCandidateBlinkMask &
               (1ULL << SAME_TYPE_CANDIDATE_SQUARE)) != 0ULL,
              "phase2: missing captured piece identifies matching occupied pieces");
        Check((restoreRuntime.undoRestoreInitialMask & (1ULL << 9U)) != 0ULL,
              "phase2: missing captured pawn identifies conventional starting squares");

        modules::BoardLedFrame overlayOnlyLeds{};
        std::array<modules::Rgb, 64> overlayOnlyGrid{};
        modules::led_frame_composer::internal::ApplyCenterTextOverlay(
            restoreCtx, overlayOnlyLeds, overlayOnlyGrid);
        Check(!HasBrightNeutralPixel(overlayOnlyGrid),
              "phase2: capture restore uses spatial guidance without a board-letter glyph");

        runtime::GameStateMachine baseline(smCfg);
        baseline.Reset();
        baseline.SetExpectedOccupancyMask((1ULL << 12) | (1ULL << SAME_TYPE_CANDIDATE_SQUARE));
        baseline.DebugSetPhysicalOccupancyMask((1ULL << 12) | (1ULL << SAME_TYPE_CANDIDATE_SQUARE));
        const std::array<bool, 64> baselineOccupied =
            Occ64ToOccupiedPhysical(baseline.GetPhysicalOccupancyMask());

        modules::led_frame_composer::ComposeInputs baselineIn = in;

        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> baselineLedFrame{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> baselineEdgeFrame{};
        std::array<modules::Rgb, 64> baselineGridFrame{};
        test_support::ComposeWithSnapshot(
            baselineIn,
            baseline,
            derived,
            &engine,
            baselineLedFrame,
            baselineEdgeFrame,
            baselineGridFrame);

        const modules::Rgb candidateStart = PieceLedAtSquare(ledFrame, SAME_TYPE_CANDIDATE_SQUARE);
        const modules::Rgb candidateBaseline = PieceLedAtSquare(baselineLedFrame, SAME_TYPE_CANDIDATE_SQUARE);

        Check(!NearWhite(gridFrame[28]), "phase2: restore square is not near-white");
        Check(ChannelSpread(gridFrame[28]) >= 24U,
              "phase2: restore square uses saturated player color (visible even with overlay)");
        Check(Intensity(gridFrame[28]) > static_cast<uint16_t>(Intensity(gridFrame[12]) + 40U),
              "phase2: restore square dominates over return-to square");
        Check(!RgbEq(candidateStart, modules::RGB_BLACK),
              "phase2: same-type restore candidate piece starts fully visible");
        Check(RgbEq(candidateStart, candidateBaseline),
              "phase2: same-type restore candidate piece stays as bright as its normal piece color");
        fxe::ManualMillis::SetNowMs(pulseBaseMs + 400U);
        test_support::ComposeWithSnapshot(
            in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
        const modules::Rgb candidateDark = PieceLedAtSquare(ledFrame, SAME_TYPE_CANDIDATE_SQUARE);
        Check(RgbEq(candidateDark, modules::RGB_BLACK),
              "phase2: same-type restore candidate piece smoothly pulses down to black");

        fxe::ManualMillis::SetNowMs(pulseBaseMs + 800U);
        test_support::ComposeWithSnapshot(
            in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
        const modules::Rgb candidateReturn = PieceLedAtSquare(ledFrame, SAME_TYPE_CANDIDATE_SQUARE);
        Check(RgbEq(candidateReturn, candidateStart),
              "phase2: same-type restore candidate piece returns to full brightness on the next pulse");
        bool startReferenceVisible = false;
        for (uint32_t sampleMs = 0U; sampleMs <= 1600U; sampleMs += 100U) {
            fxe::ManualMillis::SetNowMs(pulseBaseMs + sampleMs);
            test_support::ComposeWithSnapshot(
                in, sm, derived, &engine, ledFrame, edgeFrame, gridFrame);
            // Squares 9 and 18 have the same board parity and are both empty;
            // only 9 is a conventional black-pawn starting square.
            startReferenceVisible =
                startReferenceVisible || !RgbEq(gridFrame[9U], gridFrame[18U]);
        }
        Check(startReferenceVisible,
              "phase2: an empty conventional start square receives a restrained animated reference");
    }

    if (g_failures == 0) {
        std::puts("[UNDO_CAPTURE_RESTORE_PHASE_UX] ok");
        return 0;
    }

    std::printf("[UNDO_CAPTURE_RESTORE_PHASE_UX] failures=%d\n", g_failures);
    return 1;
}
