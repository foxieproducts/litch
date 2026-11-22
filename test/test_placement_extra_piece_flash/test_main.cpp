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
#include "runtime/game_state_machine.hpp"
#include "../support/led_frame_composer_snapshot_adapter.hpp"
#include "test_helpers.hpp"

using placement_led_test_helpers::AllEdgeLit;
using placement_led_test_helpers::AllEdgeRedDominant;
using placement_led_test_helpers::AnyEdgeLit;
using placement_led_test_helpers::PieceLedAt;
using placement_led_test_helpers::RgbEq;
using placement_led_test_helpers::RgbNe;
using placement_led_test_helpers::SquareLed2At;
using placement_led_test_helpers::SquareLedAt;

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[PLACEMENT_EXTRA_PIECE_FLASH] start");

    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_placement_extra_piece_flash.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    settings.Doc()["P1_PIECE_WHEEL"] = 0;
    settings.Doc()["P2_PIECE_WHEEL"] = 128;

    runtime::GameStateMachineConfig smCfg;
    smCfg.illegalFlashMs = 10;
    smCfg.placementStableRequiredMs = 0;
    smCfg.cpuIdleDelayMs = 999999;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    // Create a placement mismatch: expected empty, but physical has a piece on a8 (sq=0).
    sm.SetExpectedOccupancyMask(0);
    sm.ApplyPhysicalOccupancySnapshot(1ULL << 0);
    Check(sm.ForceEnterPlacementMode(), "force enter placement mode");
    Check(sm.IsPlacementModeActive(), "placement mode active");
    Check(sm.GetCpuIllegalMask() == 0ULL,
          "placement mismatch does not masquerade as CPU-illegal state");

    modules::LitchMaxEngine engine;
    engine.Reset();

    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);

    std::array<bool, 64> occupiedPhysical{};
    occupiedPhysical.fill(false);
    occupiedPhysical[0] = true;

    modules::led_frame_composer::ComposeInputs in;
    in.settings = &settings;
    in.firstPlayerIsP2 = false;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledOn{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeOn{};
    std::array<modules::Rgb, 64> gridOn{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledOff{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeOff{};
    std::array<modules::Rgb, 64> gridOff{};

    // Strong-stage color/cadence fixtures. The production accidental-input
    // journey independently asserts quiet/gentle timing and immediate recovery.
    fxe::ManualMillis::SetNowMs(1500);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledOn, edgeOn, gridOn);

    // Edge dark phase; board mismatch square should stay visibly animated.
    fxe::ManualMillis::SetNowMs(1510);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledOff, edgeOff, gridOff);

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledRotate{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeRotate{};
    std::array<modules::Rgb, 64> gridRotate{};

    fxe::ManualMillis::SetNowMs(1740);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledRotate, edgeRotate, gridRotate);

    Check(RgbNe(SquareLedAt(ledOn, 0), modules::RGB_BLACK), "extra: initial square LED stays visible");
    Check(RgbNe(PieceLedAt(ledOn, 0), modules::RGB_BLACK), "extra: initial piece LED stays visible");
    Check(RgbNe(SquareLed2At(ledOn, 0), modules::RGB_BLACK), "extra: second square LED stays visible");
    Check(RgbNe(SquareLedAt(ledOn, 0), PieceLedAt(ledOn, 0)), "extra: square and piece LEDs use different rainbow colors");
    Check(RgbNe(PieceLedAt(ledOn, 0), SquareLed2At(ledOn, 0)), "extra: rainbow spans all three LEDs");
    Check(RgbNe(SquareLedAt(ledOff, 0), modules::RGB_BLACK), "extra: board cue remains visible during edge dark phase");
    Check(RgbNe(PieceLedAt(ledOff, 0), modules::RGB_BLACK), "extra: piece LED remains visible during edge dark phase");
    Check(RgbNe(SquareLedAt(ledRotate, 0), SquareLedAt(ledOn, 0)), "extra: rainbow rotates over time");

    // Missing piece reuses the fault channel so rebooted mismatch recovery
    // matches live invalid feedback.
    runtime::GameStateMachine smMissing(smCfg);
    fxe::ManualMillis::SetNowMs(0);
    smMissing.Reset();

    static constexpr uint8_t MISSING_SQUARE = 1;

    // Create a placement mismatch: expected occupied, but physical is empty on b8 (sq=1).
    smMissing.SetExpectedOccupancyMask(1ULL << MISSING_SQUARE);
    smMissing.ApplyPhysicalOccupancySnapshot(0);
    Check(smMissing.ForceEnterPlacementMode(), "force enter placement mode (missing)");
    Check(smMissing.IsPlacementModeActive(), "placement mode active (missing)");

    std::array<bool, 64> occupiedPhysicalMissing{};
    occupiedPhysicalMissing.fill(false);

    modules::led_frame_composer::ComposeInputs inMissing = in;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledMissingOn{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeMissingOn{};
    std::array<modules::Rgb, 64> gridMissingOn{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledMissingOff{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeMissingOff{};
    std::array<modules::Rgb, 64> gridMissingOff{};

    fxe::ManualMillis::SetNowMs(1500);
    test_support::ComposeWithSnapshot(
        inMissing, smMissing, derived, &engine, ledMissingOn, edgeMissingOn, gridMissingOn);

    fxe::ManualMillis::SetNowMs(1510);
    test_support::ComposeWithSnapshot(
        inMissing, smMissing, derived, &engine, ledMissingOff, edgeMissingOff, gridMissingOff);

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledMissingRotate{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeMissingRotate{};
    std::array<modules::Rgb, 64> gridMissingRotate{};

    fxe::ManualMillis::SetNowMs(1740);
    test_support::ComposeWithSnapshot(inMissing,
                                      smMissing,
                                      derived,
                                      &engine,
                                      ledMissingRotate,
                                      edgeMissingRotate,
                                      gridMissingRotate);

    Check(RgbNe(SquareLedAt(ledMissingOn, MISSING_SQUARE), modules::RGB_BLACK),
          "missing: initial square LED stays visible");
    Check(!RgbNe(PieceLedAt(ledMissingOn, MISSING_SQUARE), modules::RGB_BLACK),
          "missing: initial empty piece LED stays dark");
    Check(RgbNe(SquareLed2At(ledMissingOn, MISSING_SQUARE), modules::RGB_BLACK),
          "missing: second square LED stays visible");
    Check(RgbNe(SquareLedAt(ledMissingOn, MISSING_SQUARE), SquareLed2At(ledMissingOn, MISSING_SQUARE)),
          "missing: square LEDs use opposite wheel colors");
    Check(RgbNe(SquareLedAt(ledMissingOff, MISSING_SQUARE), modules::RGB_BLACK),
          "missing: board cue remains visible during edge dark phase");
    Check(!RgbNe(PieceLedAt(ledMissingOff, MISSING_SQUARE), modules::RGB_BLACK),
          "missing: empty piece LED remains dark during edge dark phase");
    Check(RgbNe(SquareLedAt(ledMissingRotate, MISSING_SQUARE), SquareLedAt(ledMissingOn, MISSING_SQUARE)),
          "missing: rainbow rotates over time");
    Check(AnyEdgeLit(edgeMissingOn), "missing: edge error pulse is active on-phase");
    Check(!AnyEdgeLit(edgeMissingOff), "missing: edge error pulse reaches dark phase");

    fxe::ManualMillis::SetNowMs(300U);
    runtime::GameStateMachine smIdleMissing(smCfg);
    smIdleMissing.Reset();
    smIdleMissing.SetExpectedOccupancyMask(1ULL << MISSING_SQUARE);
    smIdleMissing.SetExpectedMoverAndOpponentOccupancyMasks(1ULL << MISSING_SQUARE, 0ULL);
    smIdleMissing.DebugSetPhysicalOccupancyMask(0ULL);

    std::array<bool, 64> occupiedPhysicalIdleMissing{};

    modules::led_frame_composer::ComposeInputs inIdleMissing = in;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledIdleMissingOn{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeIdleMissingOn{};
    std::array<modules::Rgb, 64> gridIdleMissingOn{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledIdleMissingOff{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeIdleMissingOff{};
    std::array<modules::Rgb, 64> gridIdleMissingOff{};

    modules::led_frame_composer::internal::ComposeContext idleMissingCtx;
    runtime::InteractionModelSnapshot contextSnapshot0;
    Check(test_support::BuildComposeContextWithSnapshot(
              inIdleMissing, smIdleMissing, derived, &engine, idleMissingCtx, contextSnapshot0),
          "idle missing: build compose context");
    Check(modules::led_frame_composer::internal::ComputeInvalidBoardState(idleMissingCtx).IsActive(),
          "idle missing: shared invalid-state helper activates immediately");

    fxe::ManualMillis::SetNowMs(1800U);
    test_support::ComposeWithSnapshot(inIdleMissing,
                                      smIdleMissing,
                                      derived,
                                      &engine,
                                      ledIdleMissingOn,
                                      edgeIdleMissingOn,
                                      gridIdleMissingOn);

    fxe::ManualMillis::SetNowMs(1810U);
    test_support::ComposeWithSnapshot(inIdleMissing,
                                      smIdleMissing,
                                      derived,
                                      &engine,
                                      ledIdleMissingOff,
                                      edgeIdleMissingOff,
                                      gridIdleMissingOff);

    Check(RgbNe(SquareLedAt(ledIdleMissingOn, MISSING_SQUARE), modules::RGB_BLACK),
          "idle missing: strong square rainbow starts on-phase");
    Check(!RgbNe(PieceLedAt(ledIdleMissingOn, MISSING_SQUARE), modules::RGB_BLACK),
          "idle missing: empty center stays dark immediately");
    Check(RgbNe(SquareLedAt(ledIdleMissingOn, MISSING_SQUARE),
                SquareLed2At(ledIdleMissingOn, MISSING_SQUARE)),
          "idle missing: square pair uses the shared rainbow cue");
    Check(AllEdgeLit(edgeIdleMissingOn), "idle missing: edge error pulse starts on the same frame");
    Check(AllEdgeRedDominant(edgeIdleMissingOn), "idle missing: edge error pulse uses the red fault state");
    Check(!AnyEdgeLit(edgeIdleMissingOff), "idle missing: edge error pulse reaches the dark phase");

    fxe::ManualMillis::SetNowMs(400U);
    runtime::GameStateMachine smIdleExtra(smCfg);
    smIdleExtra.Reset();
    static constexpr uint8_t MATCHED_SQUARE = 9;
    static constexpr uint8_t EXTRA_SQUARE = 0;
    smIdleExtra.SetExpectedOccupancyMask(1ULL << MATCHED_SQUARE);
    smIdleExtra.SetExpectedMoverAndOpponentOccupancyMasks(1ULL << MATCHED_SQUARE, 0ULL);
    smIdleExtra.DebugSetPhysicalOccupancyMask((1ULL << MATCHED_SQUARE) | (1ULL << EXTRA_SQUARE));

    std::array<bool, 64> occupiedPhysicalIdleExtra{};
    occupiedPhysicalIdleExtra[MATCHED_SQUARE] = true;
    occupiedPhysicalIdleExtra[EXTRA_SQUARE] = true;

    modules::led_frame_composer::ComposeInputs inIdleExtra = in;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledIdleExtraOn{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeIdleExtraOn{};
    std::array<modules::Rgb, 64> gridIdleExtraOn{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledIdleExtraOff{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeIdleExtraOff{};
    std::array<modules::Rgb, 64> gridIdleExtraOff{};

    modules::led_frame_composer::internal::ComposeContext idleExtraCtx;
    runtime::InteractionModelSnapshot contextSnapshot1;
    Check(test_support::BuildComposeContextWithSnapshot(
              inIdleExtra, smIdleExtra, derived, &engine, idleExtraCtx, contextSnapshot1),
          "idle extra: build compose context");
    Check(modules::led_frame_composer::internal::ComputeInvalidBoardState(idleExtraCtx).IsActive(),
          "idle extra: shared invalid-state helper activates immediately");

    fxe::ManualMillis::SetNowMs(1900U);
    test_support::ComposeWithSnapshot(inIdleExtra,
                                      smIdleExtra,
                                      derived,
                                      &engine,
                                      ledIdleExtraOn,
                                      edgeIdleExtraOn,
                                      gridIdleExtraOn);

    fxe::ManualMillis::SetNowMs(1910U);
    test_support::ComposeWithSnapshot(inIdleExtra,
                                      smIdleExtra,
                                      derived,
                                      &engine,
                                      ledIdleExtraOff,
                                      edgeIdleExtraOff,
                                      gridIdleExtraOff);

    Check(RgbNe(SquareLedAt(ledIdleExtraOn, EXTRA_SQUARE), modules::RGB_BLACK),
          "idle extra: strong square rainbow starts on-phase");
    Check(RgbNe(PieceLedAt(ledIdleExtraOn, EXTRA_SQUARE), modules::RGB_BLACK),
          "idle extra: strong piece rainbow starts on-phase");
    Check(RgbNe(SquareLedAt(ledIdleExtraOn, EXTRA_SQUARE), PieceLedAt(ledIdleExtraOn, EXTRA_SQUARE)),
          "idle extra: square and piece slots use the shared rainbow cue");
    Check(AllEdgeLit(edgeIdleExtraOn), "idle extra: edge error pulse starts on the same frame");
    Check(AllEdgeRedDominant(edgeIdleExtraOn), "idle extra: edge error pulse uses the red fault state");
    Check(!AnyEdgeLit(edgeIdleExtraOff), "idle extra: edge error pulse reaches the dark phase");

    runtime::GameStateMachineConfig delayedCfg = smCfg;
    delayedCfg.placementStableRequiredMs = 1200U;

    runtime::GameStateMachine smDelayed(delayedCfg);
    smDelayed.Reset();
    smDelayed.SetExpectedOccupancyMask(1ULL << MISSING_SQUARE);
    smDelayed.SetExpectedMoverAndOpponentOccupancyMasks(1ULL << MISSING_SQUARE, 0ULL);
    smDelayed.DebugSetPhysicalOccupancyMask(1ULL << MISSING_SQUARE);

    std::array<bool, 64> occupiedPhysicalDelayed{};
    occupiedPhysicalDelayed.fill(false);
    occupiedPhysicalDelayed[MISSING_SQUARE] = true;

    fxe::ManualMillis::SetNowMs(100U);
    smDelayed.HandleSensorChange(MISSING_SQUARE, false);
    occupiedPhysicalDelayed[MISSING_SQUARE] = false;
    Check(!smDelayed.IsPlacementModeActive(), "delayed: placement mode still gated off immediately after lift");

    runtime::GameStateMachine smMismatchPickup(delayedCfg);
    smMismatchPickup.Reset();
    static constexpr uint8_t PREEXISTING_MISSING_SQUARE = 6;
    static constexpr uint8_t PICKUP_FROM_MISMATCHED_BOARD = 9;
    const uint64_t mismatchedExpectedOcc64 = (1ULL << PREEXISTING_MISSING_SQUARE) | (1ULL << PICKUP_FROM_MISMATCHED_BOARD);
    const uint64_t mismatchedPhysicalOcc64 = (1ULL << PICKUP_FROM_MISMATCHED_BOARD);
    smMismatchPickup.SetExpectedOccupancyMask(mismatchedExpectedOcc64);
    smMismatchPickup.SetExpectedMoverAndOpponentOccupancyMasks((1ULL << PICKUP_FROM_MISMATCHED_BOARD), 0ULL);
    smMismatchPickup.DebugSetPhysicalOccupancyMask(mismatchedPhysicalOcc64);

    std::array<bool, 64> occupiedPhysicalMismatchPickup{};
    occupiedPhysicalMismatchPickup.fill(false);
    occupiedPhysicalMismatchPickup[PICKUP_FROM_MISMATCHED_BOARD] = true;

    modules::led_frame_composer::ComposeInputs inMismatchPickup = in;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledMismatchPickupStart{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeMismatchPickupStart{};
    std::array<modules::Rgb, 64> gridMismatchPickupStart{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledMismatchPickupRotate{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeMismatchPickupRotate{};
    std::array<modules::Rgb, 64> gridMismatchPickupRotate{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledMismatchPickupOff{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeMismatchPickupOff{};
    std::array<modules::Rgb, 64> gridMismatchPickupOff{};

    fxe::ManualMillis::SetNowMs(500U);
    smMismatchPickup.HandleSensorChange(PICKUP_FROM_MISMATCHED_BOARD, false);
    occupiedPhysicalMismatchPickup[PICKUP_FROM_MISMATCHED_BOARD] = false;
    smMismatchPickup.SetValidDestMask(1ULL << 17);

    fxe::ManualMillis::SetNowMs(2000U);
    test_support::ComposeWithSnapshot(inMismatchPickup,
                                      smMismatchPickup,
                                      derived,
                                      &engine,
                                      ledMismatchPickupStart,
                                      edgeMismatchPickupStart,
                                      gridMismatchPickupStart);

    fxe::ManualMillis::SetNowMs(2010U);
    test_support::ComposeWithSnapshot(inMismatchPickup,
                                      smMismatchPickup,
                                      derived,
                                      &engine,
                                      ledMismatchPickupOff,
                                      edgeMismatchPickupOff,
                                      gridMismatchPickupOff);

    fxe::ManualMillis::SetNowMs(2240U);
    test_support::ComposeWithSnapshot(inMismatchPickup,
                                      smMismatchPickup,
                                      derived,
                                      &engine,
                                      ledMismatchPickupRotate,
                                      edgeMismatchPickupRotate,
                                      gridMismatchPickupRotate);

    Check(RgbNe(SquareLedAt(ledMismatchPickupStart, PICKUP_FROM_MISMATCHED_BOARD), modules::RGB_BLACK),
          "mismatch pickup: picked-up square stays visibly mismatched before placement mode");
    Check(RgbNe(SquareLedAt(ledMismatchPickupStart, PICKUP_FROM_MISMATCHED_BOARD),
                SquareLed2At(ledMismatchPickupStart, PICKUP_FROM_MISMATCHED_BOARD)),
          "mismatch pickup: square LEDs stay opposite on the wheel even after legal destinations appear");
    Check(RgbNe(SquareLedAt(ledMismatchPickupRotate, PICKUP_FROM_MISMATCHED_BOARD),
                SquareLedAt(ledMismatchPickupStart, PICKUP_FROM_MISMATCHED_BOARD)),
          "mismatch pickup: swirl stays continuous instead of falling back to pickup flash");
    Check(AllEdgeLit(edgeMismatchPickupStart),
          "mismatch pickup: edge error pulse starts with the first rainbow frame");
    Check(AllEdgeRedDominant(edgeMismatchPickupStart),
          "mismatch pickup: edge state uses the red fault pulse rather than placement guidance");
    Check(!AnyEdgeLit(edgeMismatchPickupOff),
          "mismatch pickup: edge fault pulse reaches the dark phase without waiting for placement mode");

    runtime::GameStateMachine smLate(smCfg);
    smLate.Reset();
    smLate.SetExpectedOccupancyMask(0);
    smLate.ApplyPhysicalOccupancySnapshot(1ULL << 0);
    fxe::ManualMillis::SetNowMs(10);
    Check(smLate.ForceEnterPlacementMode(), "force enter placement mode at prior off-phase");

    modules::led_frame_composer::ComposeInputs inLate = in;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledLate{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeLate{};
    std::array<modules::Rgb, 64> gridLate{};
    fxe::ManualMillis::SetNowMs(1510U);
    test_support::ComposeWithSnapshot(
        inLate, smLate, derived, &engine, ledLate, edgeLate, gridLate);

    Check(RgbNe(SquareLedAt(ledLate, 0), modules::RGB_BLACK),
          "persistent extra starts strong animation on-phase despite prior edge dark phase");
    Check(RgbNe(PieceLedAt(ledLate, 0), modules::RGB_BLACK),
          "persistent extra piece LED starts strong animation on-phase");

    runtime::GameStateMachine smPinned(smCfg);
    smPinned.Reset();

    modules::LitchMaxEngine pinnedEngine;
    pinnedEngine.Reset();
    Check(pinnedEngine.SetFen("4r3/8/8/8/8/8/4N3/4K3 w - - 0 1"), "SetFen pinned knight self-check");

    modules::EngineDerivedState pinnedDerived;
    pinnedDerived.RefreshFromEngine(pinnedEngine);

    static constexpr uint8_t PINNED_FROM_E2 = 52;
    static constexpr uint8_t KING_E1 = 60;
    static constexpr uint8_t ATTACKER_E8 = 4;
    const uint64_t pinnedExpectedOcc64 = (1ULL << ATTACKER_E8) | (1ULL << PINNED_FROM_E2) | (1ULL << KING_E1);
    const uint64_t pinnedPhysicalOcc64 = (1ULL << ATTACKER_E8) | (1ULL << KING_E1);
    smPinned.SetExpectedOccupancyMask(pinnedExpectedOcc64);
    smPinned.SetExpectedMoverAndOpponentOccupancyMasks((1ULL << PINNED_FROM_E2) | (1ULL << KING_E1),
                                                       (1ULL << ATTACKER_E8));
    smPinned.DebugSetPhysicalOccupancyMask(pinnedPhysicalOcc64);
    Check(smPinned.RejectCurrentPickupAsIllegal(PINNED_FROM_E2), "reject pinned knight pickup as illegal");

    std::array<bool, 64> occupiedPhysicalPinned{};
    occupiedPhysicalPinned.fill(false);
    occupiedPhysicalPinned[ATTACKER_E8] = true;
    occupiedPhysicalPinned[KING_E1] = true;

    modules::led_frame_composer::ComposeInputs inPinned = in;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledPinnedBaseline{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgePinnedBaseline{};
    std::array<modules::Rgb, 64> gridPinnedBaseline{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledPinnedPrompt{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgePinnedPrompt{};
    std::array<modules::Rgb, 64> gridPinnedPrompt{};

    fxe::ManualMillis::SetNowMs(0);
    test_support::ComposeWithSnapshot(inPinned,
                                      smPinned,
                                      pinnedDerived,
                                      &pinnedEngine,
                                      ledPinnedBaseline,
                                      edgePinnedBaseline,
                                      gridPinnedBaseline);

    smPinned.SetKingSafetyPrompt(PINNED_FROM_E2, KING_E1, ATTACKER_E8);
    test_support::ComposeWithSnapshot(inPinned,
                                      smPinned,
                                      pinnedDerived,
                                      &pinnedEngine,
                                      ledPinnedPrompt,
                                      edgePinnedPrompt,
                                      gridPinnedPrompt);

    Check(!RgbEq(SquareLedAt(ledPinnedPrompt, KING_E1), SquareLedAt(ledPinnedBaseline, KING_E1)),
          "self-check prompt: king square gains additional danger cue");
    Check(!RgbEq(SquareLedAt(ledPinnedPrompt, ATTACKER_E8), SquareLedAt(ledPinnedBaseline, ATTACKER_E8)),
          "self-check prompt: attacker square gains additional danger cue");

    if (g_failures == 0) {
        std::puts("[PLACEMENT_EXTRA_PIECE_FLASH] ok");
        return 0;
    }

    std::printf("[PLACEMENT_EXTRA_PIECE_FLASH] failures=%d\n", g_failures);
    return 1;
}
