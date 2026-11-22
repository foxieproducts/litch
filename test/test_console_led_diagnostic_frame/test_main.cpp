#include <array>
#include <cstdio>

#include "firmware/console_led_diagnostic_frame.hpp"
#include "hardware/led_layout.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/led_power_envelope.hpp"
#include "modules/rgbw_auto_map.hpp"

namespace {

int g_failures = 0;

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
        m_leds[ledIndex] = modules::AutoMapRgbToRgbw(
            modules::Rgb{r, g, b}, 32U);
    }

    void Show() override {}

    uint16_t GetCurrentMa() const {
        uint32_t totalMaTimes100 = 0U;
        for (const auto& channels : m_leds) {
            totalMaTimes100 += EstimateChannelCurrentMaTimes100(
                channels.r, modules::S3_RED_CURRENT_MA_TIMES100);
            totalMaTimes100 += EstimateChannelCurrentMaTimes100(
                channels.g, modules::S3_GREEN_CURRENT_MA_TIMES100);
            totalMaTimes100 += EstimateChannelCurrentMaTimes100(
                channels.b, modules::S3_BLUE_CURRENT_MA_TIMES100);
            totalMaTimes100 += EstimateChannelCurrentMaTimes100(
                channels.w, modules::S3_WHITE_CURRENT_MA_TIMES100);
        }
        return static_cast<uint16_t>((totalMaTimes100 + 99U) / 100U);
    }

  private:
    std::array<modules::RgbwChannels, hardware::TOTAL_LED_COUNT> m_leds{};
};

} // namespace

int main() {
    std::puts("[CONSOLE_LED_DIAGNOSTIC_FRAME] start");

    firmware::ConsoleLedDiagnosticFrame diagnostic;
    Check(EqualRgb(diagnostic.GetFrame().front(), modules::RGB_BLACK),
          "diagnostic frame starts cleared");
    Check(diagnostic.SetBoardLed(0U, modules::RGB_RED),
          "first board LED is accepted");
    Check(diagnostic.SetBoardLed(
              static_cast<uint16_t>(hardware::BOARD_LED_COUNT - 1U),
              modules::RGB_BLUE),
          "final board LED is accepted");
    Check(!diagnostic.SetBoardLed(hardware::BOARD_LED_COUNT, modules::RGB_WHITE),
          "edge LED is outside the board diagnostic surface");
    Check(EqualRgb(diagnostic.GetFrame()[hardware::FirstEdgeLedIndex()],
                   modules::RGB_BLACK),
          "rejected board index cannot mutate the first edge LED");
    Check(EqualRgb(diagnostic.GetFrame().front(), modules::RGB_RED),
          "separate setled calls accumulate in the diagnostic frame");
    Check(EqualRgb(
              diagnostic.GetFrame()[hardware::BOARD_LED_COUNT - 1U],
              modules::RGB_BLUE),
          "accumulation preserves the final board LED");
    diagnostic.Clear();
    Check(EqualRgb(diagnostic.GetFrame().front(), modules::RGB_BLACK) &&
              EqualRgb(
                  diagnostic.GetFrame()[hardware::BOARD_LED_COUNT - 1U],
                  modules::RGB_BLACK),
          "clear discards the accumulated diagnostic frame");

    const modules::LedPowerEnvelope envelope =
        modules::SelectS3LedPowerEnvelope(true);
    modules::PowerBudgetPolicyConfig policyConfig;
    policyConfig.maxLedCurrentMa = envelope.maxLedCurrentMa;
    policyConfig.maxTotalCurrentMa = envelope.maxTotalCurrentMa;
    policyConfig.fixedSystemCurrentMa = envelope.fixedSystemCurrentMa;
    modules::PowerBudgetPolicy policy(policyConfig);
    S3RgbwMeasuringRenderer renderer;

    modules::BudgetAwareLedPipelineConfig pipelineConfig;
    pipelineConfig.brightnessScale01 = 1.0f;
    pipelineConfig.enableRgbwAutoMapping = true;
    pipelineConfig.rgbwCoolToWarmThreshold = 32U;
    pipelineConfig.redChannelMaxCurrentMaTimes100 =
        modules::S3_RED_CURRENT_MA_TIMES100;
    pipelineConfig.greenChannelMaxCurrentMaTimes100 =
        modules::S3_GREEN_CURRENT_MA_TIMES100;
    pipelineConfig.blueChannelMaxCurrentMaTimes100 =
        modules::S3_BLUE_CURRENT_MA_TIMES100;
    pipelineConfig.whiteChannelMaxCurrentMaTimes100 =
        modules::S3_WHITE_CURRENT_MA_TIMES100;
    modules::BudgetAwareLedPipeline pipeline(renderer, policy, pipelineConfig);

    const uint16_t conservativeLedHeadroomMa = static_cast<uint16_t>(
        envelope.maxTotalCurrentMa - envelope.fixedSystemCurrentMa);
    for (uint16_t index = 0U; index < hardware::BOARD_LED_COUNT; ++index) {
        Check(diagnostic.SetBoardLed(index, modules::RGB_WHITE),
              "batched board LED update is accepted");
        pipeline.RenderFrame(diagnostic.GetFrame().data(),
                             diagnostic.GetFrame().size());

        const modules::BudgetAwareLedPipelineStats stats =
            pipeline.GetLastStats();
        const uint16_t measuredCurrentMa = renderer.GetCurrentMa();
        Check(stats.appliedLedCurrentMa == measuredCurrentMa,
              "batch step stats match post-RGBW renderer current");
        CheckLessThanOrEqual(measuredCurrentMa,
                             stats.decision.allowedLedCurrentMa,
                             "every batch step stays inside its LED allowance");
        CheckLessThanOrEqual(measuredCurrentMa,
                             conservativeLedHeadroomMa,
                             "every batch step stays inside conservative headroom");
        CheckLessThanOrEqual(
            static_cast<uint16_t>(envelope.fixedSystemCurrentMa +
                                  measuredCurrentMa),
            envelope.maxTotalCurrentMa,
            "every batch step stays inside the total source envelope");
    }

    const modules::BudgetAwareLedPipelineStats finalStats =
        pipeline.GetLastStats();
    Check(finalStats.estimatedLedCurrentMa > conservativeLedHeadroomMa,
          "full white setled batch exercises active limiting");
    Check(finalStats.appliedLedCurrentMa < finalStats.estimatedLedCurrentMa,
          "full white setled batch is reduced rather than emitted raw");

    std::puts(g_failures == 0 ? "[CONSOLE_LED_DIAGNOSTIC_FRAME] PASS"
                              : "[CONSOLE_LED_DIAGNOSTIC_FRAME] FAIL");
    return g_failures == 0 ? 0 : 1;
}
