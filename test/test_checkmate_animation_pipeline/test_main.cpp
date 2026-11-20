#include <array>
#include <cstdint>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/board_led_output_mapping.hpp"
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

uint16_t Intensity(const modules::Rgb c) {
    return static_cast<uint16_t>(c.r) + static_cast<uint16_t>(c.g) + static_cast<uint16_t>(c.b);
}

uint64_t FenOccupancy(const char* fen) {
    uint64_t mask = 0ULL;
    uint8_t square = 0U;
    for (const char* p = fen; p != nullptr && *p != '\0' && *p != ' '; ++p) {
        if (*p == '/') {
            continue;
        }
        if (*p >= '1' && *p <= '8') {
            square = static_cast<uint8_t>(square + static_cast<uint8_t>(*p - '0'));
            continue;
        }
        if (square < 64U) {
            mask |= (1ULL << square);
            ++square;
        }
    }
    return mask;
}

template <size_t N>
bool FramesEqual(const std::array<modules::Rgb, N>& a,
                 const std::array<modules::Rgb, N>& b) {
    for (size_t i = 0; i < N; ++i) {
        if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b) {
            return false;
        }
    }
    return true;
}

bool BoardLedRegionsEqual(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& a,
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& b) {
    for (uint16_t i = 0U; i < hardware::FirstEdgeLedIndex(); ++i) {
        if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b) {
            return false;
        }
    }
    return true;
}

bool RgbEqual(const modules::Rgb a, const modules::Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool AnyGridDifferenceOutsideMask(
    const std::array<modules::Rgb, 64>& a,
    const std::array<modules::Rgb, 64>& b,
    const uint64_t excludedMask) {
    for (uint8_t square = 0U; square < 64U; ++square) {
        if (((excludedMask >> square) & 1ULL) != 0ULL) {
            continue;
        }
        if (!RgbEqual(a[square], b[square])) {
            return true;
        }
    }
    return false;
}

bool HomeEdgeIsBlack(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& frame,
    const uint8_t start) {
    for (uint8_t i = 0U; i < 8U; ++i) {
        const auto color = frame[static_cast<uint8_t>(start + i)];
        if (Intensity(color) != 0U) {
            return false;
        }
    }
    return true;
}

uint8_t BrightestHomeIndex(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& frame,
    const uint8_t start) {
    uint8_t brightest = 0U;
    uint16_t brightestIntensity = 0U;
    for (uint8_t i = 0U; i < 8U; ++i) {
        const uint16_t intensity = Intensity(frame[static_cast<uint8_t>(start + i)]);
        if (intensity > brightestIntensity) {
            brightest = i;
            brightestIntensity = intensity;
        }
    }
    return brightest;
}

modules::Rgb PieceLedAt(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame, const uint8_t physicalSq) {
    return ledFrame[hardware::LedIndexForSquare(physicalSq, hardware::SquareLedSlot::Piece)];
}

} // namespace

int main() {
    std::puts("[CHECKMATE_ANIMATION_PIPELINE] start");

    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_checkmate_animation_pipeline.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    // Ensure piece colors are non-black.
    settings.Doc()["P1_PIECE_WHEEL"] = 10;
    settings.Doc()["P2_PIECE_WHEEL"] = 200;
    settings.Doc()["CHECKMATE_ANIM_STYLE"] = "fireworks";

    runtime::GameStateMachineConfig smCfg;
    smCfg.cpuIdleDelayMs = 999999;
    smCfg.placementStableRequiredMs = 0;

    runtime::GameStateMachine sm(smCfg);
    sm.Reset();

    modules::LitchMaxEngine engine;
    engine.Reset();

    // Checkmate-contributor scenario from real gameplay:
    // black king h4 is checkmated by rook h6 with queen g7 controlling escape squares.
    const char* fenMate = "8/6Q1/7R/8/2p1P2k/4P3/2PK4/8 b - - 0 1";
    Check(engine.SetFen(fenMate), "SetFen(mate)");

    // Capture checkmate start time deterministically.
    fxe::ManualMillis::SetNowMs(0);
    modules::EngineDerivedState derived;
    derived.RefreshFromEngine(engine);
    Check(derived.inCheckmate, "derived.inCheckmate");
    Check(derived.threatenedKingSquareValid, "threatened king square valid");

    const uint64_t mateOccupancy = FenOccupancy(fenMate);
    sm.SetExpectedOccupancyMask(mateOccupancy);
    sm.DebugSetPhysicalOccupancyMask(mateOccupancy);

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledBefore{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeBefore{};
    std::array<modules::Rgb, 64> gridBefore{};

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledImpact{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeImpact{};
    std::array<modules::Rgb, 64> gridImpact{};

    modules::led_frame_composer::ComposeInputs in;
    in.settings = &settings;
    in.firstPlayerIsP2 = false;
    in.menuOverlay = nullptr;
    in.menuActive = false;

    // Checkmate should keep the king and contributors readable while pulsing their squares.
    fxe::ManualMillis::SetNowMs(200);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledBefore, edgeBefore, gridBefore);
    const modules::Rgb kingBefore = PieceLedAt(ledBefore, 39);
    const modules::Rgb contributorBefore = PieceLedAt(ledBefore, 14);
    const runtime::InteractionModelSnapshot mateSnapshot =
        test_support::BuildInteractionSnapshot(sm, derived, &engine);
    uint64_t contributorsMask = 0ULL;
    Check(modules::led_frame_composer::TryComputeCheckmateContributorsMask(
              mateSnapshot, contributorsMask),
          "checkmate contributors resolve from immutable snapshot");
    Check(contributorsMask == ((1ULL << 14U) | (1ULL << 23U)),
          "checkmate contributors are g7 and h6");
    Check(Intensity(kingBefore) > 0U, "king piece lit before impact");
    Check(Intensity(gridBefore[39]) > Intensity(gridBefore[0]), "mated king square is accented");
    Check(Intensity(contributorBefore) > 0U, "contributor piece stays readable");
    Check(HomeEdgeIsBlack(edgeBefore, 0U),
          "white-win checkmate keeps all eight losing P2 edge pixels black");
    Check(!RgbEqual(edgeBefore[16], edgeBefore[0]),
          "white-win checkmate animates the winning P1 home edge");

    fxe::ManualMillis::SetNowMs(1300);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, ledImpact, edgeImpact, gridImpact);
    const modules::Rgb kingImpact = PieceLedAt(ledImpact, 39);
    const modules::Rgb contributorImpact = PieceLedAt(ledImpact, 14);
    Check(Intensity(kingImpact) > 0U, "king piece stays readable later in the pulse");
    Check(Intensity(gridImpact[39]) > Intensity(gridImpact[0]), "mated king square stays accented");
    Check(Intensity(contributorImpact) > 0U, "contributor piece stays readable later in the pulse");
    Check(!FramesEqual(edgeBefore, edgeImpact), "status ring visibly animates over the checkmate episode");
    Check(!FramesEqual(gridBefore, gridImpact), "board accents visibly animate over the checkmate episode");

    // Brightness drift with fixed geometry reads as a stuck pattern on the
    // physical ring. Require the expanding sparks to travel across most of the
    // winner's home edge during one episode.
    std::array<bool, 8> visitedWinnerEdgePositions{};
    uint8_t visitedWinnerEdgeCount = 0U;
    constexpr std::array<uint32_t, 7> MOTION_SAMPLE_TIMES_MS = {
        200U, 500U, 800U, 1100U, 1400U, 1700U, 2000U};
    for (const uint32_t sampleMs : MOTION_SAMPLE_TIMES_MS) {
        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> sampleLed{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> sampleEdge{};
        std::array<modules::Rgb, 64> sampleGrid{};
        fxe::ManualMillis::SetNowMs(sampleMs);
        test_support::ComposeWithSnapshot(
            in, sm, derived, &engine, sampleLed, sampleEdge, sampleGrid);
        const uint8_t peak = BrightestHomeIndex(sampleEdge, 16U);
        if (!visitedWinnerEdgePositions[peak]) {
            visitedWinnerEdgePositions[peak] = true;
            ++visitedWinnerEdgeCount;
        }
    }
    Check(visitedWinnerEdgeCount >= 4U,
          "checkmate status animation has visible spatial motion");

    // Prove the board keeps breathing after the 900ms entry fade has ended.
    // This prevents a static post-fade treatment from satisfying the animation
    // test merely because two frames used different fade scales.
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> pulseHighLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> pulseHighEdge{};
    std::array<modules::Rgb, 64> pulseHighGrid{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> pulseLowLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> pulseLowEdge{};
    std::array<modules::Rgb, 64> pulseLowGrid{};
    fxe::ManualMillis::SetNowMs(1100U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, pulseHighLed, pulseHighEdge, pulseHighGrid);
    fxe::ManualMillis::SetNowMs(2200U);
    test_support::ComposeWithSnapshot(
        in, sm, derived, &engine, pulseLowLed, pulseLowEdge, pulseLowGrid);
    Check(!FramesEqual(pulseHighGrid, pulseLowGrid),
          "checkmate board continues pulsing after its entry fade");
    const uint64_t checkmateAnchorMask = contributorsMask | (1ULL << 39U);
    Check(AnyGridDifferenceOutsideMask(
              pulseHighGrid,
              pulseLowGrid,
              checkmateAnchorMask),
          "checkmate fireworks animate non-anchor squares");

    // The established alternate style launches fire from the checkmate
    // contributors toward the mated king. Style selection was accidentally
    // removed during cleanup, so exercise it through the complete compositor
    // rather than testing a detached animation helper.
    modules::SettingsStoreConfig fireLobSettingsCfg;
    fireLobSettingsCfg.path = "build/test_checkmate_animation_pipeline_fire_lob.json";
    modules::SettingsStore fireLobSettings(fireLobSettingsCfg);
    (void)fireLobSettings.Load();
    fireLobSettings.Doc()["P1_PIECE_WHEEL"] = 10;
    fireLobSettings.Doc()["P2_PIECE_WHEEL"] = 200;
    fireLobSettings.Doc()["CHECKMATE_ANIM_STYLE"] = "fire-lob";
    fireLobSettings.Doc()["MINIMAL_ANIMATIONS"] = false;

    modules::led_frame_composer::ComposeInputs fireLobIn = in;
    fireLobIn.settings = &fireLobSettings;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> fireLobBeforeLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> fireLobBeforeEdge{};
    std::array<modules::Rgb, 64> fireLobBeforeGrid{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> fireLobImpactLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> fireLobImpactEdge{};
    std::array<modules::Rgb, 64> fireLobImpactGrid{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> fireworksImpactLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> fireworksImpactEdge{};
    std::array<modules::Rgb, 64> fireworksImpactGrid{};

    fxe::ManualMillis::SetNowMs(200U);
    test_support::ComposeWithSnapshot(
        fireLobIn,
        sm,
        derived,
        &engine,
        fireLobBeforeLed,
        fireLobBeforeEdge,
        fireLobBeforeGrid);
    fxe::ManualMillis::SetNowMs(910U);
    test_support::ComposeWithSnapshot(
        fireLobIn,
        sm,
        derived,
        &engine,
        fireLobImpactLed,
        fireLobImpactEdge,
        fireLobImpactGrid);
    test_support::ComposeWithSnapshot(
        in,
        sm,
        derived,
        &engine,
        fireworksImpactLed,
        fireworksImpactEdge,
        fireworksImpactGrid);

    Check(Intensity(PieceLedAt(fireLobBeforeLed, 39U)) > 0U,
          "fire-lob keeps the mated king lit before impact");
    Check(Intensity(PieceLedAt(fireLobImpactLed, 39U)) == 0U,
          "fire-lob darkens the mated king during contributor impact");
    Check(!BoardLedRegionsEqual(fireLobImpactLed, fireworksImpactLed),
          "fire-lob and fireworks settings select different board animations");

    settings.Doc()["CHECKMATE_ANIM_STYLE"] = "fireworks";
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> fireworksStyleLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> fireworksStyleEdge{};
    std::array<modules::Rgb, 64> fireworksStyleGrid{};
    fxe::ManualMillis::SetNowMs(1050U);
    test_support::ComposeWithSnapshot(
        in,
        sm,
        derived,
        &engine,
        fireworksStyleLed,
        fireworksStyleEdge,
        fireworksStyleGrid);

    static constexpr std::array<const char*, 3> ADDED_STYLES = {
        "ripple-bloom",
        "winner-sweep",
        "winner-constellation",
    };
    for (const char* style : ADDED_STYLES) {
        settings.Doc()["CHECKMATE_ANIM_STYLE"] = style;
        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> styleEarlyLed{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> styleEarlyEdge{};
        std::array<modules::Rgb, 64> styleEarlyGrid{};
        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> styleLaterLed{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> styleLaterEdge{};
        std::array<modules::Rgb, 64> styleLaterGrid{};
        fxe::ManualMillis::SetNowMs(350U);
        test_support::ComposeWithSnapshot(
            in,
            sm,
            derived,
            &engine,
            styleEarlyLed,
            styleEarlyEdge,
            styleEarlyGrid);
        fxe::ManualMillis::SetNowMs(1050U);
        test_support::ComposeWithSnapshot(
            in,
            sm,
            derived,
            &engine,
            styleLaterLed,
            styleLaterEdge,
            styleLaterGrid);
        Check(!FramesEqual(styleEarlyGrid, styleLaterGrid),
              "added checkmate style has visible board motion");
        Check(!FramesEqual(styleLaterGrid, fireworksStyleGrid),
              "added checkmate style selects a distinct board recipe");
    }

    settings.Doc()["CHECKMATE_ANIM_STYLE"] = "winner-sweep";
    settings.Doc()["MINIMAL_ANIMATIONS"] = true;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> minimalEarlyLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> minimalEarlyEdge{};
    std::array<modules::Rgb, 64> minimalEarlyGrid{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> minimalLaterLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> minimalLaterEdge{};
    std::array<modules::Rgb, 64> minimalLaterGrid{};
    fxe::ManualMillis::SetNowMs(500U);
    test_support::ComposeWithSnapshot(
        in,
        sm,
        derived,
        &engine,
        minimalEarlyLed,
        minimalEarlyEdge,
        minimalEarlyGrid);
    fxe::ManualMillis::SetNowMs(1500U);
    test_support::ComposeWithSnapshot(
        in,
        sm,
        derived,
        &engine,
        minimalLaterLed,
        minimalLaterEdge,
        minimalLaterGrid);
    Check(FramesEqual(minimalEarlyGrid, minimalLaterGrid),
          "Minimal Animations replaces the selected checkmate motion with a static board result");
    Check(FramesEqual(minimalEarlyEdge, minimalLaterEdge),
          "Minimal Animations keeps a static winner-owned checkmate edge");
    Check(HomeEdgeIsBlack(minimalEarlyEdge, 0U),
          "Minimal checkmate keeps all eight losing edge pixels black");
    Check(Intensity(PieceLedAt(minimalEarlyLed, 39U)) > 0U,
          "Minimal checkmate keeps the losing king readable");
    // Stationary terminal markers obey Hall occupancy even though the
    // acknowledged mate position still names the king and contributors.
    const uint64_t missingMateAnchors =
        mateOccupancy & ~((1ULL << 39U) | (1ULL << 14U));
    sm.DebugSetPhysicalOccupancyMask(missingMateAnchors);
    for (const bool flipSides : std::array<bool, 2>{false, true}) {
        modules::led_frame_composer::ComposeInputs emptyIn = in;
        std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> emptyLed{};
        std::array<modules::Rgb, modules::EDGE_LED_COUNT> emptyEdge{};
        std::array<modules::Rgb, 64> emptyGrid{};
        test_support::ComposeWithSnapshot(
            emptyIn, sm, derived, &engine, emptyLed, emptyEdge, emptyGrid);
        const auto physical = hardware::MapSemanticLedFrame(
            {hardware::BoardRevision::V2S3, flipSides, flipSides},
            emptyLed, emptyEdge);
        const uint8_t king = flipSides ? 24U : 39U;
        const uint8_t contributor = flipSides ? 49U : 14U;
        const uint8_t remainingRook = flipSides ? 40U : 23U;
        Check(Intensity(physical[hardware::LedIndexForSquare(
                      king, hardware::SquareLedSlot::Piece)]) == 0U &&
                  Intensity(physical[hardware::LedIndexForSquare(
                      contributor, hardware::SquareLedSlot::Piece)]) == 0U &&
                  Intensity(physical[hardware::LedIndexForSquare(
                      remainingRook, hardware::SquareLedSlot::Piece)]) > 0U,
              "stationary mate markers require mapped physical occupancy");
    }

    // Preserve the explicit travelling Fire Lob source glow. Once travel has
    // ended, the same vacant source must return to black; checkmate itself is
    // never a blanket permission to light absent pieces.
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> emptyTravelLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> emptyTravelEdge{};
    std::array<modules::Rgb, 64> emptyTravelGrid{};
    fxe::ManualMillis::SetNowMs(200U);
    test_support::ComposeWithSnapshot(
        fireLobIn, sm, derived, &engine,
        emptyTravelLed, emptyTravelEdge, emptyTravelGrid);
    Check(Intensity(PieceLedAt(emptyTravelLed, 14U)) > 0U &&
              Intensity(PieceLedAt(emptyTravelLed, 39U)) == 0U,
          "Fire Lob travel may glow at its source without reviving the absent king");
    fxe::ManualMillis::SetNowMs(1900U);
    test_support::ComposeWithSnapshot(
        fireLobIn, sm, derived, &engine,
        emptyTravelLed, emptyTravelEdge, emptyTravelGrid);
    Check(Intensity(PieceLedAt(emptyTravelLed, 14U)) == 0U &&
              Intensity(PieceLedAt(emptyTravelLed, 39U)) == 0U,
          "empty Fire Lob source returns to black after travel expires");
    sm.DebugSetPhysicalOccupancyMask(mateOccupancy);
    settings.Doc()["MINIMAL_ANIMATIONS"] = false;
    settings.Doc()["CHECKMATE_ANIM_STYLE"] = "fireworks";

    // The same elapsed point in two different checkmate episodes must render
    // identically. Absolute uptime must not select the animation phase.
    modules::EngineDerivedState earlyEpisode = derived;
    earlyEpisode.checkmateStartMs = 1000U;
    modules::EngineDerivedState lateEpisode = derived;
    lateEpisode.checkmateStartMs = 5000U;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> earlyLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> earlyEdge{};
    std::array<modules::Rgb, 64> earlyGrid{};
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> lateLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> lateEdge{};
    std::array<modules::Rgb, 64> lateGrid{};

    fxe::ManualMillis::SetNowMs(1300U);
    test_support::ComposeWithSnapshot(
        in, sm, earlyEpisode, &engine, earlyLed, earlyEdge, earlyGrid);
    fxe::ManualMillis::SetNowMs(5300U);
    test_support::ComposeWithSnapshot(
        in, sm, lateEpisode, &engine, lateLed, lateEdge, lateGrid);
    Check(FramesEqual(earlyEdge, lateEdge),
          "checkmate status animation is anchored to episode elapsed time");
    Check(FramesEqual(earlyGrid, lateGrid),
          "checkmate board animation is anchored to episode elapsed time");

    // Fatal game-over owns the complete frame. Stale lower-priority flags must
    // not alter either board or status output.
    fxe::ManualMillis::SetNowMs(1300U);
    runtime::InteractionModelSnapshot baselineSnapshot =
        test_support::BuildInteractionSnapshot(sm, earlyEpisode, &engine);
    modules::led_frame_composer::ComposeInputs baselineIn = in;
    baselineIn.interactionSnapshot = &baselineSnapshot;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> baselineLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> baselineEdge{};
    std::array<modules::Rgb, 64> baselineGrid{};
    modules::led_frame_composer::Compose(
        baselineIn, baselineLed, baselineEdge, baselineGrid);

    runtime::InteractionModelSnapshot noisySnapshot = baselineSnapshot;
    noisySnapshot.render.menuOverlayActive = true;
    // An explicitly active Quick Setup is now a user-owned exception; this
    // fixture retains unsolicited menu flags to test fatal suppression.
    noisySnapshot.render.quickSettingsOverlayActive = false;
    noisySnapshot.render.textOverlayActive = true;
    noisySnapshot.render.hintDisplayRequested = true;
    noisySnapshot.render.allPiecesFlashActive = true;
    noisySnapshot.render.allPiecesFlashOn = false;
    noisySnapshot.game.cpuThinking = true;
    noisySnapshot.game.hintThinking = true;
    noisySnapshot.interaction.placementModeActive = true;
    noisySnapshot.interaction.humanIllegalMask = 1ULL;
    noisySnapshot.interaction.illegalOn = true;
    noisySnapshot.interaction.taggedState.tag =
        runtime::ChessInteractionPhase::ExecutingPlan;
    noisySnapshot.interaction.taggedState.plan.kind =
        runtime::PhysicalTransitionKind::Undo;
    noisySnapshot.interaction.taggedState.plan.source =
        runtime::PhysicalTransitionSource::Undo;
    noisySnapshot.interaction.taggedState.plan.moveFrom = 14U;
    noisySnapshot.interaction.taggedState.plan.moveTo = 15U;
    noisySnapshot.interaction.visualActiveSideOverrideValid = true;
    noisySnapshot.render.moveTransition = runtime::MoveRenderTransition{
        runtime::MoveRenderTransitionKind::CancelUndo, 14U, 15U};
    modules::led_frame_composer::ComposeInputs noisyIn = in;
    noisyIn.interactionSnapshot = &noisySnapshot;
    noisyIn.menuActive = true;
    noisyIn.menuVisualAlpha255 = 255U;
    noisyIn.menuBackgroundDimScale256 = 0U;
    noisyIn.menuIntent.menuActive = true;
    noisyIn.menuIntent.menuOwner = modules::UiOwner::P2;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> noisyLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> noisyEdge{};
    std::array<modules::Rgb, 64> noisyGrid{};
    modules::led_frame_composer::Compose(
        noisyIn, noisyLed, noisyEdge, noisyGrid);
    Check(FramesEqual(baselineLed, noisyLed),
          "fatal checkmate suppresses lower-priority board effects");
    Check(FramesEqual(baselineEdge, noisyEdge),
          "fatal checkmate suppresses lower-priority status effects");
    Check(FramesEqual(baselineGrid, noisyGrid),
          "fatal checkmate keeps one authoritative grid effect");

    // Destructive-hold progress is safety feedback, not a competing menu. It
    // may temporarily own the status ring without dimming or replacing the
    // checkmate board treatment.
    modules::led_frame_composer::ComposeInputs resetHoldIn = baselineIn;
    resetHoldIn.menuIntent.menuActive = true;
    resetHoldIn.menuIntent.menuOwner = modules::UiOwner::P1;
    resetHoldIn.menuIntent.menuHoldActive = true;
    resetHoldIn.menuIntent.menuHoldFillAll = true;
    resetHoldIn.menuIntent.menuValue01_255 = 128U;
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> resetHoldLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> resetHoldEdge{};
    std::array<modules::Rgb, 64> resetHoldGrid{};
    modules::led_frame_composer::Compose(
        resetHoldIn, resetHoldLed, resetHoldEdge, resetHoldGrid);
    Check(!FramesEqual(baselineEdge, resetHoldEdge),
          "reset hold progress remains visible during checkmate");
    Check(resetHoldEdge[0].g > resetHoldEdge[0].r,
          "reset hold progress retains its green safety color");
    Check(FramesEqual(baselineGrid, resetHoldGrid),
          "reset hold progress does not disturb the checkmate board");
    Check(BoardLedRegionsEqual(baselineLed, resetHoldLed),
          "reset hold progress leaves all board LED slots unchanged");

    // Mirror the winner: white is checkmated, so Black/P2 owns the animated
    // home edge when P1 owns White.
    constexpr const char* WHITE_MATED_FEN =
        "7K/6q1/6k1/8/8/8/8/8 w - - 0 1";
    modules::LitchMaxEngine blackWinEngine;
    blackWinEngine.Reset();
    Check(blackWinEngine.SetFen(WHITE_MATED_FEN), "SetFen(white mated)");
    modules::EngineDerivedState blackWinDerived;
    fxe::ManualMillis::SetNowMs(9000U);
    blackWinDerived.RefreshFromEngine(blackWinEngine);
    Check(blackWinDerived.inCheckmate, "white-mated fixture is checkmate");
    runtime::GameStateMachine blackWinSm(smCfg);
    blackWinSm.Reset();
    const uint64_t blackWinOccupancy = FenOccupancy(WHITE_MATED_FEN);
    blackWinSm.SetExpectedOccupancyMask(blackWinOccupancy);
    blackWinSm.DebugSetPhysicalOccupancyMask(blackWinOccupancy);
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> blackWinLed{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> blackWinEdge{};
    std::array<modules::Rgb, 64> blackWinGrid{};
    fxe::ManualMillis::SetNowMs(9200U);
    test_support::ComposeWithSnapshot(in,
                                      blackWinSm,
                                      blackWinDerived,
                                      &blackWinEngine,
                                      blackWinLed,
                                      blackWinEdge,
                                      blackWinGrid);
    Check(HomeEdgeIsBlack(blackWinEdge, 16U),
          "black-win checkmate keeps all eight losing P1 edge pixels black");
    Check(!RgbEqual(blackWinEdge[0], blackWinEdge[16]),
          "black-win checkmate animates the winning P2 home edge");

    if (g_failures == 0) {
        std::puts("[CHECKMATE_ANIMATION_PIPELINE] ok");
        return 0;
    }

    std::printf("[CHECKMATE_ANIMATION_PIPELINE] failures=%d\n", g_failures);
    return 1;
}
