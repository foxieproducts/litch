#include <array>
#include <cstdint>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "modules/settings_store.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"
#include "runtime/interaction_model_presentation.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

bool RgbEq(const modules::Rgb lhs, const modules::Rgb rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

void TestPublishedCaptureHintOnLastMove() {
    constexpr uint8_t VICTIM = 27U; // d5: CPU last move and White's capture target.
    constexpr uint64_t VICTIM_BIT = UINT64_C(1) << VICTIM;
    constexpr uint64_t OCCUPIED = (UINT64_C(1) << 4U) | VICTIM_BIT |
        (UINT64_C(1) << 36U) | (UINT64_C(1) << 60U);
    fxe::ManualMillis::SetNowMs(1000U);
    runtime::GameStateMachine sm;
    sm.Reset();
    sm.HandleInteractionPositionAcknowledged(OCCUPIED,
        (UINT64_C(1) << 4U) | (UINT64_C(1) << 60U), 0ULL,
        (UINT64_C(1) << 36U) | (UINT64_C(1) << 60U),
        (UINT64_C(1) << 4U) | VICTIM_BIT, VICTIM_BIT | (UINT64_C(1) << 36U));
    sm.ApplyPhysicalOccupancySnapshot(OCCUPIED);
    sm.SetLastMoveForSide(modules::PersistedSide::Black, 11U, VICTIM);
    sm.SetLastMoveMostRecentSide(modules::PersistedSide::Black);
    sm.SetHintDisplayRequested(true);
    sm.SetHintMoveSquares(36U, VICTIM);
    modules::EngineDerivedState derived;
    derived.sideToMove = modules::SideToMove::White;
    const auto snapshot = runtime::BuildInteractionModelSnapshot({
        .stateMachine = &sm, .derived = &derived,
        .authoritativeFen = "4k3/8/8/3p4/4P3/8/8/4K3 w - d6 0 2",
    });
    Check(snapshot.render.hintCaptureSquare == VICTIM &&
              snapshot.render.blackLastMoveValid && snapshot.render.blackLastMoveToSquare == VICTIM,
          "captured publication carries both the last move and the next capture victim");
    const auto published = runtime::CaptureInteractionModelPresentation(sm, snapshot);
    modules::SettingsStoreConfig config;
    config.path = "build/test_published_last_move_capture_hint.json";
    modules::SettingsStore settings(config);
    (void)settings.Load();
    struct Sample { uint32_t age; bool bright; };
    constexpr Sample samples[] = {{0U,true},{119U,true},{120U,false},{239U,false},
        {240U,true},{359U,true},{360U,false},{1199U,false},{1200U,true}};
    for (const auto sample : samples) {
        // Deliberately leave the game publication untouched: this is how the
        // independent hardware animation task samples a captured snapshot.
        const auto current = runtime::SampleInteractionModelPresentation(published, 1000U + sample.age);
        modules::led_frame_composer::ComposeInputs inputs;
        inputs.settings = &settings;
        inputs.interactionSnapshot = &current;
        modules::BoardLedFrame board{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> edge{};
        std::array<modules::Rgb, 64U> grid{};
        uint64_t removal = 0ULL, taps = 0ULL;
        modules::led_frame_composer::Compose(inputs, board, edge, grid,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &removal, nullptr, nullptr, &taps);
        const auto& square = board[VICTIM];
        const modules::Rgb expected = sample.bright ? modules::Rgb{255U,101U,0U}
                                                    : modules::Rgb{64U,25U,0U};
        Check(removal == VICTIM_BIT && (taps == VICTIM_BIT) == sample.bright,
              "a retained publication keeps removal ownership and advances tap ownership");
        Check(RgbEq(square.left, expected) && RgbEq(square.right, expected) &&
                  RgbEq(square.top, {}) && RgbEq(square.bottom, {}),
              "last-move overlap cannot replace the synchronized capture rails or dark victim pair");
        for (const bool rotate : {false, true}) {
            const auto mapped = hardware::MapSemanticBoardFrame(
                {hardware::BoardRevision::V3_3S3, rotate, false}, board);
            const uint16_t base = runtime::MapSquareIndex(VICTIM, rotate) * 4U;
            Check(RgbEq(mapped[base], expected) && RgbEq(mapped[base + 2U], expected) &&
                      RgbEq(mapped[base + 1U], {}) && RgbEq(mapped[base + 3U], {}),
                  "V3.3 mapping preserves capture ownership of all four victim emitters after rotation");
        }
    }
}

void TestHintRemovalClockAcrossRefinements() {
    constexpr uint64_t kings = (1ULL << 4U) | (1ULL << 60U);
    constexpr uint64_t white = (1ULL << 27U) | (1ULL << 29U) | (1ULL << 60U);
    constexpr uint64_t black = (1ULL << 4U) | (1ULL << 18U) | (1ULL << 20U);
    constexpr uint64_t occupancy = white | black;
    fxe::ManualMillis::SetNowMs(1000U);
    runtime::GameStateMachine sm; sm.Reset();
    sm.HandleInteractionPositionAcknowledged(occupancy, kings, 0ULL, white, black,
        (1ULL << 27U) | (1ULL << 29U) | (1ULL << 18U) | (1ULL << 20U));
    sm.ApplyPhysicalOccupancySnapshot(occupancy);
    sm.SetHintDisplayRequested(true); sm.SetHintMoveSquares(27U, 20U);
    modules::EngineDerivedState derived; derived.sideToMove = modules::SideToMove::White;
    runtime::InteractionModelSnapshotBuildInputs inputs;
    inputs.stateMachine = &sm; inputs.derived = &derived;
    inputs.authoritativeFen = "4k3/8/2p1p3/3P1P2/8/8/8/4K3 w - - 0 1";
    const auto Capture = [&]() { return runtime::CaptureInteractionModelPresentation(
        sm, runtime::BuildInteractionModelSnapshot(inputs)); };
    auto published = Capture();
    Check(published.snapshot.render.hintRemovalSquare == 20U &&
              published.snapshot.render.hintRemovalElapsedMs == 0U,
          "first actual victim starts its own fresh clock");
    fxe::ManualMillis::SetNowMs(1450U); sm.ReplayHintMove(); published = Capture();
    Check(published.snapshot.render.hintRemovalSquare == 20U &&
              published.snapshot.render.hintRemovalElapsedMs == 450U,
          "replaying the same victim retains its physical clock");
    fxe::ManualMillis::SetNowMs(1600U); sm.SetHintMoveSquares(29U, 20U); published = Capture();
    Check(published.snapshot.render.hintRemovalSquare == 20U &&
              published.snapshot.render.hintRemovalElapsedMs == 600U,
          "a different attacker of the same victim retains the clock");
    modules::SettingsStoreConfig config; config.path = "build/test_hint_removal_clock.json";
    modules::SettingsStore settings(config); (void)settings.Load();
    const auto Render = [&](const uint32_t now, const uint8_t square, const bool expectTap) {
        const auto sampled = runtime::SampleInteractionModelPresentation(published, now);
        modules::led_frame_composer::ComposeInputs compose;
        compose.settings = &settings; compose.interactionSnapshot = &sampled;
        modules::BoardLedFrame board{}; std::array<modules::Rgb, modules::EDGE_LED_COUNT> edge{};
        std::array<modules::Rgb, 64> grid{}; uint64_t removal=0ULL, taps=0ULL;
        modules::led_frame_composer::Compose(compose,board,edge,grid,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
            &removal,nullptr,nullptr,&taps);
        Check((removal & (1ULL << square)) != 0ULL && ((taps & (1ULL << square)) != 0ULL) == expectTap,
              "displayed victim keeps its own clock through the independent snapshot sampler");
        Check(RgbEq(board[square].top,{}) && RgbEq(board[square].bottom,{}),
              "displayed victim piece emitters stay dark");
    };
    Render(1720U,20U,false); // Same-victim midpoint remains visible at its existing rest phase.
    fxe::ManualMillis::SetNowMs(1900U); sm.SetHintMoveSquares(27U,18U); published=Capture();
    Check(published.snapshot.render.hintRemovalSquare==18U &&
              published.snapshot.render.previousHintRemovalSquare==20U,
          "different victim gets a new clock while outgoing victim retains its own");
    Render(1960U,20U,false);
    Render(2030U,18U,true);
    fxe::ManualMillis::SetNowMs(1960U); sm.ReplayHintMove(); published=Capture();
    Check(published.snapshot.render.hintRemovalSquare==20U &&
              published.snapshot.render.hintRemovalElapsedMs==960U,
          "rewinding an outgoing refinement retains the victim already shown");
    sm.SetHintDisplayRequested(false); sm.SetHintDisplayRequested(true); published=Capture();
    Check(published.snapshot.render.hintRemovalSquare==20U &&
              published.snapshot.render.hintRemovalElapsedMs==0U,
          "a genuinely new claim for the same victim restarts its cue");

    // Three candidate captures can coalesce before the first handoff. Keep
    // the outgoing A clock rather than the never-displayed intermediate B.
    const uint64_t threeBlack=black | (1ULL<<22U);
    const auto SeedThree = [&](uint32_t now) {
        fxe::ManualMillis::SetNowMs(now); sm.Reset();
        sm.HandleInteractionPositionAcknowledged(white|threeBlack,kings,0ULL,white,threeBlack,
            (1ULL<<27U)|(1ULL<<29U)|(1ULL<<18U)|(1ULL<<20U)|(1ULL<<22U));
        sm.ApplyPhysicalOccupancySnapshot(white|threeBlack);
        inputs.authoritativeFen="4k3/8/2p1p1p1/3P1P2/8/8/8/4K3 w - - 0 1";
        sm.SetHintDisplayRequested(true); sm.SetHintMoveSquares(27U,20U);
    };
    SeedThree(5000U);
    fxe::ManualMillis::SetNowMs(5600U); sm.SetHintMoveSquares(27U,18U);
    fxe::ManualMillis::SetNowMs(5700U); sm.SetHintMoveSquares(29U,22U); published=Capture();
    Render(5701U,20U,false);
    Render(5721U,22U,true);
    SeedThree(8000U);
    fxe::ManualMillis::SetNowMs(8600U); sm.SetHintMoveSquares(27U,18U);
    fxe::ManualMillis::SetNowMs(8750U); sm.SetHintMoveSquares(29U,22U); published=Capture();
    Render(8751U,18U,true);
    fxe::ManualMillis::SetNowMs(8751U); sm.ReplayHintMove(); published=Capture();
    Check(published.snapshot.render.hintRemovalSquare==18U &&
              published.snapshot.render.hintRemovalElapsedMs==151U &&
              published.snapshot.render.hintRemovalDelayMs==120U,
          "replay during the second half retains the displayed B clock");

    fxe::ManualMillis::SetNowMs(9000U); sm.Reset();
    const uint64_t epWhite=(1ULL<<28U)|(1ULL<<60U), epBlack=(1ULL<<27U)|(1ULL<<4U);
    sm.HandleInteractionPositionAcknowledged(epWhite|epBlack,kings,0ULL,epWhite,epBlack,
        (1ULL<<28U)|(1ULL<<27U),19U,27U);
    sm.ApplyPhysicalOccupancySnapshot(epWhite|epBlack);
    inputs.authoritativeFen = "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1";
    sm.SetHintDisplayRequested(true); sm.SetHintMoveSquares(28U,19U); published=Capture();
    Check(published.snapshot.render.hintRemovalSquare==27U &&
              published.snapshot.render.hintCaptureSquare==27U &&
              published.snapshot.render.hintMoveToSquare==19U,
          "en passant keys the cue to the physical victim, not the landing square");
}

void TestAllPiecesFlashUptimeWrap() {
    runtime::GameStateMachine sm;
    modules::EngineDerivedState derived;
    const runtime::InteractionModelSnapshotBuildInputs inputs{
        .stateMachine = &sm,
        .derived = &derived,
    };
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 249U);
    sm.Reset();
    auto published = runtime::CaptureInteractionModelPresentation(sm,
        runtime::BuildInteractionModelSnapshot(inputs));
    auto CheckFlash = [&](const uint32_t nowMs, const bool active, const bool on,
                          const char* checkpoint) {
        fxe::ManualMillis::SetNowMs(nowMs);
        const auto direct = runtime::BuildInteractionModelSnapshot(inputs);
        const auto sampled = runtime::SampleInteractionModelPresentation(published, nowMs);
        if (direct.render.allPiecesFlashActive != active ||
            direct.render.allPiecesFlashOn != on ||
            sampled.render.allPiecesFlashActive != active ||
            sampled.render.allPiecesFlashOn != on) {
            std::printf("[FLASH_WRAP] %s at %u: direct=%u/%u sampled=%u/%u expected=%u/%u\n",
                checkpoint, nowMs, direct.render.allPiecesFlashActive,
                direct.render.allPiecesFlashOn, sampled.render.allPiecesFlashActive,
                sampled.render.allPiecesFlashOn, active, on);
        }
        Check(direct.render.allPiecesFlashActive == active &&
                  direct.render.allPiecesFlashOn == on,
              "direct snapshot preserves flash lifetime and cadence across uptime wrap");
        Check(sampled.render.allPiecesFlashActive == active &&
                  sampled.render.allPiecesFlashOn == on,
              "independently sampled snapshot preserves flash lifetime and cadence across uptime wrap");
    };
    auto Publish = [&]() {
        published = runtime::CaptureInteractionModelPresentation(sm,
            runtime::BuildInteractionModelSnapshot(inputs));
    };

    CheckFlash(UINT32_MAX - 249U, false, false, "never triggered");
    sm.TriggerAllPiecesFlash(500U);
    Publish();
    CheckFlash(UINT32_MAX - 249U, true, false, "before wrap, off phase");
    CheckFlash(UINT32_MAX - 95U, true, true, "before wrap, on phase");
    CheckFlash(0U, true, true, "after wrap, on phase");
    CheckFlash(103U, true, true, "last millisecond of crossing on phase");
    CheckFlash(104U, true, false, "crossing on phase still lasts 200 ms");
    CheckFlash(200U, true, false, "after wrap, off phase");
    CheckFlash(249U, true, false, "last active millisecond");
    CheckFlash(250U, false, false, "exact expiry");

    // A deadline of zero is a valid wrapped expiry, not an inactive sentinel.
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 499U);
    sm.TriggerAllPiecesFlash(500U);
    Publish();
    CheckFlash(UINT32_MAX - 95U, true, true, "zero deadline, before wrap");
    CheckFlash(0U, false, false, "zero deadline, exact expiry");

    fxe::ManualMillis::SetNowMs(1000U);
    sm.SetExpectedOccupancyMask(1ULL);
    Check(sm.ForceEnterPlacementMode() && sm.IsPlacementModeActive(),
          "flash expiry fixture holds placement mode for its missing piece");
    sm.TriggerAllPiecesFlash(500U);
    Publish();
    CheckFlash(1400U, true, false, "ordinary flash");
    fxe::ManualMillis::SetNowMs(1500U);
    sm.Tick();
    Publish();
    CheckFlash(1500U, false, false, "expired during placement mode");
    CheckFlash(UINT32_MAX, false, false, "retired before later wrap");
    CheckFlash(1000U, false, false, "retired after full uptime cycle");

    fxe::ManualMillis::SetNowMs(2000U);
    sm.TriggerAllPiecesFlash(500U);
    Publish();
    fxe::ManualMillis::SetNowMs(UINT32_MAX);
    sm.RetireExpiredAllPiecesFlash(2100U);
    CheckFlash(2100U, true, true, "presentation retirement uses its owner's clock");
    fxe::ManualMillis::SetNowMs(0U);
    sm.RetireExpiredAllPiecesFlash(2500U);
    fxe::ManualMillis::SetNowMs(2500U);
    Publish();
    CheckFlash(UINT32_MAX, false, false, "presentation retires without gameplay tick");
    CheckFlash(2000U, false, false, "paused gameplay cannot revive old flash");

    // Retirement still permits the next real rejection to start a fresh cue.
    fxe::ManualMillis::SetNowMs(1000U);
    sm.TriggerAllPiecesFlash(0U);
    Publish();
    CheckFlash(1000U, true, false, "zero duration retains one millisecond");
    CheckFlash(1001U, false, false, "zero duration expires");
    sm.Reset();
    Publish();
    CheckFlash(1000U, false, false, "reset clears flash");
}

} // namespace

int main() {
    std::puts("[INTERACTION_SNAPSHOT_RENDER_STATE] start");

    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    TestAllPiecesFlashUptimeWrap();
    TestPublishedCaptureHintOnLastMove();
    TestHintRemovalClockAcrossRefinements();
    fxe::ManualMillis::SetNowMs(0U);

    runtime::GameStateMachine sm;
    sm.Reset();
    sm.SetExpectedKingAndRookOccupancyMasks((1ULL << 4U) | (1ULL << 60U), 0ULL);
    sm.TriggerAllPiecesFlash(500U);
    sm.SetHintDisplayRequested(true);
    sm.SetHintThinkingActive(true);
    sm.SetHintMoveSquares(12U, 28U);
    sm.SetHintDuckMoveSquares(18U, 27U);
    sm.SetLastMoveForSide(modules::PersistedSide::White, 8U, 24U);
    sm.SetLastMoveMostRecentSide(modules::PersistedSide::White);

    modules::EngineDerivedState derived;
    derived.sideToMove = modules::SideToMove::White;

    runtime::InteractionModelSnapshot snapshot = runtime::BuildInteractionModelSnapshot({
        .stateMachine = &sm,
        .derived = &derived,
        .variant = modules::GameVariant::Duck,
        .duckSquare = 18U,
        .setupLifecycle = runtime::SetupLifecycle::FreshSetup,
        .duckTurnPhase = modules::DuckTurnPhase::DuckRelocation,
        .variantOutcome = {
            modules::VariantOutcomeReason::Fowled,
            modules::GameWinner::White,
        },
    });

    Check(snapshot.game.variant == modules::GameVariant::Duck,
          "variant copied to game snapshot");
    Check(snapshot.game.duckSquare == 18U,
          "duck square copied to game snapshot");
    Check(snapshot.game.setupLifecycle == runtime::SetupLifecycle::FreshSetup,
          "setup lifecycle copied to game snapshot");
    Check(snapshot.game.duckTurnPhase == modules::DuckTurnPhase::DuckRelocation,
          "duck phase copied to game snapshot");
    Check(snapshot.game.variantOutcome.reason == modules::VariantOutcomeReason::Fowled &&
              snapshot.game.variantOutcome.winner == modules::GameWinner::White,
          "variant outcome copied to game snapshot");
    Check(snapshot.render.hintDisplayRequested, "hint display copied to render snapshot");
    Check(snapshot.game.hintThinking, "hint thinking copied to game snapshot");
    Check(snapshot.render.hintMoveValid, "hint move validity copied to render snapshot");
    Check(snapshot.render.hintMoveFromSquare == 12U, "hint move from square copied to render snapshot");
    Check(snapshot.render.hintMoveToSquare == 28U, "hint move to square copied to render snapshot");
    Check(snapshot.render.hintDuckMoveValid &&
              snapshot.render.hintDuckMoveFromSquare == 18U &&
              snapshot.render.hintDuckMoveToSquare == 27U,
          "compound Duck hint copied to render snapshot");
    Check(snapshot.render.allPiecesFlashActive, "all-pieces flash active copied to render snapshot");
    Check(snapshot.render.allPiecesFlashOn, "all-pieces flash phase copied to render snapshot");
    Check(snapshot.render.whiteLastMoveValid, "white last move copied to render snapshot");
    Check(snapshot.render.whiteLastMoveFromSquare == 8U, "white last move from square copied to render snapshot");
    Check(snapshot.render.whiteLastMoveToSquare == 24U, "white last move to square copied to render snapshot");
    Check(snapshot.render.lastMoveMostRecentKnown, "most recent side copied to render snapshot");
    Check(snapshot.render.lastMoveMostRecentSide == modules::PersistedSide::White,
          "most recent side value copied to render snapshot");

    const auto published = runtime::CaptureInteractionModelPresentation(sm, snapshot);
    fxe::ManualMillis::SetNowMs(200U);
    snapshot = runtime::SampleInteractionModelPresentation(published, fxe::Millis());
    Check(!snapshot.render.allPiecesFlashOn,
          "all-pieces flash advances from a frozen publication while main does no work");

    runtime::GameStateMachine cpuSm;
    cpuSm.Reset();
    const uint64_t cpuBefore = (1ULL << 7U) | (1ULL << 12U);
    cpuSm.HandleInteractionPositionAcknowledged(
        cpuBefore, 0ULL, 0ULL, cpuBefore, 0ULL);
    cpuSm.ApplyPhysicalOccupancySnapshot(cpuBefore);
    Check(cpuSm.QueueCpuMove(12U, 28U, false, 0U, '\0'),
          "queue CPU render fixture");

    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = "build/test_cpu_tentative_render.json";
    (void)std::remove(settingsConfig.path);
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "CPU tentative render settings load");

    const auto visualSettings = modules::led_frame_composer::CaptureComposeSettings(settings);
    runtime::InteractionModelSnapshot cpuSnapshot;
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.resolvedSettings = &visualSettings;
    inputs.interactionSnapshot = &cpuSnapshot;
    auto ComposeCpu = [&](std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& board,
                          std::array<modules::Rgb, modules::EDGE_LED_COUNT>& edge,
                          modules::led_frame_composer::internal::ComposeContext* outContext) {
        cpuSnapshot = runtime::BuildInteractionModelSnapshot({
                .stateMachine = &cpuSm,
                .derived = &derived,
            });
        std::array<modules::Rgb, 64> grid{};
        modules::led_frame_composer::Compose(inputs, board, edge, grid);
        if (outContext != nullptr) {
            Check(modules::led_frame_composer::internal::BuildComposeContext(
                      inputs, *outContext),
                  "CPU tentative compose context builds");
        }
    };

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> baselineBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> baselineEdge{};
    ComposeCpu(baselineBoard, baselineEdge, nullptr);
    cpuSm.HandleSensorChange(7U, false);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> invalidBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> invalidEdge{};
    modules::led_frame_composer::internal::ComposeContext invalidContext;
    ComposeCpu(invalidBoard, invalidEdge, &invalidContext);
    Check(invalidContext.cpuTentativeCorrectionMask == 0ULL &&
              invalidContext.invalidBoard.Mask() == (1ULL << 7U) &&
              invalidContext.cpuIllegalMask == (1ULL << 7U),
          "wrong CPU-piece pickup activates immediate validation");
    bool edgeChanged = false;
    for (uint8_t i = 0U; i < modules::EDGE_LED_COUNT; ++i) {
        edgeChanged = edgeChanged ||
                      !RgbEq(baselineEdge[i], invalidEdge[i]);
    }
    Check(!edgeChanged && invalidContext.correctionFeedback ==
              modules::CorrectionFeedback::Quiet,
          "wrong CPU-piece pickup initially keeps the status edge quiet");
    fxe::ManualMillis::SetNowMs(fxe::Millis() + 1500U);
    ComposeCpu(invalidBoard, invalidEdge, &invalidContext);
    edgeChanged = false;
    for (uint8_t i = 0U; i < modules::EDGE_LED_COUNT; ++i) {
        edgeChanged = edgeChanged || !RgbEq(baselineEdge[i], invalidEdge[i]);
    }
    Check(edgeChanged && invalidContext.correctionFeedback ==
              modules::CorrectionFeedback::Strong,
          "persistent wrong CPU-piece pickup escalates without changing validation");

    cpuSm.HandleSensorChange(7U, true);
    cpuSm.HandleSensorChange(12U, false);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> placementBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> placementEdge{};
    ComposeCpu(placementBoard, placementEdge, nullptr);
    cpuSm.HandleSensorChange(5U, true);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> tentativeBoard{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> tentativeEdge{};
    modules::led_frame_composer::internal::ComposeContext tentativeContext;
    ComposeCpu(tentativeBoard, tentativeEdge, &tentativeContext);
    Check(tentativeContext.cpuTentativeCorrectionMask == (1ULL << 5U) &&
              !tentativeContext.invalidBoard.IsActive() &&
              tentativeContext.cpuIllegalMask == 0ULL,
          "off-target CPU mover placement retains local tolerance");
    bool placementEdgeUnchanged = true;
    for (uint8_t i = 0U; i < modules::EDGE_LED_COUNT; ++i) {
        placementEdgeUnchanged = placementEdgeUnchanged &&
                                 RgbEq(placementEdge[i], tentativeEdge[i]);
    }
    Check(placementEdgeUnchanged,
          "off-target CPU mover placement does not trigger edge fault");
    const modules::Rgb tentativeSquare = tentativeBoard[hardware::LedIndexForSquare(
        5U, hardware::SquareLedSlot::Square0)];
    Check(tentativeSquare.r > tentativeSquare.g &&
              tentativeSquare.g > tentativeSquare.b,
          "off-target CPU mover placement renders local amber cue");

    if (g_failures == 0) {
        std::puts("[INTERACTION_SNAPSHOT_RENDER_STATE] ok");
        return 0;
    }

    std::printf("[INTERACTION_SNAPSHOT_RENDER_STATE] failures=%d\n", g_failures);
    return 1;
}
