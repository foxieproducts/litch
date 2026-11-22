#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../support/led_frame_composer_snapshot_adapter.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/led_layout.hpp"
#include "modules/engine_derived_state.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/orientation.hpp"
#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"
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

template <std::size_t N>
modules::Rgb PieceLedAtSquare(const std::array<modules::Rgb, N>& ledFrame,
                              const uint8_t physicalSq) {
    return ledFrame[hardware::LedIndexForSquare(physicalSq, hardware::SquareLedSlot::Piece)];
}

modules::Rgb SquareLedAtSquare(const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& ledFrame,
                               const uint8_t physicalSq,
                               const hardware::SquareLedSlot slot =
                                   hardware::SquareLedSlot::Square0) {
    return ledFrame[hardware::LedIndexForSquare(physicalSq, slot)];
}

uint16_t Energy(const modules::Rgb color) {
    return static_cast<uint16_t>(color.r) + color.g + color.b;
}

bool AnyStatusLit(
    const std::array<modules::Rgb, modules::EDGE_LED_COUNT>& status) {
    for (const modules::Rgb color : status) {
        if (color.r != 0U || color.g != 0U || color.b != 0U) {
            return true;
        }
    }
    return false;
}

void CheckOwnerColor(const modules::Rgb actual,
                     const modules::UiOwner expectedOwner,
                     const uint8_t logicalSquare,
                     const uint8_t physicalSquare,
                     const bool firstPlayerIsP2,
                     const bool boardRotate,
                     const bool squareRotate) {
    const bool matches = expectedOwner == modules::UiOwner::P1
                             ? actual.r > actual.g && actual.r > actual.b
                             : actual.g > actual.r && actual.b > actual.r;
    if (matches) {
        return;
    }

    std::printf(
        "[FAIL] K/Q owner color logical=%u physical=%u first=%s "
        "board_rotate=%u square_rotate=%u owner=%s actual=%u,%u,%u\n",
        static_cast<unsigned>(logicalSquare),
        static_cast<unsigned>(physicalSquare),
        firstPlayerIsP2 ? "P2" : "P1",
        boardRotate ? 1U : 0U,
        squareRotate ? 1U : 0U,
        expectedOwner == modules::UiOwner::P1 ? "P1" : "P2",
        static_cast<unsigned>(actual.r),
        static_cast<unsigned>(actual.g),
        static_cast<unsigned>(actual.b));
    ++g_failures;
}

void ComposeFreshKq(
    modules::led_frame_composer::ComposeInputs inputs,
    const runtime::GameStateMachine& machine,
    const modules::EngineDerivedState& derived,
    const modules::IEngine& engine,
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& board,
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>& status,
    std::array<modules::Rgb, 64>& grid) {
    char fen[litch_max::FEN_BUFFER_CAPACITY] = {};
    (void)engine.GetFen(fen, sizeof(fen));
    runtime::InteractionModelSnapshot snapshot =
        runtime::BuildInteractionModelSnapshot({
            .stateMachine = &machine,
            .derived = &derived,
            .authoritativeFen = fen,
            .setupLifecycle = runtime::SetupLifecycle::FreshSetup,
        });
    inputs.interactionSnapshot = &snapshot;
    modules::led_frame_composer::Compose(
        inputs, board, status, grid);
}

} // namespace

int main() {
    std::puts("[KQ_SWAP_PROMPT_RENDER] start");

    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    modules::SettingsStoreConfig settingsCfg;
    settingsCfg.path = "build/test_kq_swap_prompt_render.json";
    modules::SettingsStore settings(settingsCfg);
    (void)settings.Load();

    settings.Doc()["P1_PIECE_WHEEL"] = 0U;
    settings.Doc()["P2_PIECE_WHEEL"] = 128U;

    runtime::GameStateMachine sm;
    sm.Reset();

    modules::LitchMaxEngine engine;
    engine.Reset();

    modules::EngineDerivedState derived;
    derived.sideToMove = modules::SideToMove::White;

    // Use the canonical opening so the production FreshSetup presentation owns
    // temporary K/Q vacancies instead of generic mid-game mismatch feedback.
    static constexpr uint8_t E8 = 4U;
    static constexpr uint8_t D8 = 3U;
    static constexpr uint8_t D1 = 59U;
    static constexpr uint8_t E1 = 60U;
    static constexpr uint64_t KQ_MASK64 = (1ULL << E1) | (1ULL << D1) | (1ULL << E8) | (1ULL << D8);
    static constexpr uint64_t INITIAL_OCCUPANCY =
        0xFFFF00000000FFFFULL;

    sm.SetExpectedOccupancyMask(INITIAL_OCCUPANCY);
    sm.DebugSetPhysicalOccupancyMask(INITIAL_OCCUPANCY);

    sm.TriggerInitialKingPlacementPrompt();
    Check(sm.StartKingQueenSwapPrompt(KQ_MASK64), "start kq prompt");
    fxe::ManualMillis::SetNowMs(0U);

    modules::led_frame_composer::ComposeInputs in;
    in.settings = &settings;
    in.firstPlayerIsP2 = true;

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> ledFrame{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> edgeFrame{};
    std::array<modules::Rgb, 64> gridFrame{};

    // The K/Q correction is the more specific physical action. It owns the
    // four squares while preserving the optional king identity assist so the
    // latter can resume if an untouched K/Q correction is canceled.
    char precedenceFen[litch_max::FEN_BUFFER_CAPACITY] = {};
    (void)engine.GetFen(precedenceFen, sizeof(precedenceFen));
    runtime::InteractionModelSnapshot precedenceSnapshot =
        runtime::BuildInteractionModelSnapshot({
            .stateMachine = &sm,
            .derived = &derived,
            .authoritativeFen = precedenceFen,
            .setupLifecycle = runtime::SetupLifecycle::FreshSetup,
        });
    in.interactionSnapshot = &precedenceSnapshot;
    modules::led_frame_composer::internal::ComposeContext precedenceContext;
    Check(modules::led_frame_composer::internal::BuildComposeContext(
              in, precedenceContext) &&
              precedenceContext.kingQueenSwapPromptActive,
          "K/Q correction owns the fresh-setup render context");
    Check(!precedenceContext.initialKingPlacementPromptActive,
          "K/Q correction suppresses the overlapping king assist");
    Check(precedenceContext.kingQueenSwapPromptFlashMask == KQ_MASK64,
          "K/Q correction starts with all four target squares pending");
    uint64_t latentKingMask = 0ULL;
    Check(sm.TryGetInitialKingPlacementPrompt(latentKingMask),
          "K/Q correction preserves the latent king assist");

    // Exercise the full GAME_FIRST_PLAYER x BOARD_ROTATE matrix. The snapshot
    // masks stay logical; each cue is mapped to its physical square exactly
    // once using literal BOARD_ROTATE. First-player choice changes neither
    // coordinates nor the existing owner palette; literal rotation moves the
    // cue without exchanging its color.
    static constexpr std::array<uint8_t, 4> KQ_SQUARES = {
        E1, D1, E8, D8};
    for (uint8_t firstPlayerIndex = 0U; firstPlayerIndex < 2U;
         ++firstPlayerIndex) {
        const bool firstPlayerIsP2 = firstPlayerIndex != 0U;
        for (uint8_t boardRotateIndex = 0U; boardRotateIndex < 2U;
             ++boardRotateIndex) {
            const bool boardRotate = boardRotateIndex != 0U;
            const bool squareRotate = modules::ComputeSquareRotate(
                boardRotate, firstPlayerIsP2);
            in.firstPlayerIsP2 = firstPlayerIsP2;
            // Production applies this at the hardware output boundary. The
            // native composer seam uses flipSides to exercise the same stable
            // logical-to-physical involution without hardware I/O.
            ComposeFreshKq(
                in, sm, derived, engine, ledFrame, edgeFrame, gridFrame);
            const auto physicalFrame = hardware::MapSemanticLedFrame(
                {hardware::BoardRevision::V2S3, squareRotate, squareRotate},
                ledFrame, edgeFrame);

            for (std::size_t i = 0U; i < KQ_SQUARES.size(); ++i) {
                const uint8_t logicalSquare = KQ_SQUARES[i];
                const uint8_t physicalSquare = runtime::MapSquareIndex(
                    logicalSquare, squareRotate);
                const bool whiteBackRank = logicalSquare >= 56U;
                const modules::UiOwner logicalOwner =
                    whiteBackRank
                        ? modules::UiOwner::P1
                        : modules::UiOwner::P2;
                const modules::UiOwner expectedPalette = firstPlayerIsP2
                    ? modules::SwapP1P2Owner(logicalOwner) : logicalOwner;
                const modules::Rgb cue = PieceLedAtSquare(
                    physicalFrame, physicalSquare);
                CheckOwnerColor(cue,
                                expectedPalette,
                                logicalSquare,
                                physicalSquare,
                                firstPlayerIsP2,
                                boardRotate,
                                squareRotate);
            }
        }
    }

    in.firstPlayerIsP2 = false;
    ComposeFreshKq(
        in, sm, derived, engine, ledFrame, edgeFrame, gridFrame);

    const modules::Rgb d1OuterAtStart = SquareLedAtSquare(
        ledFrame, D1, hardware::SquareLedSlot::Square0);
    const modules::Rgb d1InnerAtStart = SquareLedAtSquare(
        ledFrame, D1, hardware::SquareLedSlot::Square2);
    const modules::Rgb e1InnerAtStart = SquareLedAtSquare(
        ledFrame, E1, hardware::SquareLedSlot::Square0);
    const modules::Rgb e1OuterAtStart = SquareLedAtSquare(
        ledFrame, E1, hardware::SquareLedSlot::Square2);
    Check(Energy(d1OuterAtStart) > Energy(d1InnerAtStart) &&
              Energy(d1InnerAtStart) != 0U &&
              Energy(e1InnerAtStart) != 0U &&
              Energy(e1OuterAtStart) != 0U,
          "reciprocal cue starts with a strong head on a persistent four-LED bracket");

    // The bright head travels across the two adjacent squares, through the
    // two LEDs at their shared seam, then reverses. This must be much stronger
    // than the previous all-square breathing cue.
    fxe::ManualMillis::SetNowMs(320U);
    ComposeFreshKq(
        in, sm, derived, engine, ledFrame, edgeFrame, gridFrame);
    const modules::Rgb e1InnerDuringTravel = SquareLedAtSquare(
        ledFrame, E1, hardware::SquareLedSlot::Square0);
    Check(Energy(e1InnerDuringTravel) > Energy(e1InnerAtStart),
          "reciprocal cue visibly travels across the shared d/e seam");

    fxe::ManualMillis::SetNowMs(700U);
    ComposeFreshKq(
        in, sm, derived, engine, ledFrame, edgeFrame, gridFrame);
    Check(Energy(PieceLedAtSquare(ledFrame, D1)) == 0U &&
              Energy(PieceLedAtSquare(ledFrame, E1)) == 0U &&
              Energy(SquareLedAtSquare(ledFrame, D1)) != 0U &&
              Energy(SquareLedAtSquare(ledFrame, E1)) != 0U,
          "pending K/Q pieces blink off while their required spatial bracket remains visible");

    modules::led_frame_composer::ComposeInputs pausedIn = in;
    pausedIn.runtimeHealth.state = runtime::RuntimeHealthState::InputPaused;
    pausedIn.runtimeHealth.advice.boardInputAllowed = false;
    ComposeFreshKq(
        pausedIn, sm, derived, engine, ledFrame, edgeFrame, gridFrame);
    Check(Energy(SquareLedAtSquare(
              ledFrame, E1, hardware::SquareLedSlot::Square0)) == 0U,
          "InputPaused replaces the K/Q movement invitation");

    modules::led_frame_composer::ComposeInputs recoveryIn = in;
    recoveryIn.higherPriorityStatusOverlayActive = true;
    ComposeFreshKq(
        recoveryIn, sm, derived, engine, ledFrame, edgeFrame, gridFrame);
    Check(Energy(SquareLedAtSquare(
              ledFrame, E1, hardware::SquareLedSlot::Square0)) == 0U,
          "OTA or recovery status replaces the K/Q movement invitation");

    // After one square is lifted, stop the reciprocal loop and hold a strong
    // landing socket on the vacancy while its occupied partner points inward.
    sm.HandleSensorChange(E1, false);
    fxe::ManualMillis::SetNowMs(0U);
    ComposeFreshKq(
        in, sm, derived, engine, ledFrame, edgeFrame, gridFrame);
    const modules::Rgb e1LandingStrong = SquareLedAtSquare(ledFrame, E1);
    const modules::Rgb d1SourceStrong = SquareLedAtSquare(
        ledFrame, D1, hardware::SquareLedSlot::Square2);
    Check(!AnyStatusLit(edgeFrame),
          "K/Q vacancy remains calm and does not raise status-ring fault");

    fxe::ManualMillis::SetNowMs(320U);
    ComposeFreshKq(
        in, sm, derived, engine, ledFrame, edgeFrame, gridFrame);
    Check(Energy(e1LandingStrong) > 0U && Energy(d1SourceStrong) > 0U,
          "partial K/Q swap keeps both landing socket and source direction strong");

    sm.HandleSensorChange(E1, true);
    const runtime::InteractionModelSnapshot restored =
        test_support::BuildInteractionSnapshot(sm, derived, &engine);
    Check((restored.interaction.kingQueenSwapPromptFlashMask &
           (1ULL << E1)) == 0ULL,
          "restored K/Q square leaves the pending guidance mask");
    Check((restored.interaction.kingQueenSwapPromptFlashMask &
           (1ULL << D1)) != 0ULL,
          "unvisited K/Q source remains locally guided");
    fxe::ManualMillis::SetNowMs(1020U);
    ComposeFreshKq(in, sm, derived, engine, ledFrame, edgeFrame, gridFrame);
    Check(Energy(PieceLedAtSquare(ledFrame, E1)) != 0U &&
              Energy(PieceLedAtSquare(ledFrame, D1)) == 0U,
          "restored square keeps ordinary piece light while the remaining source blinks");

    for (const uint8_t square : std::array<uint8_t, 3>{D1, D8, E8}) {
        sm.HandleSensorChange(square, false);
        sm.HandleSensorChange(square, true);
    }
    fxe::ManualMillis::SetNowMs(10U);
    ComposeFreshKq(
        in, sm, derived, engine, ledFrame, edgeFrame, gridFrame);
    Check(sm.IsKingQueenSwapPromptActive() &&
              Energy(SquareLedAtSquare(
                  ledFrame, E1, hardware::SquareLedSlot::Square0)) == 0U,
          "zero-pending stability dwell does not reflash all four K/Q squares");

    if (g_failures == 0) {
        std::puts("[KQ_SWAP_PROMPT_RENDER] ok");
        return 0;
    }

    std::printf("[KQ_SWAP_PROMPT_RENDER] failures=%d\n", g_failures);
    return 1;
}
