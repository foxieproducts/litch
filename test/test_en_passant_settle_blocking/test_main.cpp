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

struct SquareLeds {
    modules::Rgb left;
    modules::Rgb piece;
    modules::Rgb right;
};

SquareLeds ComposeSquareLeds(const runtime::GameStateMachine& stateMachine,
                             const modules::SettingsStore& settings,
                             const uint8_t logicalSquare,
                             const bool flipSides = false,
                             const bool firstPlayerIsP2 = false,
                             const modules::IEngine* engine = nullptr) {
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
    const auto pixel = [&](const hardware::SquareLedSlot slot) {
        const auto target = hardware::MapSemanticSquareLedTargets(
            hardware::BoardRevision::V2S3, logicalSquare, slot, flipSides);
        return physicalFrame[target.indices[0U]];
    };
    return {pixel(hardware::SquareLedSlot::Square0), pixel(hardware::SquareLedSlot::Piece),
            pixel(hardware::SquareLedSlot::Square2)};
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
    const auto sourceBefore = ComposeSquareLeds(sm, settings, 28U, false, false, &renderEngine);
    const auto alternativeBefore = ComposeSquareLeds(sm, settings, 20U, false, false, &renderEngine);
    const auto p2SourceBefore = ComposeSquareLeds(sm, settings, 28U, false, true, &renderEngine);
    sm.HandleSensorChange(28, false);
    sm.SetValidDestMask((1ULL << 21) | (1ULL << 20));
    sm.HandleSensorChange(21, true);
    const uint32_t promptStart = clock.NowMs();
    constexpr modules::Rgb STRIKE{255U, 101U, 0U}, REST{64U, 25U, 0U};
    const auto firstHuman = ComposeSquareLeds(sm, settings, 29U, false, false, &renderEngine);
    const auto firstCpu = ComposeSquareLeds(cpuCaptureSm, settings, 29U, false, false, &renderEngine);
    Check(RgbEq(firstHuman.left, STRIKE) && RgbEq(firstHuman.right, STRIKE) &&
              RgbEq(firstCpu.left, STRIKE) && RgbEq(firstCpu.right, STRIKE),
          "newly required human and CPU removal cues both begin with an amber strike");

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
    // The default 560 ms placement ripple can briefly cross these squares.
    // Check retired move highlights after that separate effect has finished.
    clock.SetNowMs(1600U);
    fxe::ManualMillis::SetNowMs(clock.NowMs());
    sm.Tick();
    const auto sourcePending = ComposeSquareLeds(sm, settings, 28U, false, false, &renderEngine);
    const auto alternativePending = ComposeSquareLeds(sm, settings, 20U, false, false, &renderEngine);
    Check(RgbEq(sourcePending.left, sourceBefore.left) && RgbEq(sourcePending.right, sourceBefore.right),
          "en-passant removal phase clears the original-source highlight");
    Check(RgbEq(alternativePending.left, alternativeBefore.left) &&
              RgbEq(alternativePending.right, alternativeBefore.right),
          "en-passant removal phase clears alternative legal-move highlights");
    Check(RgbEq(ComposeSquareLeds(sm, settings, 21U, false, false, &renderEngine).piece, sourceBefore.piece),
          "en-passant destination piece uses the mover's color");
    Check(RgbEq(ComposeSquareLeds(sm, settings, 21U, true, false, &renderEngine).piece, sourceBefore.piece),
          "rotated en-passant destination piece uses the mover's color");
    Check(RgbEq(ComposeSquareLeds(sm, settings, 21U, false, true, &renderEngine).piece, p2SourceBefore.piece),
          "P2 mover's en-passant destination uses the P2 color");

    constexpr uint32_t PERIOD_MS = 1200U;
    struct Sample { uint32_t offset; modules::Rgb expected; };
    constexpr Sample SAMPLES[] = {
        {0U, STRIKE}, {119U, STRIKE}, {120U, REST}, {239U, REST},
        {240U, STRIKE}, {359U, STRIKE}, {360U, REST}, {1199U, REST},
        {1200U, STRIKE},
    };
    const uint32_t start = promptStart + PERIOD_MS;
    for (const auto& sample : SAMPLES) {
        clock.SetNowMs(start + sample.offset);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        for (const auto* state : {&sm, &cpuCaptureSm}) {
            const auto expectVictim = [&](const bool rotated, const bool p2) {
                const auto leds = ComposeSquareLeds(*state, settings, 29U, rotated, p2, &renderEngine);
                if (!RgbEq(leds.left, sample.expected) || !RgbEq(leds.right, sample.expected) ||
                    !RgbEq(leds.piece, modules::RGB_BLACK)) {
                    std::printf("[FAIL] f5 removal +%ums cpu=%u rotated=%u p2=%u "
                                "expected=%u/%u/%u left=%u/%u/%u right=%u/%u/%u piece=%u/%u/%u\n",
                                sample.offset, state == &cpuCaptureSm, rotated, p2,
                                sample.expected.r, sample.expected.g, sample.expected.b,
                                leds.left.r, leds.left.g, leds.left.b,
                                leds.right.r, leds.right.g, leds.right.b,
                                leds.piece.r, leds.piece.g, leds.piece.b);
                    ++g_failures;
                }
            };
            expectVictim(false, false);
            expectVictim(true, false);
            expectVictim(false, true);
        }
    }

    const uint32_t removedAtMs = clock.NowMs();
    sm.HandleSensorChange(29, false);
    const runtime::InteractionModelSnapshot removedSnapshot =
        test_support::BuildInteractionSnapshot(
            sm, modules::EngineDerivedState{});
    Check(!runtime::IsSettlingActive(removedSnapshot),
          "captured-pawn removal ends the pending capture animation");

    // The completed placement now owns a bounded 560 ms ripple afterglow,
    // which can legitimately pass through the adjacent captured-pawn square.
    for (const uint32_t delay : {600U, 650U}) {
        clock.SetNowMs(removedAtMs + delay);
        fxe::ManualMillis::SetNowMs(clock.NowMs());
        const auto cleared = ComposeSquareLeds(sm, settings, 29U);
        Check(RgbEq(cleared.left, alternativeBefore.left) && RgbEq(cleared.right, alternativeBefore.right) &&
                  RgbEq(cleared.piece, modules::RGB_BLACK),
              "after the placement ripple the empty f5 returns to ordinary board lighting");
    }

    if (g_failures == 0) {
        std::puts("[EN_PASSANT_SETTLE_BLOCKING] ok");
        return 0;
    }

    std::printf("[EN_PASSANT_SETTLE_BLOCKING] failures=%d\n", g_failures);
    return 1;
}
