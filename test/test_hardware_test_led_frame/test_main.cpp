#include <array>
#include <cstdio>

#include "firmware/hardware_test_led_frame.hpp"
#include "hardware/led_layout.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/led_power_envelope.hpp"
#include "modules/rgbw_auto_map.hpp"
#include "modules/status_led_frame_adapter.hpp"

namespace {

int g_failures = 0;

constexpr uint16_t RED_CURRENT_MA_TIMES100 =
    modules::S3_RED_CURRENT_MA_TIMES100;
constexpr uint16_t GREEN_CURRENT_MA_TIMES100 =
    modules::S3_GREEN_CURRENT_MA_TIMES100;
constexpr uint16_t BLUE_CURRENT_MA_TIMES100 =
    modules::S3_BLUE_CURRENT_MA_TIMES100;
constexpr uint16_t WHITE_CURRENT_MA_TIMES100 =
    modules::S3_WHITE_CURRENT_MA_TIMES100;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }

    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

void CheckLessThanOrEqual(const uint16_t actual,
                          const uint16_t limit,
                          const char* message) {
    if (actual <= limit) {
        return;
    }

    std::printf("[FAIL] %s (actual=%u limit=%u)\n", message,
                static_cast<unsigned>(actual), static_cast<unsigned>(limit));
    ++g_failures;
}

bool EqualRgb(const modules::Rgb lhs, const modules::Rgb rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b;
}

uint32_t EstimateChannelCurrentMaTimes100(
    const uint8_t level,
    const uint16_t fullScaleCurrentMaTimes100) {
    return (static_cast<uint32_t>(level) * fullScaleCurrentMaTimes100 + 127U) /
           255U;
}

class S3RgbwMeasuringRenderer final : public modules::ILedRenderer {
  public:
    uint16_t GetLedCount() const override {
        return static_cast<uint16_t>(m_leds.size());
    }

    void Clear() override {
        for (auto& channels : m_leds) {
            channels = {};
        }
    }

    void SetLedRgb(const uint16_t ledIndex,
                   const uint8_t r,
                   const uint8_t g,
                   const uint8_t b) override {
        if (ledIndex >= m_leds.size()) {
            return;
        }
        m_leds[ledIndex] = modules::AutoMapRgbToRgbw(modules::Rgb{r, g, b}, 32U);
    }

    void Show() override {}

    uint16_t GetCurrentMa() const {
        uint32_t totalMaTimes100 = 0U;
        for (const auto& channels : m_leds) {
            totalMaTimes100 += EstimateChannelCurrentMaTimes100(
                channels.r, RED_CURRENT_MA_TIMES100);
            totalMaTimes100 += EstimateChannelCurrentMaTimes100(
                channels.g, GREEN_CURRENT_MA_TIMES100);
            totalMaTimes100 += EstimateChannelCurrentMaTimes100(
                channels.b, BLUE_CURRENT_MA_TIMES100);
            totalMaTimes100 += EstimateChannelCurrentMaTimes100(
                channels.w, WHITE_CURRENT_MA_TIMES100);
        }
        return static_cast<uint16_t>((totalMaTimes100 + 99U) / 100U);
    }

  private:
    std::array<modules::RgbwChannels, hardware::TOTAL_LED_COUNT> m_leds{};
};

} // namespace

int main() {
    std::puts("[HARDWARE_TEST_LED_FRAME] start");

    const modules::Rgb stimulus = firmware::WhiteChannelTestRgb(255U);
    const modules::RgbwChannels mapped = modules::AutoMapRgbToRgbw(stimulus, 32U);
    const uint16_t residualRgb = static_cast<uint16_t>(mapped.r) + mapped.g + mapped.b;
    const modules::Rgb paletteColor{34U, 195U, 226U};
    const modules::RgbwChannels litchcraftMapped =
        modules::AutoMapRgbToRgbw(paletteColor, 32U, 0U);

    Check(mapped.w >= 220U, "white test drives the mapped W channel strongly");
    Check(mapped.w > residualRgb, "white test is W-dominant after production mapping");
    Check(litchcraftMapped.r == paletteColor.r &&
              litchcraftMapped.g == paletteColor.g &&
              litchcraftMapped.b == paletteColor.b &&
              litchcraftMapped.w == 0U,
          "zero white-mix keeps Litchcraft palette RGB exact and disables W");

    firmware::HardwareTestLedFrameInputs inputs;
    inputs.step = firmware::HardwareTestMode::Step::WhiteChannel;
    inputs.nowMs = 1530U;
    firmware::HardwareTestLedFrame frame =
        firmware::ComposeHardwareTestLedFrame(inputs);
    const firmware::HardwareTestStatusFrame whiteStatusFrame =
        firmware::ComposeHardwareTestStatusFrame(inputs);
    modules::status_led_frame_adapter::ApplyToDedicatedChain(
        whiteStatusFrame, false, frame);
    Check(EqualRgb(frame.front(), stimulus),
          "white step fills the first LED with the W stimulus");
    Check(EqualRgb(frame.back(), stimulus),
          "white step fills the final LED with the W stimulus");
    Check(frame.size() == hardware::TOTAL_LED_COUNT,
          "hardware test frame covers every physical LED");

    firmware::HardwareTestLedFrameInputs hallInputs;
    hallInputs.step = firmware::HardwareTestMode::Step::HallSensors;
    hallInputs.occupiedMask = 1ULL;
    const firmware::HardwareTestLedFrame hallFrame =
        firmware::ComposeHardwareTestLedFrame(hallInputs);
    Check(EqualRgb(hallFrame[0U], modules::RGB_WHITE),
          "occupied hall square lights its first LED");
    Check(EqualRgb(hallFrame[1U], modules::RGB_WHITE),
          "occupied hall square lights its piece LED");
    Check(EqualRgb(hallFrame[2U], modules::RGB_WHITE),
          "occupied hall square lights its final LED");
    Check(EqualRgb(hallFrame[3U], modules::RGB_BLACK),
          "unoccupied hall square remains black");

    firmware::HardwareTestLedFrameInputs rainbowInputs;
    rainbowInputs.step = firmware::HardwareTestMode::Step::LedRainbow;
    const firmware::HardwareTestLedFrame rainbowFrame =
        firmware::ComposeHardwareTestLedFrame(rainbowInputs);
    Check(!EqualRgb(rainbowFrame[0U], modules::RGB_BLACK),
          "rainbow step lights the first LED");
    Check(!EqualRgb(rainbowFrame[0U], rainbowFrame[1U]),
          "rainbow step varies color across adjacent LEDs");

    firmware::HardwareTestLedFrameInputs lightInputs;
    lightInputs.step = firmware::HardwareTestMode::Step::LightSensors;
    lightInputs.nowMs = 24U;
    const firmware::HardwareTestLedFrame lightFrame =
        firmware::ComposeHardwareTestLedFrame(lightInputs);
    Check(!EqualRgb(lightFrame[0U], modules::RGB_BLACK),
          "light-sensor step keeps a visible LED diagnostic");

    firmware::HardwareTestLedFrameInputs idleButtonInputs;
    idleButtonInputs.step = firmware::HardwareTestMode::Step::Buttons;
    firmware::HardwareTestLedFrame idleButtonFrame =
        firmware::ComposeHardwareTestLedFrame(idleButtonInputs);
    const firmware::HardwareTestStatusFrame idleButtonStatusFrame =
        firmware::ComposeHardwareTestStatusFrame(idleButtonInputs);
    modules::status_led_frame_adapter::ApplyToDedicatedChain(
        idleButtonStatusFrame, false, idleButtonFrame);

    firmware::HardwareTestLedFrameInputs pressedButtonInputs = idleButtonInputs;
    pressedButtonInputs.pressedButtons[static_cast<size_t>(modules::UserButton::Up)] = true;
    firmware::HardwareTestLedFrame pressedButtonFrame =
        firmware::ComposeHardwareTestLedFrame(pressedButtonInputs);
    const firmware::HardwareTestStatusFrame pressedButtonStatusFrame =
        firmware::ComposeHardwareTestStatusFrame(pressedButtonInputs);
    modules::status_led_frame_adapter::ApplyToDedicatedChain(
        pressedButtonStatusFrame, false, pressedButtonFrame);
    const uint16_t firstEdge = hardware::FirstEdgeLedIndex();
    Check(!EqualRgb(idleButtonFrame[0U], modules::RGB_BLACK),
          "button step keeps a dim animated background");
    Check(!EqualRgb(pressedButtonFrame[firstEdge], idleButtonFrame[firstEdge]),
          "pressed button overrides its edge indicator at full test intensity");

    modules::PowerBudgetPolicyConfig policyConfig;
    const modules::LedPowerEnvelope envelope =
        modules::SelectS3LedPowerEnvelope(true);
    policyConfig.maxLedCurrentMa = envelope.maxLedCurrentMa;
    policyConfig.maxTotalCurrentMa = envelope.maxTotalCurrentMa;
    policyConfig.fixedSystemCurrentMa = envelope.fixedSystemCurrentMa;
    const uint16_t conservativeLedHeadroomMa = static_cast<uint16_t>(
        envelope.maxTotalCurrentMa - envelope.fixedSystemCurrentMa);
    modules::PowerBudgetPolicy policy(policyConfig);
    S3RgbwMeasuringRenderer renderer;

    modules::BudgetAwareLedPipelineConfig pipelineConfig;
    pipelineConfig.brightnessScale01 = 77.0f / 256.0f;
    pipelineConfig.enableRgbwAutoMapping = true;
    pipelineConfig.rgbwCoolToWarmThreshold = 32U;
    pipelineConfig.redChannelMaxCurrentMaTimes100 = RED_CURRENT_MA_TIMES100;
    pipelineConfig.greenChannelMaxCurrentMaTimes100 = GREEN_CURRENT_MA_TIMES100;
    pipelineConfig.blueChannelMaxCurrentMaTimes100 = BLUE_CURRENT_MA_TIMES100;
    pipelineConfig.whiteChannelMaxCurrentMaTimes100 = WHITE_CURRENT_MA_TIMES100;
    modules::BudgetAwareLedPipeline pipeline(renderer, policy, pipelineConfig);

    constexpr std::array<firmware::HardwareTestMode::Step, 5> STEPS = {
        firmware::HardwareTestMode::Step::LedRainbow,
        firmware::HardwareTestMode::Step::Buttons,
        firmware::HardwareTestMode::Step::HallSensors,
        firmware::HardwareTestMode::Step::LightSensors,
        firmware::HardwareTestMode::Step::WhiteChannel,
    };
    for (const firmware::HardwareTestMode::Step step : STEPS) {
        firmware::HardwareTestLedFrameInputs worstCaseInputs;
        worstCaseInputs.step = step;
        worstCaseInputs.nowMs = 1530U;
        worstCaseInputs.occupiedMask = UINT64_MAX;
        worstCaseInputs.pressedButtons.fill(true);
        firmware::HardwareTestLedFrame worstCaseFrame =
            firmware::ComposeHardwareTestLedFrame(worstCaseInputs);
        const firmware::HardwareTestStatusFrame worstCaseStatusFrame =
            firmware::ComposeHardwareTestStatusFrame(worstCaseInputs);
        modules::status_led_frame_adapter::ApplyToDedicatedChain(
            worstCaseStatusFrame, false, worstCaseFrame);
        pipeline.RenderFrame(worstCaseFrame.data(), worstCaseFrame.size());

        const modules::BudgetAwareLedPipelineStats stats = pipeline.GetLastStats();
        const uint16_t measuredCurrentMa = renderer.GetCurrentMa();
        Check(stats.appliedLedCurrentMa == measuredCurrentMa,
              "hardware-test stats match post-mapping current");
        CheckLessThanOrEqual(measuredCurrentMa, stats.decision.allowedLedCurrentMa,
                             "hardware-test step stays within LED allowance");
        CheckLessThanOrEqual(measuredCurrentMa, conservativeLedHeadroomMa,
                             "hardware-test step stays within conservative LED headroom");
        CheckLessThanOrEqual(
            static_cast<uint16_t>(envelope.fixedSystemCurrentMa +
                                  measuredCurrentMa),
            envelope.maxTotalCurrentMa,
            "hardware-test step stays within total source envelope");
    }

    if (g_failures == 0) {
        std::puts("[HARDWARE_TEST_LED_FRAME] ok");
        return 0;
    }

    std::printf("[HARDWARE_TEST_LED_FRAME] failures=%d\n", g_failures);
    return 1;
}
