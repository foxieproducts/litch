#include <array>
#include <cstdio>

#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/led_power_envelope.hpp"
#include "modules/rgbw_auto_map.hpp"

namespace {

int g_failures = 0;

constexpr size_t S3_LED_COUNT = 224U;
constexpr uint8_t RGBW_COOL_TO_WARM_THRESHOLD = 32U;
constexpr uint16_t RED_CURRENT_MA_TIMES100 =
    modules::S3_RED_CURRENT_MA_TIMES100;
constexpr uint16_t GREEN_CURRENT_MA_TIMES100 =
    modules::S3_GREEN_CURRENT_MA_TIMES100;
constexpr uint16_t BLUE_CURRENT_MA_TIMES100 =
    modules::S3_BLUE_CURRENT_MA_TIMES100;
constexpr uint16_t WHITE_CURRENT_MA_TIMES100 =
    modules::S3_WHITE_CURRENT_MA_TIMES100;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckLessThanOrEqual(const uint16_t actual, const uint16_t limit, const char* msg) {
    if (actual <= limit) {
        return;
    }

    std::printf("[FAIL] %s (actual=%u limit=%u)\n", msg, static_cast<unsigned>(actual),
                static_cast<unsigned>(limit));
    ++g_failures;
}

uint32_t EstimateChannelCurrentMaTimes100(const uint8_t level,
                                          const uint16_t fullScaleCurrentMaTimes100) {
    return (static_cast<uint32_t>(level) * fullScaleCurrentMaTimes100 + 127U) / 255U;
}

class FakeLedRenderer final : public modules::ILedRenderer {
  public:
    FakeLedRenderer() = default;

    uint16_t GetLedCount() const override { return static_cast<uint16_t>(m_leds.size()); }

    void Clear() override {
        for (auto& c : m_leds) {
            c = modules::RGB_BLACK;
        }
    }

    void SetLedRgb(const uint16_t ledIndex, const uint8_t r, const uint8_t g, const uint8_t b) override {
        if (ledIndex >= m_leds.size()) {
            return;
        }
        m_leds[ledIndex] = modules::Rgb{r, g, b};
    }

    void Show() override { ++m_showCount; }

    const std::array<modules::Rgb, 2>& Get() const { return m_leds; }
    int GetShowCount() const { return m_showCount; }

  private:
    std::array<modules::Rgb, 2> m_leds = {modules::RGB_BLACK, modules::RGB_BLACK};
    int m_showCount = 0;
};

class S3RgbwMeasuringRenderer final : public modules::ILedRenderer {
  public:
    uint16_t GetLedCount() const override { return static_cast<uint16_t>(m_leds.size()); }

    void Clear() override {
        for (auto& channels : m_leds) {
            channels = {};
        }
    }

    void SetLedRgb(const uint16_t ledIndex, const uint8_t r, const uint8_t g, const uint8_t b) override {
        if (ledIndex >= m_leds.size()) {
            return;
        }
        m_leds[ledIndex] = modules::AutoMapRgbToRgbw(
            modules::Rgb{r, g, b}, RGBW_COOL_TO_WARM_THRESHOLD);
    }

    void Show() override { ++m_showCount; }

    const modules::RgbwChannels& GetLed(const size_t ledIndex) const {
        return m_leds[ledIndex];
    }

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
    std::array<modules::RgbwChannels, S3_LED_COUNT> m_leds = {};
    int m_showCount = 0;
};

std::array<modules::Rgb, S3_LED_COUNT> MakeSolidS3Frame(const modules::Rgb color) {
    std::array<modules::Rgb, S3_LED_COUNT> frame = {};
    frame.fill(color);
    return frame;
}

} // namespace

int main() {
    std::puts("[BUDGET_AWARE_LED_PIPELINE] start");

    {
        modules::PowerBudgetPolicyConfig cfg;
        cfg.maxLedCurrentMa = 1000U;
        cfg.maxTotalCurrentMa = 1000U;
        cfg.hysteresisMa = 0U;
        modules::PowerBudgetPolicy policy(cfg);
        FakeLedRenderer renderer;
        modules::BudgetAwareLedPipeline pipeline(renderer, policy);
        pipeline.SetBrightnessScale01(0.5f);
        const std::array<modules::Rgb, 2> frame{modules::Rgb{255, 0, 0}, modules::Rgb{0, 0, 64}};
        const std::array<uint8_t, 2> mask{1U, 0U};
        pipeline.RenderFrame(frame.data(), frame.size());
        const auto baseline = renderer.Get();
        pipeline.RenderFrame(frame.data(), frame.size(), {mask.data(), mask.size(), 384U});
        std::printf("[ACCENT] baseline=%u boosted=%u\n", baseline[0].r, renderer.Get()[0].r);
        Check(renderer.Get()[0].r == 191U, "saturated palette gains150% after ambient brightness, before genuine cap");
        Check(renderer.Get()[1].b == baseline[1].b, "unmarked cue retains exact normal output");
        pipeline.RenderFrame(frame.data(), frame.size(), {mask.data(), mask.size(), 256U});
        Check(renderer.Get()[0].r == baseline[0].r && renderer.Get()[1].b == baseline[1].b,
              "default accent factor is bit-identical to baseline");
    }

    {
        modules::PowerBudgetPolicyConfig cfg;
        cfg.maxLedCurrentMa = 60; // tight cap
        cfg.maxTotalCurrentMa = 60;
        cfg.allowUsbPower = false;
        cfg.hysteresisMa = 0;

        modules::PowerBudgetPolicy policy(cfg);
        FakeLedRenderer renderer;

        modules::BudgetAwareLedPipelineConfig pcfg;
        pcfg.channelMaxCurrentMa = 20;
        pcfg.brightnessScale01 = 1.0f;

        modules::BudgetAwareLedPipeline pipe(renderer, policy, pcfg);

        const std::array<modules::Rgb, 2> frame = {modules::RGB_WHITE, modules::RGB_WHITE};
        pipe.RenderFrame(frame.data(), frame.size());

        const auto out = renderer.Get();
        Check(out.size() == 2, "renderer has 2 LEDs");
        Check(renderer.GetShowCount() == 1, "Show called");

        Check(out[0].r >= 120 && out[0].r <= 136, "scaled r approx half");
        Check(out[0].g >= 120 && out[0].g <= 136, "scaled g approx half");
        Check(out[0].b >= 120 && out[0].b <= 136, "scaled b approx half");

        const auto stats = pipe.GetLastStats();
        Check(stats.estimatedLedCurrentMa == 120, "legacy estimated current matches model");
        Check(stats.decision.allowedLedCurrentMa == 60, "allowed current clamps to cap");
    }

    {
        modules::PowerBudgetPolicyConfig cfg;
        cfg.maxLedCurrentMa = 1000;
        cfg.maxTotalCurrentMa = 1000;
        cfg.allowUsbPower = false;
        cfg.hysteresisMa = 0;

        modules::PowerBudgetPolicy policy(cfg);
        FakeLedRenderer renderer;

        modules::BudgetAwareLedPipelineConfig pcfg;
        pcfg.channelMaxCurrentMa = 6;
        pcfg.enableRgbwAutoMapping = true;
        pcfg.redChannelMaxCurrentMaTimes100 = 507U;
        pcfg.greenChannelMaxCurrentMaTimes100 = 513U;
        pcfg.blueChannelMaxCurrentMaTimes100 = 515U;
        pcfg.whiteChannelMaxCurrentMaTimes100 = 1222U;

        modules::BudgetAwareLedPipeline pipe(renderer, policy, pcfg);

        const std::array<modules::Rgb, 2> frame = {modules::RGB_WHITE, modules::RGB_BLACK};
        pipe.RenderFrame(frame.data(), frame.size());

        const auto out = renderer.Get();
        Check(out[0].r == 255 && out[0].g == 255 && out[0].b == 255, "measured model keeps full brightness when under cap");
        Check(out[1].r == 0 && out[1].g == 0 && out[1].b == 0, "unused LED stays black");

        const auto stats = pipe.GetLastStats();
        Check(stats.estimatedLedCurrentMa == 28, "measured RGBW estimate matches calibrated full-white draw");
        Check(stats.decision.allowedLedCurrentMa == 28, "allowed current tracks calibrated estimate when under cap");
    }

    {
        modules::PowerBudgetPolicyConfig cfg;
        const modules::LedPowerEnvelope envelope =
            modules::SelectS3LedPowerEnvelope(true);
        cfg.maxLedCurrentMa = envelope.maxLedCurrentMa;
        cfg.maxTotalCurrentMa = envelope.maxTotalCurrentMa;
        cfg.fixedSystemCurrentMa = envelope.fixedSystemCurrentMa;

        modules::PowerBudgetPolicy policy(cfg);
        S3RgbwMeasuringRenderer renderer;

        modules::BudgetAwareLedPipelineConfig pipelineCfg;
        pipelineCfg.brightnessScale01 = 230.0f / 256.0f;
        pipelineCfg.enableRgbwAutoMapping = true;
        pipelineCfg.rgbwCoolToWarmThreshold = RGBW_COOL_TO_WARM_THRESHOLD;
        pipelineCfg.redChannelMaxCurrentMaTimes100 = RED_CURRENT_MA_TIMES100;
        pipelineCfg.greenChannelMaxCurrentMaTimes100 = GREEN_CURRENT_MA_TIMES100;
        pipelineCfg.blueChannelMaxCurrentMaTimes100 = BLUE_CURRENT_MA_TIMES100;
        pipelineCfg.whiteChannelMaxCurrentMaTimes100 = WHITE_CURRENT_MA_TIMES100;

        modules::BudgetAwareLedPipeline pipeline(renderer, policy, pipelineCfg);
        const auto frame = MakeSolidS3Frame(modules::Rgb{184U, 184U, 188U});
        pipeline.RenderFrame(frame.data(), frame.size());

        const auto stats = pipeline.GetLastStats();
        Check(stats.estimatedLedCurrentMa == 1792U,
              "production-like near-neutral request uses calibrated RGBW estimate");
        Check(stats.decision.allowedLedCurrentMa == 216U,
              "fixed board load leaves 216mA for LEDs");
        Check(stats.appliedLedCurrentMa == 211U,
              "nearest safe near-neutral frame applies 211mA");
        Check(stats.appliedLedCurrentMa == renderer.GetCurrentMa(),
              "stats report the final post-mapping LED current");
        CheckLessThanOrEqual(renderer.GetCurrentMa(), stats.decision.allowedLedCurrentMa,
                             "post-render RGBW current stays within policy allowance");
        Check(renderer.GetLed(0U).w >= 18U,
              "near-neutral limiting preserves the nearest safe quantized frame");
        CheckLessThanOrEqual(static_cast<uint16_t>(
                                 envelope.fixedSystemCurrentMa +
                                 renderer.GetCurrentMa()),
                             envelope.maxTotalCurrentMa,
                             "fixed board load plus LEDs stays within source envelope");

        const std::array<modules::Rgb, 10> stressColors = {
            modules::RGB_WHITE,
            modules::Rgb{184U, 184U, 185U},
            modules::Rgb{185U, 184U, 184U},
            modules::Rgb{184U, 185U, 184U},
            modules::Rgb{96U, 96U, 100U},
            modules::Rgb{100U, 96U, 96U},
            modules::Rgb{96U, 100U, 96U},
            modules::Rgb{255U, 0U, 0U},
            modules::Rgb{0U, 255U, 0U},
            modules::Rgb{0U, 0U, 255U},
        };
        for (const modules::Rgb color : stressColors) {
            const auto stressFrame = MakeSolidS3Frame(color);
            pipeline.RenderFrame(stressFrame.data(), stressFrame.size());

            const auto normalCue = renderer.GetLed(S3_LED_COUNT - 1U);
            std::array<uint8_t, S3_LED_COUNT> accentMask{};
            for (size_t i = 0U; i < S3_LED_COUNT / 2U; ++i) { accentMask[i] = 1U; }
            for (uint16_t gain = 0U; gain <= modules::LedFrameAccent::MAX_SCALE256; gain += 8U) {
                pipeline.RenderFrame(stressFrame.data(), stressFrame.size(),
                                     {accentMask.data(), accentMask.size(), gain});
                const auto cue = renderer.GetLed(S3_LED_COUNT - 1U);
                Check(cue.r == normalCue.r && cue.g == normalCue.g && cue.b == normalCue.b && cue.w == normalCue.w,
                      "every accent step preserves exact required cue under tight RGBW cap");
                Check(pipeline.GetLastStats().appliedLedCurrentMa == renderer.GetCurrentMa(),
                      "accent estimator matches actual rounded RGBW output");
                Check(renderer.GetCurrentMa() <= pipeline.GetLastStats().decision.allowedLedCurrentMa,
                      "no accent step crosses current cap, including RGBW threshold changes");
            }
            pipeline.RenderFrame(stressFrame.data(), stressFrame.size());
            const auto stressStats = pipeline.GetLastStats();
            const uint16_t measuredCurrentMa = renderer.GetCurrentMa();
            Check(stressStats.appliedLedCurrentMa == measuredCurrentMa,
                  "stats match measured RGBW current across stress colors");
            CheckLessThanOrEqual(measuredCurrentMa,
                                 stressStats.decision.allowedLedCurrentMa,
                                 "stress color stays within LED allowance");
            CheckLessThanOrEqual(static_cast<uint16_t>(
                                     envelope.fixedSystemCurrentMa +
                                     measuredCurrentMa),
                                 envelope.maxTotalCurrentMa,
                                 "stress color stays within total source envelope");
        }
    }

    for (const bool recovery : {false, true}) {
        const auto envelope = modules::SelectS3LedPowerEnvelope(recovery);
        Check(envelope.maxTotalCurrentMa == (recovery ? modules::S3_CONSERVATIVE_TOTAL_CURRENT_MA : modules::S3_NORMAL_TOTAL_CURRENT_MA),
              "gain sweep selects intended normal or brownout envelope");
        Check(modules::S3_NORMAL_TOTAL_CURRENT_MA > modules::S3_CONSERVATIVE_TOTAL_CURRENT_MA,
              "normal and protection fixtures exercise distinct caps");
        for (const bool noHeadroom : {false, true}) {
            modules::PowerBudgetPolicyConfig cfg;
            cfg.maxLedCurrentMa = envelope.maxLedCurrentMa;
            cfg.maxTotalCurrentMa = envelope.maxTotalCurrentMa;
            cfg.fixedSystemCurrentMa = noHeadroom ? cfg.maxTotalCurrentMa : envelope.fixedSystemCurrentMa;
            modules::PowerBudgetPolicy policy(cfg);
            S3RgbwMeasuringRenderer renderer;
            modules::BudgetAwareLedPipelineConfig pcfg;
            pcfg.enableRgbwAutoMapping = true;
            pcfg.redChannelMaxCurrentMaTimes100 = RED_CURRENT_MA_TIMES100;
            pcfg.greenChannelMaxCurrentMaTimes100 = GREEN_CURRENT_MA_TIMES100;
            pcfg.blueChannelMaxCurrentMaTimes100 = BLUE_CURRENT_MA_TIMES100;
            pcfg.whiteChannelMaxCurrentMaTimes100 = WHITE_CURRENT_MA_TIMES100;
            modules::BudgetAwareLedPipeline pipeline(renderer, policy, pcfg);
            const auto frame = MakeSolidS3Frame(modules::RGB_WHITE);
            std::array<uint8_t, S3_LED_COUNT> mask{};
            for (size_t i = 0U; i < mask.size() / 2U; ++i) { mask[i] = 1U; }
            pipeline.RenderFrame(frame.data(), frame.size());
            const auto normal = renderer.GetLed(S3_LED_COUNT - 1U);
            for (uint16_t gain = 0U; gain <= modules::LedFrameAccent::MAX_SCALE256; gain += 8U) {
                pipeline.RenderFrame(frame.data(), frame.size(), {mask.data(), mask.size(), gain});
                const auto cue = renderer.GetLed(S3_LED_COUNT - 1U);
                Check(cue.r == normal.r && cue.g == normal.g && cue.b == normal.b && cue.w == normal.w,
                      "weak/strong/zero-headroom accent preserves baseline cue");
                Check(pipeline.GetLastStats().powerLimited,
                      "source-limited baseline remains reported after actual-demand registration");
                Check(renderer.GetCurrentMa() == pipeline.GetLastStats().appliedLedCurrentMa,
                      "all source envelopes estimate actual RGBW current");
                Check(static_cast<uint32_t>(renderer.GetCurrentMa()) + cfg.fixedSystemCurrentMa <= cfg.maxTotalCurrentMa,
                      "accent cannot spend nonexistent source headroom");
            }
        }
    }

    if (g_failures == 0) {
        std::puts("[BUDGET_AWARE_LED_PIPELINE] ok");
        return 0;
    }

    std::printf("[BUDGET_AWARE_LED_PIPELINE] failures=%d\n", g_failures);
    return 1;
}
