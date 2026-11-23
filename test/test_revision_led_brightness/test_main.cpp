#include <array>
#include <cmath>
#include <cstdio>

#include "hardware/board_hardware_profile.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/led_render_policy.hpp"

namespace {

int failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        if (failures < 20) { std::printf("[FAIL] %s\n", message); }
        ++failures;
    }
}

class CaptureRenderer final : public modules::ILedRenderer {
  public:
    uint16_t GetLedCount() const override { return pixels.size(); }
    void Clear() override { pixels.fill({}); }
    void SetLedRgb(uint16_t index, uint8_t r, uint8_t g, uint8_t b) override {
        pixels[index] = {r, g, b};
    }
    void Show() override {}
    std::array<modules::Rgb, 300> pixels{};
};

uint16_t CurrentMa(const CaptureRenderer& renderer) {
    uint32_t totalTimes100 = 0U;
    for (const auto color : renderer.pixels) {
        for (const uint8_t channel : {color.r, color.g, color.b}) {
            totalTimes100 += (static_cast<uint32_t>(channel) * 2000U + 127U) / 255U;
        }
    }
    return static_cast<uint16_t>((totalTimes100 + 99U) / 100U);
}

void CheckProfiles() {
    using hardware::BoardRevision;
    for (const auto revision : {BoardRevision::Unknown, BoardRevision::V1C3,
                               BoardRevision::V2S3, BoardRevision::V3S3}) {
        Check(hardware::BoardHardwareProfileFor(revision).ledOutputScale256 == 256U,
              "existing/unknown revisions retain full output gain");
    }
    Check(hardware::RuntimeProfileForS3Sample({true, true, false})
                  .resolved.ledOutputScale256 == 128U,
          "production V3.3 strap profile alone selects provisional half gain");
}

void CheckBothChainsAndAccents() {
    std::array<modules::Rgb, 300> frame{};
    std::array<uint8_t, 300> accentMask{};
    for (uint16_t i = 0U; i < frame.size(); ++i) {
        frame[i] = {static_cast<uint8_t>(i), static_cast<uint8_t>(255U - i),
                    static_cast<uint8_t>(i * 73U)};
        accentMask[i] = i % 2U;
    }
    const auto original = frame;
    for (const uint16_t brightness : {0U, 8U, 64U, 138U, 230U, 256U}) {
        for (const uint16_t accentGain : {192U, 256U, 320U}) {
            modules::PowerBudgetPolicyConfig policyConfig;
            policyConfig.maxLedCurrentMa = 20000U;
            policyConfig.maxTotalCurrentMa = 20000U;
            modules::PowerBudgetPolicy referencePolicy(policyConfig);
            modules::PowerBudgetPolicy dimmedPolicy(policyConfig);
            CaptureRenderer reference;
            CaptureRenderer dimmed;
            modules::BudgetAwareLedPipelineConfig config;
            modules::BudgetAwareLedPipeline referencePipeline(reference, referencePolicy, config);
            config.outputBrightnessScale256 = hardware::BoardHardwareProfileFor(
                hardware::BoardRevision::V3_3S3).ledOutputScale256;
            modules::BudgetAwareLedPipeline dimmedPipeline(dimmed, dimmedPolicy, config);
            const float scale = static_cast<float>(brightness) / 256.0f;
            referencePipeline.SetBrightnessScale01(scale);
            dimmedPipeline.SetBrightnessScale01(scale);
            const modules::LedFrameAccent accent{accentMask.data(), accentMask.size(), accentGain};
            referencePipeline.RenderFrame(frame.data(), frame.size(), accent);
            dimmedPipeline.RenderFrame(frame.data(), frame.size(), accent);
            for (uint16_t i = 0U; i < frame.size(); ++i) {
                const auto full = reference.pixels[i];
                const auto half = dimmed.pixels[i];
                Check(half.r == (full.r + 1U) / 2U &&
                          half.g == (full.g + 1U) / 2U &&
                          half.b == (full.b + 1U) / 2U,
                      "board/perimeter/control outputs halve final bytes, including accent ceilings");
                Check(frame[i].r == original[i].r && frame[i].g == original[i].g &&
                          frame[i].b == original[i].b,
                      "qualification gain preserves canonical frame colors");
            }
            const auto stats = dimmedPipeline.GetLastStats();
            Check(stats.estimatedLedCurrentMa == CurrentMa(dimmed) &&
                      stats.appliedLedCurrentMa == CurrentMa(dimmed),
                  "estimator and actual RGB outputs use the same qualification gain");
            Check(std::fabs(stats.appliedBrightnessScale01 -
                      referencePipeline.GetLastStats().appliedBrightnessScale01 * 0.5f) < 0.00001f,
                  "reported effective brightness includes the hardware gain");
        }
    }
}

void CheckPowerLimiting() {
    std::array<modules::Rgb, 300> frame{};
    frame.fill({255U, 255U, 255U});
    std::array<uint8_t, 300> mask{};
    mask.fill(1U);
    modules::PowerBudgetPolicyConfig policyConfig;
    policyConfig.maxLedCurrentMa = 450U;
    policyConfig.maxTotalCurrentMa = 450U;
    modules::PowerBudgetPolicy policy(policyConfig);
    CaptureRenderer renderer;
    modules::BudgetAwareLedPipelineConfig config;
    config.outputBrightnessScale256 = 128U;
    modules::BudgetAwareLedPipeline pipeline(renderer, policy, config);
    pipeline.RenderFrame(frame.data(), frame.size(), {mask.data(), mask.size(), 320U});
    Check(pipeline.GetLastStats().powerLimited && CurrentMa(renderer) <= 450U &&
              pipeline.GetLastStats().appliedLedCurrentMa == CurrentMa(renderer),
          "dimmed full-chain accents remain within the actual current ceiling");
}

void CheckCaptureTargetStrength() {
    const modules::Rgb amber{255U, 101U, 0U};
    std::array<modules::Rgb, 300> frame{};
    frame[0] = frame[1] = frame[2] = amber;
    frame[3] = {17U, 97U, 231U};
    std::array<uint8_t, 300> mask{};
    mask[0] = mask[1] = mask[4] = 1U;
    const modules::LedFrameAccent taps{mask.data(), mask.size(),
        modules::led_render_policy::CAPTURE_REMOVAL_ACCENT_SCALE256, 256U};
    const auto same = [](const modules::Rgb a, const modules::Rgb b) {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    };

    for (const uint16_t gain : {256U, 128U}) {
        for (const float brightness : {0.0f, 8.0f / 256.0f, 0.25f, 0.90f, 1.0f}) {
            modules::PowerBudgetPolicyConfig policyConfig;
            policyConfig.maxLedCurrentMa = policyConfig.maxTotalCurrentMa = 20000U;
            modules::PowerBudgetPolicy policy(policyConfig);
            CaptureRenderer renderer;
            modules::BudgetAwareLedPipelineConfig config;
            config.outputBrightnessScale256 = gain;
            modules::BudgetAwareLedPipeline pipeline(renderer, policy, config);
            pipeline.SetBrightnessScale01(brightness);
            pipeline.RenderFrame(frame.data(), frame.size());
            const auto baseline = renderer.pixels;
            pipeline.RenderFrame(frame.data(), frame.size(), taps);
            const float peak = std::fmin(brightness * 1.5f, 1.0f);
            const auto expected = [peak, gain](const uint8_t channel) {
                return (std::lround(channel * peak) * gain + 128U) / 256U;
            };
            const auto rail = renderer.pixels[0];
            Check(same(rail, renderer.pixels[1]) && rail.r == expected(amber.r) &&
                      rail.g == expected(amber.g) && rail.b == 0U,
                  "both amber rails spend spare current up to 1.5x brightness and the output ceiling");
            for (size_t i = 2U; i < frame.size(); ++i) {
                Check(same(renderer.pixels[i], baseline[i]),
                      "peak gain preserves unmarked pixels and cannot light a dark emitter");
            }
            Check(CurrentMa(renderer) == pipeline.GetLastStats().appliedLedCurrentMa,
                  "peak current estimate matches final output at both hardware gains");
        }
    }

    frame.fill({255U, 255U, 255U});
    frame[0] = frame[1] = frame[2] = amber;
    frame[4] = {};
    for (const bool noHeadroom : {false, true}) {
        modules::PowerBudgetPolicyConfig config;
        config.maxLedCurrentMa = config.maxTotalCurrentMa = 80U;
        config.fixedSystemCurrentMa = noHeadroom ? 80U : 0U;
        modules::PowerBudgetPolicy policy(config);
        CaptureRenderer renderer;
        modules::BudgetAwareLedPipeline pipeline(renderer, policy);
        pipeline.SetBrightnessScale01(0.90f);
        pipeline.RenderFrame(frame.data(), frame.size());
        const auto baseline = renderer.pixels;
        const modules::LedFrameAccent oversized{mask.data(), mask.size(), 512U, 512U};
        for (const auto& accent : {taps, oversized}) {
            pipeline.RenderFrame(frame.data(), frame.size(), accent);
            const auto rail = renderer.pixels[0];
            Check(same(rail, renderer.pixels[1]) && rail.r >= baseline[0].r &&
                      rail.g >= baseline[0].g && rail.b == 0U,
                  "the limiter preserves paired amber and the baseline when trimming a tap");
            for (size_t i = 2U; i < frame.size(); ++i) {
                Check(same(renderer.pixels[i], baseline[i]),
                      "current arbitration preserves all unrelated emitters and the dark rail");
            }
            Check(CurrentMa(renderer) + config.fixedSystemCurrentMa <= 80U &&
                      pipeline.GetLastStats().appliedLedCurrentMa == CurrentMa(renderer),
                  "actual peak output respects hard current and zero remaining source current");
            if (noHeadroom) Check(same(rail, modules::RGB_BLACK),
                                  "zero available current keeps both rails dark");
        }
    }
}

} // namespace

int main() {
    CheckProfiles();
    CheckBothChainsAndAccents();
    CheckPowerLimiting();
    CheckCaptureTargetStrength();
    std::printf("[REVISION_LED_BRIGHTNESS] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
