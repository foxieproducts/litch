#include <array>
#include <cstdio>

#include "modules/budget_aware_led_pipeline.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckInRangeU8(const uint8_t v, const uint8_t lo, const uint8_t hi, const char* msg) {
    if (v >= lo && v <= hi) {
        return;
    }

    std::printf("[FAIL] %s (got=%u range=[%u,%u])\n", msg, v, lo, hi);
    ++g_failures;
}

class FakeLedRenderer final : public modules::ILedRenderer {
  public:
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

    const modules::Rgb& Get0() const { return m_leds[0]; }

  private:
    std::array<modules::Rgb, 1> m_leds = {modules::RGB_BLACK};
    int m_showCount = 0;
};

} // namespace

int main() {
    std::puts("[BRIGHTNESS] start");

    modules::PowerBudgetPolicyConfig policyCfg;
    policyCfg.maxLedCurrentMa = 1000;
    policyCfg.maxTotalCurrentMa = 1000;
    policyCfg.allowUsbPower = false;
    policyCfg.hysteresisMa = 0;

    modules::PowerBudgetPolicy policy(policyCfg);

    FakeLedRenderer renderer;

    modules::BudgetAwareLedPipelineConfig cfg;
    cfg.channelMaxCurrentMa = 20;

    modules::BudgetAwareLedPipeline pipe(renderer, policy, cfg);

    const std::array<modules::Rgb, 1> frame = {modules::RGB_WHITE};

    pipe.SetBrightnessScale01(-1.0f);
    pipe.RenderFrame(frame.data(), frame.size());
    {
        const modules::Rgb out = renderer.Get0();
        Check(out.r == 0 && out.g == 0 && out.b == 0, "negative brightness clamps to 0");
        const auto stats = pipe.GetLastStats();
        Check(stats.appliedBrightnessScale01 == 0.0f, "appliedBrightnessScale01 is 0 when brightness 0");
    }

    pipe.SetBrightnessScale01(2.0f);
    pipe.RenderFrame(frame.data(), frame.size());
    {
        const modules::Rgb out = renderer.Get0();
        // Allow rounding tolerance.
        CheckInRangeU8(out.r, 254, 255, "brightness>1 clamps to full (r)");
        CheckInRangeU8(out.g, 254, 255, "brightness>1 clamps to full (g)");
        CheckInRangeU8(out.b, 254, 255, "brightness>1 clamps to full (b)");
        const auto stats = pipe.GetLastStats();
        Check(stats.appliedBrightnessScale01 == 1.0f, "appliedBrightnessScale01 clamps to 1");
    }

    if (g_failures == 0) {
        std::puts("[BRIGHTNESS] ok");
        return 0;
    }

    std::printf("[BRIGHTNESS] failures=%d\n", g_failures);
    return 1;
}
