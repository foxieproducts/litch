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
    std::array<modules::Rgb,300U> frame{};
    frame[0U]={255U,255U,255U}; frame[1U]={255U,255U,255U};
    frame[2U]={16U,16U,16U};frame[3U]={17U,97U,231U};
    std::array<uint8_t,300U> mask{}; mask[0U]=1U;mask[4U]=1U;
    for(const uint16_t gain:{256U,128U})for(const float brightness:{0.25f,0.90f,1.00f}){
        modules::PowerBudgetPolicyConfig policyConfig;
        policyConfig.maxLedCurrentMa=20000U;policyConfig.maxTotalCurrentMa=20000U;
        modules::PowerBudgetPolicy policy(policyConfig);CaptureRenderer renderer;
        modules::BudgetAwareLedPipelineConfig config;config.outputBrightnessScale256=gain;
        modules::BudgetAwareLedPipeline pipeline(renderer,policy,config);pipeline.SetBrightnessScale01(brightness);
        pipeline.RenderFrame(frame.data(),frame.size());const auto baseline=renderer.pixels;
        pipeline.RenderFrame(frame.data(),frame.size(),{mask.data(),mask.size(),
            modules::led_render_policy::CAPTURE_REMOVAL_ACCENT_SCALE256,256U});
        for(uint16_t i=1U;i<frame.size();++i)
            Check(renderer.pixels[i].r==baseline[i].r && renderer.pixels[i].g==baseline[i].g && renderer.pixels[i].b==baseline[i].b,
                  "capture-only headroom never changes any unmarked pixel");
        Check(renderer.pixels[0U].r==renderer.pixels[1U].r && renderer.pixels[0U].g==renderer.pixels[1U].g &&
              renderer.pixels[0U].b==renderer.pixels[1U].b,
              "WHITE removal peak equals the full target-reference emitter in the SAME frame at every brightness");
        Check(renderer.pixels[0U].r>renderer.pixels[2U].r && renderer.pixels[4U].r==0U &&
              renderer.pixels[4U].g==0U && renderer.pixels[4U].b==0U,
              "capture white is much brighter than ordinary square lighting while the inactive owned rail stays zero");
        std::printf("[CAPTURE_TARGET_PEAK] gain=%u brightness=%.2f capture=%u target=%u ordinary=%u inactive=%u\n",
              gain,brightness,renderer.pixels[0U].r,renderer.pixels[1U].r,renderer.pixels[2U].r,renderer.pixels[4U].r);
        Check(CurrentMa(renderer)==pipeline.GetLastStats().appliedLedCurrentMa,
              "capture headroom current estimate equals the actual final output");
    }
    for(const bool noHeadroom:{false,true}){
        frame.fill({255U,255U,255U});frame[4U]={};
        modules::PowerBudgetPolicyConfig pc;pc.maxLedCurrentMa=80U;pc.maxTotalCurrentMa=80U;
        pc.fixedSystemCurrentMa=noHeadroom?80U:0U;
        modules::PowerBudgetPolicy policy(pc);CaptureRenderer renderer;
        modules::BudgetAwareLedPipeline pipeline(renderer,policy);pipeline.SetBrightnessScale01(0.90f);
        pipeline.RenderFrame(frame.data(),frame.size());const auto ordinary=renderer.pixels[1U];
        pipeline.RenderFrame(frame.data(),frame.size(),{mask.data(),mask.size(),
            modules::led_render_policy::CAPTURE_REMOVAL_ACCENT_SCALE256,256U});
        Check(renderer.pixels[0U].r==renderer.pixels[1U].r && renderer.pixels[0U].g==renderer.pixels[1U].g &&
              renderer.pixels[0U].b==renderer.pixels[1U].b,
              "current-capped white removal and target-reference peaks remain equal in the SAME frame");
        Check(renderer.pixels[4U].r==0U && renderer.pixels[4U].g==0U && renderer.pixels[4U].b==0U,
              "current arbitration never relights the inactive owned removal rail");
        std::printf("[CAPTURE_TARGET_LIMIT] no_headroom=%u capture=%u target=%u inactive=%u current=%u\n",
            noHeadroom,renderer.pixels[0U].r,renderer.pixels[1U].r,renderer.pixels[4U].r,CurrentMa(renderer));
        pipeline.RenderFrame(frame.data(),frame.size(),{mask.data(),mask.size(),285U,512U});
        Check(renderer.pixels[1U].r==ordinary.r && renderer.pixels[1U].g==ordinary.g && renderer.pixels[1U].b==ordinary.b,
              "current-limited capture accent preserves unmarked baseline bytes");
        Check(uint32_t(CurrentMa(renderer))+pc.fixedSystemCurrentMa<=80U &&
                  pipeline.GetLastStats().appliedLedCurrentMa==CurrentMa(renderer),
              "capture ceiling requests cannot bypass hard80mA or zero available source current");
        if(noHeadroom)Check(renderer.pixels[0U].r==0U && renderer.pixels[0U].g==0U && renderer.pixels[0U].b==0U,
                           "zero current headroom cannot light a capture accent");
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
