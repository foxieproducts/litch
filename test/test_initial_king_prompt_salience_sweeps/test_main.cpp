#include <array>
#include <cstdio>
#include <cstdlib>

#include "hardware/led_layout.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/led_power_envelope.hpp"
#include "modules/power_budget_policy.hpp"
#include "modules/rgbw_auto_map.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/settings_store.hpp"
#include "runtime/interaction_domain_types.hpp"

namespace {

constexpr uint64_t INITIAL_OCCUPANCY = 0xFFFF00000000FFFFULL;
constexpr uint8_t BLACK_KING = 4U;
constexpr uint8_t BLACK_QUEEN = 3U;
constexpr uint8_t WHITE_QUEEN = 59U;
constexpr uint8_t WHITE_KING = 60U;
constexpr uint8_t WHITE_PAWN_E2 = 52U;
constexpr uint8_t RGBW_COOL_TO_WARM_THRESHOLD = 32U;

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }

    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

uint16_t Energy(const modules::Rgb color) {
    return static_cast<uint16_t>(color.r) + color.g + color.b;
}

uint16_t Energy(const modules::RgbwChannels channels) {
    return static_cast<uint16_t>(channels.r) + channels.g + channels.b +
           channels.w;
}

bool SameRgb(const modules::Rgb left, const modules::Rgb right) {
    return left.r == right.r && left.g == right.g && left.b == right.b;
}

bool SameRgbw(const modules::RgbwChannels left,
              const modules::RgbwChannels right) {
    return left.r == right.r && left.g == right.g && left.b == right.b &&
           left.w == right.w;
}

modules::Rgb LedAt(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& frame,
    const uint8_t square,
    const hardware::SquareLedSlot slot) {
    return frame[hardware::LedIndexForSquare(square, slot)];
}

runtime::InteractionModelSnapshot MakePromptSnapshot(
    const uint32_t promptElapsedMs,
    const uint64_t occupiedMask =
        INITIAL_OCCUPANCY & ~(1ULL << WHITE_PAWN_E2)) {
    runtime::InteractionModelSnapshot snapshot;
    snapshot.board.expectedOccupiedMask = INITIAL_OCCUPANCY;
    snapshot.board.physicalOccupiedMask = occupiedMask;
    snapshot.board.moverOccupiedMask = 0xFFFF000000000000ULL;
    snapshot.board.opponentOccupiedMask = 0x000000000000FFFFULL;
    snapshot.board.enginePiecesValid = true;

    static constexpr char BLACK_BACK_RANK[8] = {
        'r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'};
    static constexpr char WHITE_BACK_RANK[8] = {
        'R', 'N', 'B', 'Q', 'K', 'B', 'N', 'R'};
    for (uint8_t file = 0U; file < 8U; ++file) {
        snapshot.board.enginePieces[file] = BLACK_BACK_RANK[file];
        snapshot.board.enginePieces[static_cast<uint8_t>(8U + file)] = 'p';
        snapshot.board.enginePieces[static_cast<uint8_t>(48U + file)] = 'P';
        snapshot.board.enginePieces[static_cast<uint8_t>(56U + file)] =
            WHITE_BACK_RANK[file];
    }

    snapshot.interaction.taggedState.tag = runtime::ChessInteractionPhase::Idle;
    snapshot.interaction.placementModeActive = true;
    snapshot.interaction.placementMissingMask = 1ULL << WHITE_PAWN_E2;
    snapshot.game.variant = modules::GameVariant::Undecided;
    snapshot.game.setupLifecycle = runtime::SetupLifecycle::FreshSetup;
    snapshot.game.sideToMove = modules::SideToMove::White;
    snapshot.render.initialKingPlacementPromptActive = true;
    snapshot.render.initialKingPlacementPromptMask =
        (1ULL << BLACK_KING) | (1ULL << WHITE_KING);
    snapshot.render.initialKingPlacementPromptElapsedMs = promptElapsedMs;
    snapshot.system.nowMs = promptElapsedMs;
    return snapshot;
}

void Compose(
    const fxe::SettingsStore& settings,
    const runtime::InteractionModelSnapshot& snapshot,
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& board,
    std::array<modules::Rgb, modules::EDGE_LED_COUNT>& status,
    const bool firstPlayerIsP2 = false) {
    modules::led_frame_composer::ComposeInputs inputs;
    inputs.settings = &settings;
    inputs.interactionSnapshot = &snapshot;
    inputs.firstPlayerIsP2 = firstPlayerIsP2;
    std::array<modules::Rgb, 64> grid{};
    modules::led_frame_composer::Compose(inputs, board, status, grid);
}

class S3RgbwCaptureRenderer final : public modules::ILedRenderer {
  public:
    uint16_t GetLedCount() const override {
        return hardware::TOTAL_LED_COUNT;
    }

    void Clear() override {
        for (auto& led : m_leds) {
            led = {};
        }
    }

    void SetLedRgb(const uint16_t ledIndex,
                   const uint8_t r,
                   const uint8_t g,
                   const uint8_t b) override {
        if (ledIndex >= m_leds.size()) {
            return;
        }

        m_leds[ledIndex] = modules::AutoMapRgbToRgbw(
            modules::Rgb{r, g, b}, RGBW_COOL_TO_WARM_THRESHOLD);
    }

    void Show() override {}

    const modules::RgbwChannels& Led(const uint16_t ledIndex) const {
        return m_leds[ledIndex];
    }

  private:
    std::array<modules::RgbwChannels, hardware::TOTAL_LED_COUNT> m_leds = {};
};

modules::BudgetAwareLedPipelineStats RenderThroughS3Pipeline(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& frame,
    const uint16_t brightnessScale256,
    S3RgbwCaptureRenderer& outRenderer,
    const uint16_t maxLedCurrentMa = 0U) {
    const modules::LedPowerEnvelope envelope =
        modules::SelectS3LedPowerEnvelope(true);
    modules::PowerBudgetPolicyConfig policyCfg;
    policyCfg.maxLedCurrentMa = envelope.maxLedCurrentMa;
    policyCfg.maxTotalCurrentMa = envelope.maxTotalCurrentMa;
    policyCfg.fixedSystemCurrentMa = envelope.fixedSystemCurrentMa;
    if (maxLedCurrentMa != 0U) {
        policyCfg.maxLedCurrentMa = maxLedCurrentMa;
        policyCfg.maxTotalCurrentMa = static_cast<uint16_t>(
            policyCfg.fixedSystemCurrentMa + maxLedCurrentMa);
    }
    modules::PowerBudgetPolicy policy(policyCfg);

    modules::BudgetAwareLedPipelineConfig pipelineCfg;
    pipelineCfg.brightnessScale01 =
        static_cast<float>(brightnessScale256) / 256.0f;
    pipelineCfg.enableRgbwAutoMapping = true;
    pipelineCfg.rgbwCoolToWarmThreshold = RGBW_COOL_TO_WARM_THRESHOLD;
    pipelineCfg.redChannelMaxCurrentMaTimes100 =
        modules::S3_RED_CURRENT_MA_TIMES100;
    pipelineCfg.greenChannelMaxCurrentMaTimes100 =
        modules::S3_GREEN_CURRENT_MA_TIMES100;
    pipelineCfg.blueChannelMaxCurrentMaTimes100 =
        modules::S3_BLUE_CURRENT_MA_TIMES100;
    pipelineCfg.whiteChannelMaxCurrentMaTimes100 =
        modules::S3_WHITE_CURRENT_MA_TIMES100;

    modules::BudgetAwareLedPipeline pipeline(
        outRenderer, policy, pipelineCfg);
    pipeline.RenderFrame(frame.data(), frame.size());
    return pipeline.GetLastStats();
}

void CheckPromptShape(
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& board,
    const bool flashOn,
    const char* label) {
    const auto checkKing = [&](const uint8_t king,
                               const uint8_t neighbor) {
        const modules::Rgb side0 = LedAt(
            board, king, hardware::SquareLedSlot::Square0);
        const modules::Rgb side2 = LedAt(
            board, king, hardware::SquareLedSlot::Square2);
        const modules::Rgb center = LedAt(
            board, king, hardware::SquareLedSlot::Piece);
        const modules::Rgb neighbor0 = LedAt(
            board, neighbor, hardware::SquareLedSlot::Square0);
        const modules::Rgb neighbor2 = LedAt(
            board, neighbor, hardware::SquareLedSlot::Square2);
        Check(SameRgb(side0, side2) && Energy(side0) != 0U, label);
        Check(SameRgb(neighbor0, neighbor2), label);
        Check(Energy(side0) > Energy(neighbor0), label);
        Check(flashOn ? Energy(side0) > Energy(center)
                      : Energy(side0) < Energy(center),
              flashOn ? "flash bracket exceeds the steady occupied center"
                      : "lower rest bracket sits below the steady occupied center");
    };
    checkKing(WHITE_KING, WHITE_QUEEN);
    checkKing(BLACK_KING, BLACK_QUEEN);
}

void CheckPipelineCodes(
    const S3RgbwCaptureRenderer& renderer,
    const bool flashOn,
    const char* label,
    const bool requireSeparation = true) {
    const auto checkKing = [&](const uint8_t king,
                               const uint8_t neighbor) {
        const modules::RgbwChannels side0 = renderer.Led(
            hardware::LedIndexForSquare(
                king, hardware::SquareLedSlot::Square0));
        const modules::RgbwChannels side2 = renderer.Led(
            hardware::LedIndexForSquare(
                king, hardware::SquareLedSlot::Square2));
        const modules::RgbwChannels center = renderer.Led(
            hardware::LedIndexForSquare(
                king, hardware::SquareLedSlot::Piece));
        const modules::RgbwChannels neighbor0 = renderer.Led(
            hardware::LedIndexForSquare(
                neighbor, hardware::SquareLedSlot::Square0));
        if (!SameRgbw(side0, side2) || Energy(side0) == 0U) {
            std::printf(
                "[FAIL] %s: king=%u paired=%u energy=%u/%u\n",
                label,
                king,
                SameRgbw(side0, side2) ? 1U : 0U,
                Energy(side0),
                Energy(side2));
            ++g_failures;
        }
        if (requireSeparation) {
            if (Energy(side0) <= Energy(neighbor0)) {
                std::printf(
                    "[FAIL] %s: king=%u side=%u neighbor=%u\n",
                    label,
                    king,
                    Energy(side0),
                    Energy(neighbor0));
                ++g_failures;
            }
            if (flashOn) {
                if (Energy(side0) <= Energy(center)) {
                    std::printf(
                        "[FAIL] %s: king=%u flash=%u center=%u\n",
                        label,
                        king,
                        Energy(side0),
                        Energy(center));
                    ++g_failures;
                }
            } else {
                if (Energy(center) < Energy(side0)) {
                    std::printf(
                        "[FAIL] %s: king=%u rest=%u center=%u\n",
                        label,
                        king,
                        Energy(side0),
                        Energy(center));
                    ++g_failures;
                }
            }
        }
    };
    checkKing(WHITE_KING, WHITE_QUEEN);
    checkKing(BLACK_KING, BLACK_QUEEN);
}

} // namespace

int main() {
    std::puts("[INITIAL_KING_PROMPT_SALIENCE_SWEEPS] start");

    static constexpr const char* SETTINGS_PATH =
        "build/test_initial_king_prompt_salience_sweeps.json";
    (void)std::remove(SETTINGS_PATH);
    modules::SettingsStoreConfig settingsConfig;
    settingsConfig.path = SETTINGS_PATH;
    settingsConfig.deferredSaveDelayMs = 999999U;
    modules::SettingsStore settings(settingsConfig);
    Check(settings.Load(), "settings load");

    struct PaletteCase {
        uint8_t p1Wheel;
        uint8_t p2Wheel;
    };

    const std::array<PaletteCase, 2> palettes = {{
        {0U, 128U},
        {12U, 28U},
    }};

    for (const PaletteCase palette : palettes) {
        settings.Doc()["P1_PIECE_WHEEL"] = palette.p1Wheel;
        settings.Doc()["P2_PIECE_WHEEL"] = palette.p2Wheel;
        settings.Doc()["BOARD_ROTATE"] = false;
        settings.Doc()["GAME_FIRST_PLAYER"] = 1U;
        settings.SaveNow();

        static constexpr std::array<uint32_t, 3> PHASES{
            0U, 200U, 1600U};
        std::array<
            std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>,
            PHASES.size()> phaseBoards{};
        for (size_t phaseIndex = 0U; phaseIndex < PHASES.size();
             ++phaseIndex) {
            const uint32_t promptElapsedMs = PHASES[phaseIndex];
            const bool flashOn = promptElapsedMs != 200U;
            auto& board = phaseBoards[phaseIndex];
            std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
            Compose(settings, MakePromptSnapshot(promptElapsedMs), board, status);
            CheckPromptShape(
                board,
                flashOn,
                "composed cue keeps paired sides, neighbor separation, and phase contrast");

            // Include the shipped level-1 scale and the pipeline's hard floor.
            // The paired bracket must survive ordinary quantization, and every
            // full flash must remain distinct from its neighbor. Exact
            // separation during the deliberately dim between-flash phase is a
            // revision-specific dark-room HIL result because adjacent codes
            // may legitimately collapse to the same low-level value.
            for (const uint16_t brightnessScale256 :
                 {230U, 64U, 40U, 32U, 17U, 12U}) {
                S3RgbwCaptureRenderer renderer;
                const modules::BudgetAwareLedPipelineStats stats =
                    RenderThroughS3Pipeline(
                        board, brightnessScale256, renderer);
                Check(stats.appliedLedCurrentMa <=
                          stats.decision.allowedLedCurrentMa,
                      "post-budget current stays within the policy allowance");
                CheckPipelineCodes(
                    renderer,
                    flashOn,
                    "final S3 RGBW codes retain both king side LEDs at low output",
                    flashOn);
            }

            S3RgbwCaptureRenderer limitedRenderer;
            const modules::BudgetAwareLedPipelineStats limitedStats =
                RenderThroughS3Pipeline(
                    board, 12U, limitedRenderer, 3U);
            Check(limitedStats.estimatedLedCurrentMa >
                      limitedStats.decision.allowedLedCurrentMa &&
                      limitedStats.appliedLedCurrentMa <= 3U,
                  "tight S3 envelope exercises and respects power limiting");
            // This deliberately unsupported 3 mA cap proves safety, not human
            // visibility. Quantization is allowed to collapse individual LEDs.
        }

        for (const uint8_t king :
             std::array<uint8_t, 2>{WHITE_KING, BLACK_KING}) {
            Check(SameRgb(
                      LedAt(phaseBoards[0], king,
                            hardware::SquareLedSlot::Piece),
                      LedAt(phaseBoards[1], king,
                            hardware::SquareLedSlot::Piece)) &&
                      SameRgb(
                          LedAt(phaseBoards[1], king,
                                hardware::SquareLedSlot::Piece),
                          LedAt(phaseBoards[2], king,
                                hardware::SquareLedSlot::Piece)),
                  "semantic king center stays steady through full/rest/repeat");
            Check(!SameRgb(
                      LedAt(phaseBoards[0], king,
                            hardware::SquareLedSlot::Square0),
                      LedAt(phaseBoards[1], king,
                            hardware::SquareLedSlot::Square0)) &&
                      SameRgb(
                          LedAt(phaseBoards[0], king,
                                hardware::SquareLedSlot::Square0),
                          LedAt(phaseBoards[2], king,
                                hardware::SquareLedSlot::Square0)),
                  "semantic king pair changes at rest and repeats at 1600 ms");
        }
    }

    if (g_failures == 0) {
        std::puts("[INITIAL_KING_PROMPT_SALIENCE_SWEEPS] ok");
        return 0;
    }

    std::printf("[INITIAL_KING_PROMPT_SALIENCE_SWEEPS] failures=%d\n",
                g_failures);
    return 1;
}
