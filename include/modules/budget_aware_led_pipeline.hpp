#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/led_renderer.hpp"
#include "modules/power_budget_policy.hpp"
#include "modules/rgb.hpp"

namespace modules {

struct BudgetAwareLedPipelineConfig {
    // Legacy fallback current draw at full-scale channel (255). Typical WS2812
    // values are ~20mA per channel at 255. Keep configurable for determinism
    // and testing.
    uint16_t channelMaxCurrentMa = 20;

    // Global brightness scalar applied before power-budget limiting.
    // This is the "user brightness" / ambient policy output.
    float brightnessScale01 = 1.0f;

    // Hardware qualification gain, after user/ambient/accent presentation and
    // before current estimation. 256 preserves established output bytes.
    uint16_t outputBrightnessScale256 = 256U;

    // When enabled, estimate current after the same RGB->RGBW auto-mapping the
    // S3 renderer applies.
    bool enableRgbwAutoMapping = false;

    // Apply the shared S3 optical presentation transform before RGBW mapping.
    // Canonical frame values remain unchanged outside this output pipeline.
    bool enableS3OpticalColorCalibration = false;

    // Matches the renderer's cool-to-warm setting when RGBW auto-mapping is enabled.
    uint8_t rgbwCoolToWarmThreshold = 32;

    // Scoped gain for white-die extraction. 256 preserves the established
    // renderer; presentation modes can reduce it without allocating a frame.
    uint16_t rgbwWhiteMixScale256 = 256;

    // Optional per-die measured currents in hundredths of a milliamp. A value of
    // 0 falls back to channelMaxCurrentMa for that die.
    uint16_t redChannelMaxCurrentMaTimes100 = 0;
    uint16_t greenChannelMaxCurrentMaTimes100 = 0;
    uint16_t blueChannelMaxCurrentMaTimes100 = 0;
    uint16_t whiteChannelMaxCurrentMaTimes100 = 0;
};

// Optional bounded layer over ordinary emitters. The caller owns the mask for
// this synchronous RenderFrame call; all unmarked LEDs retain baseline output.
struct LedFrameAccent {
    static constexpr uint16_t MAX_SCALE256 = 384U;
    const uint8_t* mask = nullptr;
    size_t maskCount = 0U;
    uint16_t scale256 = 256U;
    // Zero retains the normal build ceiling. An explicitly owned required cue
    // may request a higher ceiling, bounded to 256/256 and the same current cap.
    uint16_t ceilingScale256 = 0U;

    bool Includes(size_t index) const {
        return mask != nullptr && index < maskCount && mask[index] != 0U;
    }
};

struct BudgetAwareLedPipelineStats {
    // Normal request before policy scaling; with accent, its requested current
    // on the already-safe normal baseline before limiting the accent gain.
    uint16_t estimatedLedCurrentMa = 0;

    // Final current after policy scaling, RGB quantization, and RGBW mapping.
    uint16_t appliedLedCurrentMa = 0;
    float appliedBrightnessScale01 = 1.0f;
    PowerBudgetDecision decision;
    uint16_t appliedAccentScale256 = 256U;
    bool powerLimited = false;
};

// Deterministic LED pipeline:
//  intents/frame (RGB) -> brightness scale -> current estimate -> PowerBudgetPolicy -> final scale -> ILedRenderer
class BudgetAwareLedPipeline final {
  public:
    BudgetAwareLedPipeline(ILedRenderer& renderer, PowerBudgetPolicy& powerPolicy, const BudgetAwareLedPipelineConfig& cfg = {})
        : m_renderer(renderer), m_powerPolicy(powerPolicy), m_cfg(cfg) {}

    void SetBrightnessScale01(const float scale01);
    void SetRgbwWhiteMixScale256(const uint16_t scale256) {
        m_cfg.rgbwWhiteMixScale256 = scale256 > 256U ? 256U : scale256;
    }

    // \brief Render a full frame of LED values.
    // \note frameRgbCount must be <= renderer.GetLedCount(). Excess LEDs are cleared.
    void RenderFrame(const Rgb* frameRgb, size_t frameRgbCount,
                     const LedFrameAccent& accent = {});

    BudgetAwareLedPipelineStats GetLastStats() const { return m_last; }

  private:
    struct AppliedScaleResult {
        float scale01 = 0.0f;
        uint16_t currentMa = 0U;
    };

    static uint8_t ScaleU8(uint8_t v, float s01);
    static float Clamp01(float v);
    Rgb PresentOutputRgb(Rgb color, float emitterScale01) const;

    uint16_t EstimateCurrentMa(const Rgb* frameRgb, size_t frameRgbCount, float brightnessScale01,
                               const LedFrameAccent& accent = {}) const;
    float FindNextLowerScale01(const Rgb* frameRgb, size_t frameRgbCount, float currentScale01) const;
    AppliedScaleResult ResolveSafeScale(const Rgb* frameRgb, size_t frameRgbCount, float proposedScale01, uint16_t allowedLedCurrentMa) const;

    ILedRenderer& m_renderer;
    PowerBudgetPolicy& m_powerPolicy;
    BudgetAwareLedPipelineConfig m_cfg;

    BudgetAwareLedPipelineStats m_last;
};

} // namespace modules
