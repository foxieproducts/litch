#include <array>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/settings_store.hpp"
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

bool RgbEq(const modules::Rgb a, const modules::Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

modules::Rgb ComposeLed(const runtime::GameStateMachine& stateMachine,
                        const modules::SettingsStore& settings,
                        const uint8_t logicalSquare,
                        const hardware::SquareLedSlot slot,
                        const bool flipSides,
                        const bool firstPlayerIsP2,
                        const modules::IEngine* engine) {
    const modules::EngineDerivedState derived;
    runtime::InteractionModelSnapshot snapshot =
        test_support::BuildInteractionSnapshot(
            stateMachine, derived, engine);
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &settings;
    inputs.interactionSnapshot = &snapshot;
    inputs.firstPlayerIsP2 = firstPlayerIsP2;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};
    modules::led_frame_composer::Compose(
        inputs, ledFrame, edgeFrame, gridFrame);
    const auto physicalFrame = hardware::MapSemanticLedFrame(
        {hardware::BoardRevision::V2S3, flipSides, flipSides},
        ledFrame, edgeFrame);
    const auto target=hardware::MapSemanticSquareLedTargets(hardware::BoardRevision::V2S3,
        logicalSquare,slot,flipSides);
    return physicalFrame[target.indices[0U]];
}

modules::Rgb ComposeSquareLed(const runtime::GameStateMachine& stateMachine,
                              const modules::SettingsStore& settings,
                              const uint8_t logicalSquare,
                              const bool flipSides = false,
                              const bool firstPlayerIsP2 = false,
                              const modules::IEngine* engine = nullptr) {
    return ComposeLed(stateMachine,
                      settings,
                      logicalSquare,
                      hardware::SquareLedSlot::Square0,
                      flipSides,
                      firstPlayerIsP2,
                      engine);
}

modules::Rgb ComposePieceLed(const runtime::GameStateMachine& stateMachine,
                             const modules::SettingsStore& settings,
                             const uint8_t logicalSquare,
                             const bool flipSides = false,
                             const bool firstPlayerIsP2 = false,
                             const modules::IEngine* engine = nullptr) {
    return ComposeLed(stateMachine,
                      settings,
                      logicalSquare,
                      hardware::SquareLedSlot::Piece,
                      flipSides,
                      firstPlayerIsP2,
                      engine);
}

} // namespace

int main() {
    std::puts("[EN_PASSANT_SETTLE_BLOCKING] start");

    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    runtime::ManualClock clock(0);
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 100;
    cfg.cpuIdleDelayMs = 999999;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    runtime::ChessPositionState position;
    // Real White e5xf6 en passant removes the Black pawn on f5.
    position.kingMask = (1ULL << 7) | (1ULL << 63);
    position.pawnMask = (1ULL << 28) | (1ULL << 29);
    position.occupiedMask = position.kingMask | position.pawnMask;
    position.moverOccupiedMask = (1ULL << 28) | (1ULL << 63);
    position.opponentOccupiedMask = (1ULL << 29) | (1ULL << 7);
    position.sideToMoveValid = true;
    position.sideToMoveBlack = false;
    position.enPassantTargetSquare = 21U;
    position.enPassantCaptureSquare = 29U;
    sm.DebugSetExpectedPositionModel(position);
    sm.DebugSetPhysicalOccupancyMask(position.occupiedMask);

    // Start a move.
    sm.HandleSensorChange(28, false);
    Check(sm.IsHumanPieceInHand(), "Idle->PickedUp");

    // Only allow destination square 21.
    sm.SetValidDestMask(1ULL << 21);
    sm.HandleSensorChange(21, true);
    Check(sm.IsMoveSettlingActive(), "PickedUp->Settling");

    // Clearing the captured pawn square should finalize immediately even if
    // the settle timer has not yet expired.
    clock.SetNowMs(10);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.HandleSensorChange(29, false);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "clearing capture square exposes the action immediately");

    // Re-enter the same scenario to validate the post-expiry path too.
    sm.Reset();
    sm.DebugSetExpectedPositionModel(position);
    sm.DebugSetPhysicalOccupancyMask(position.occupiedMask);
    sm.HandleSensorChange(28, false);
    Check(sm.IsHumanPieceInHand(), "Idle->PickedUp (again)");
    sm.SetValidDestMask(1ULL << 21);
    sm.HandleSensorChange(21, true);
    Check(sm.IsMoveSettlingActive(), "PickedUp->Settling (again)");

    // After settle duration, finalization is blocked until capture square clears.
    clock.SetNowMs(200);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    Check(sm.IsMoveSettlingActive(),
          "settle blocked while en-passant capture square occupied");

    // Clear the captured pawn square; since the settle timer is already expired,
    // this should finalize immediately.
    sm.HandleSensorChange(29, false);
    Check(sm.GetChessInteractionModel().phase.tag ==
              runtime::ChessInteractionPhase::AwaitingAction,
          "clearing capture square exposes the action after expiry");

    // Renderer regression: after the mover reaches the empty en-passant target,
    // the off-destination captured pawn must keep flashing until it is removed.
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_en_passant_settle_led.json";
    (void)std::remove(settingsConfig.path);
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "LED settings load");
    settings.Doc()["P1_PIECE_WHEEL"] = 0U;
    settings.Doc()["P2_PIECE_WHEEL"] = 128U;
    modules::LitchMaxEngine renderEngine;
    Check(renderEngine.SetFen(
              "7k/8/8/4Pp2/8/8/8/7K w - f6 0 1"),
          "LED render engine loads pre-move pieces");

    clock.SetNowMs(1000U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    runtime::GameStateMachine cpuCaptureSm(cfg);
    cpuCaptureSm.Reset();
    cpuCaptureSm.DebugSetExpectedPositionModel(position);
    cpuCaptureSm.DebugSetPhysicalOccupancyMask(position.occupiedMask);
    Check(cpuCaptureSm.QueueCpuMove(28U, 21U, true, 29U, '\0'),
          "CPU capture comparison prompt queues");

    sm.Reset();
    sm.DebugSetExpectedPositionModel(position);
    sm.DebugSetPhysicalOccupancyMask(position.occupiedMask);
    const modules::Rgb sourceSquareBefore =
        ComposeSquareLed(sm, settings, 28U, false, false, &renderEngine);
    const modules::Rgb alternativeSquareBefore =
        ComposeSquareLed(sm, settings, 20U, false, false, &renderEngine);
    const modules::Rgb sourcePieceBefore =
        ComposePieceLed(sm, settings, 28U, false, false, &renderEngine);
    const modules::Rgb p2SourcePieceBefore =
        ComposePieceLed(sm, settings, 28U, false, true, &renderEngine);
    sm.HandleSensorChange(28, false);
    sm.SetValidDestMask((1ULL << 21) | (1ULL << 20));
    sm.HandleSensorChange(21, true);

    clock.SetNowMs(1200U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    runtime::InteractionModelSnapshot blockedSnapshot =
        test_support::BuildInteractionSnapshot(
            sm, modules::EngineDerivedState{});
    Check(blockedSnapshot.interaction.taggedState.requiredAction ==
              runtime::RequiredPhysicalAction::RemoveCapture,
          "blocked en-passant exposes captured-pawn removal action");
    Check(blockedSnapshot.interaction.taggedState.plan.removalSquare == 29U &&
              !blockedSnapshot.interaction.taggedState.plan.progress.removalComplete,
          "blocked en-passant retains pending capture-square metadata");
    Check(runtime::IsSettlingActive(blockedSnapshot),
          "blocked en-passant is visible as an active settle animation");
    const modules::Rgb sourceSquarePending =
        ComposeSquareLed(sm, settings, 28U, false, false, &renderEngine);
    const modules::Rgb alternativeSquarePending =
        ComposeSquareLed(sm, settings, 20U, false, false, &renderEngine);
    const modules::Rgb targetPiecePending =
        ComposePieceLed(sm, settings, 21U, false, false, &renderEngine);
    const modules::Rgb rotatedTargetPiecePending =
        ComposePieceLed(sm, settings, 21U, true, false, &renderEngine);
    const modules::Rgb p2TargetPiecePending =
        ComposePieceLed(sm, settings, 21U, false, true, &renderEngine);
    Check(RgbEq(sourceSquarePending, sourceSquareBefore),
          "en-passant removal phase clears the original-source highlight");
    Check(RgbEq(alternativeSquarePending, alternativeSquareBefore),
          "en-passant removal phase clears alternative legal-move highlights");
    Check(RgbEq(targetPiecePending, sourcePieceBefore),
          "en-passant destination piece uses the mover's color");
    Check(RgbEq(rotatedTargetPiecePending, sourcePieceBefore),
          "rotated en-passant destination piece uses the mover's color");
    Check(RgbEq(p2TargetPiecePending, p2SourcePieceBefore),
          "P2 mover's en-passant destination uses the P2 color");
    // Observe the next full/off pole, then its opposite. The former 1200/1400
    // samples are symmetric on the approved 600 ms complementary fade.
    constexpr uint32_t REMOVAL_CYCLE_MS = 600U;
    clock.SetNowMs(clock.NowMs() + REMOVAL_CYCLE_MS -
                   sm.GetIllegalFlashElapsedMs() % REMOVAL_CYCLE_MS);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    const modules::Rgb capturedPawnFrameA =
        ComposeSquareLed(sm, settings, 29U, false, false, &renderEngine);
    const modules::Rgb rotatedCapturedPawnFrameA =
        ComposeSquareLed(sm, settings, 29U, true, false, &renderEngine);
    const modules::Rgb capturedPawnRightFrameA = ComposeLed(
        sm, settings, 29U, hardware::SquareLedSlot::Square2,
        false, false, &renderEngine);
    const modules::Rgb rotatedCapturedPawnRightFrameA = ComposeLed(
        sm, settings, 29U, hardware::SquareLedSlot::Square2,
        true, false, &renderEngine);
    const modules::Rgb capturedPawnPieceFrameA =
        ComposePieceLed(sm, settings, 29U, false, false, &renderEngine);
    const modules::Rgb cpuCapturedPawnFrameA =
        ComposeSquareLed(cpuCaptureSm, settings, 29U, false, false,
                         &renderEngine);
    const modules::Rgb cpuCapturedPawnPieceFrameA =
        ComposePieceLed(cpuCaptureSm, settings, 29U, false, false,
                        &renderEngine);
    const modules::Rgb rotatedCpuCapturedPawnFrameA =
        ComposeSquareLed(cpuCaptureSm, settings, 29U, true, false,
                         &renderEngine);
    const modules::Rgb p2CapturedPawnFrameA =
        ComposeSquareLed(sm, settings, 29U, false, true, &renderEngine);
    const modules::Rgb p2CapturedPawnPieceFrameA =
        ComposePieceLed(sm, settings, 29U, false, true, &renderEngine);
    const modules::Rgb p2CpuCapturedPawnFrameA =
        ComposeSquareLed(cpuCaptureSm, settings, 29U, false, true,
                         &renderEngine);
    const modules::Rgb p2CpuCapturedPawnPieceFrameA =
        ComposePieceLed(cpuCaptureSm, settings, 29U, false, true,
                        &renderEngine);

    clock.SetNowMs(clock.NowMs() + 300U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    const modules::Rgb capturedPawnFrameB =
        ComposeSquareLed(sm, settings, 29U, false, false, &renderEngine);
    const modules::Rgb rotatedCapturedPawnFrameB =
        ComposeSquareLed(sm, settings, 29U, true, false, &renderEngine);
    const modules::Rgb capturedPawnRightFrameB = ComposeLed(
        sm, settings, 29U, hardware::SquareLedSlot::Square2,
        false, false, &renderEngine);
    const modules::Rgb rotatedCapturedPawnRightFrameB = ComposeLed(
        sm, settings, 29U, hardware::SquareLedSlot::Square2,
        true, false, &renderEngine);
    const modules::Rgb capturedPawnPieceFrameB =
        ComposePieceLed(sm, settings, 29U, false, false, &renderEngine);
    const modules::Rgb cpuCapturedPawnFrameB =
        ComposeSquareLed(cpuCaptureSm, settings, 29U, false, false,
                         &renderEngine);
    const modules::Rgb cpuCapturedPawnPieceFrameB =
        ComposePieceLed(cpuCaptureSm, settings, 29U, false, false,
                        &renderEngine);
    const modules::Rgb rotatedCpuCapturedPawnFrameB =
        ComposeSquareLed(cpuCaptureSm, settings, 29U, true, false,
                         &renderEngine);
    const modules::Rgb p2CapturedPawnFrameB =
        ComposeSquareLed(sm, settings, 29U, false, true, &renderEngine);
    const modules::Rgb p2CapturedPawnPieceFrameB =
        ComposePieceLed(sm, settings, 29U, false, true, &renderEngine);
    const modules::Rgb p2CpuCapturedPawnFrameB =
        ComposeSquareLed(cpuCaptureSm, settings, 29U, false, true,
                         &renderEngine);
    const modules::Rgb p2CpuCapturedPawnPieceFrameB =
        ComposePieceLed(cpuCaptureSm, settings, 29U, false, true,
                        &renderEngine);
    Check(RgbEq(capturedPawnFrameA, modules::RGB_WHITE) &&
              RgbEq(capturedPawnRightFrameA, modules::RGB_BLACK) &&
              RgbEq(capturedPawnFrameB, modules::RGB_BLACK) &&
              RgbEq(capturedPawnRightFrameB, modules::RGB_WHITE),
          "en-passant removal rails reach neutral full-white/off then off/full-white");
    Check(RgbEq(rotatedCapturedPawnFrameA, modules::RGB_WHITE) &&
              RgbEq(rotatedCapturedPawnRightFrameA, modules::RGB_BLACK) &&
              RgbEq(rotatedCapturedPawnFrameB, modules::RGB_BLACK) &&
              RgbEq(rotatedCapturedPawnRightFrameB, modules::RGB_WHITE),
          "rotated en-passant removal rails retain both exact white/off poles");
    for (const auto piece : {capturedPawnPieceFrameA, capturedPawnPieceFrameB,
                            cpuCapturedPawnPieceFrameA, cpuCapturedPawnPieceFrameB,
                            p2CapturedPawnPieceFrameA, p2CapturedPawnPieceFrameB,
                            p2CpuCapturedPawnPieceFrameA, p2CpuCapturedPawnPieceFrameB}) {
        Check(RgbEq(piece, modules::RGB_BLACK),
              "present en-passant victim Piece remains off for human/CPU/P1/P2");
    }
    Check(RgbEq(capturedPawnFrameA, rotatedCapturedPawnFrameA) &&
              RgbEq(capturedPawnFrameB, rotatedCapturedPawnFrameB),
          "en-passant removal cue maps to the rotated physical square");
    Check(RgbEq(capturedPawnFrameA, cpuCapturedPawnFrameA) &&
              RgbEq(capturedPawnFrameB, cpuCapturedPawnFrameB),
          "en-passant removal square exactly matches CPU capture cue");
    Check(RgbEq(capturedPawnPieceFrameA, cpuCapturedPawnPieceFrameA) &&
              RgbEq(capturedPawnPieceFrameB, cpuCapturedPawnPieceFrameB),
          "en-passant captured piece LED exactly matches CPU capture cue");
    Check(RgbEq(rotatedCapturedPawnFrameA, rotatedCpuCapturedPawnFrameA) &&
              RgbEq(rotatedCapturedPawnFrameB, rotatedCpuCapturedPawnFrameB),
          "rotated en-passant removal exactly matches CPU capture cue");
    Check(RgbEq(p2CapturedPawnFrameA, p2CpuCapturedPawnFrameA) &&
              RgbEq(p2CapturedPawnFrameB, p2CpuCapturedPawnFrameB) &&
              RgbEq(p2CapturedPawnPieceFrameA,
                    p2CpuCapturedPawnPieceFrameA) &&
              RgbEq(p2CapturedPawnPieceFrameB,
                    p2CpuCapturedPawnPieceFrameB),
          "P1/P2-swapped en-passant removal exactly matches CPU capture cue");

    const uint32_t removedAtMs = clock.NowMs();
    sm.HandleSensorChange(29, false);
    const runtime::InteractionModelSnapshot removedSnapshot =
        test_support::BuildInteractionSnapshot(
            sm, modules::EngineDerivedState{});
    Check(!runtime::IsSettlingActive(removedSnapshot),
          "captured-pawn removal ends the pending capture animation");

    // The completed placement now owns a bounded 560 ms ripple afterglow,
    // which can legitimately pass through the adjacent captured-pawn square.
    // Endpoint sampling moves removal later than the old absolute baseline.
    // Keep both quiet observations beyond its legitimate 560 ms afterglow.
    clock.SetNowMs(removedAtMs + 600U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    const modules::Rgb removedFrameA = ComposeSquareLed(sm, settings, 29U);
    clock.SetNowMs(removedAtMs + 650U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    const modules::Rgb removedFrameB = ComposeSquareLed(sm, settings, 29U);
    Check(RgbEq(removedFrameA, removedFrameB),
          "captured-pawn square stops flashing after removal");

    if (g_failures == 0) {
        std::puts("[EN_PASSANT_SETTLE_BLOCKING] ok");
        return 0;
    }

    std::printf("[EN_PASSANT_SETTLE_BLOCKING] failures=%d\n", g_failures);
    return 1;
}
