#include <algorithm>
#include <cmath>

#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/rgb_optical_calibration.hpp"
#include "modules/rgbw_auto_map.hpp"

#ifdef LITCH_RELEASE_PROFILE
#ifndef LITCH_MAX_EFFECTIVE_BRIGHTNESS_SCALE01
#define LITCH_MAX_EFFECTIVE_BRIGHTNESS_SCALE01 0.90f
#endif
#endif

namespace modules {

namespace {

constexpr uint8_t SAFE_SCALE_SEARCH_STEPS = 12U;

size_t BoundedFrameCount(const size_t frameRgbCount, const uint16_t ledCount) {
    return (frameRgbCount < static_cast<size_t>(ledCount))
               ? frameRgbCount
               : static_cast<size_t>(ledCount);
}

uint16_t ResolveChannelCurrentMaTimes100(const uint16_t explicitTimes100,
                                         const uint16_t fallbackWholeMa) {
    if (explicitTimes100 != 0U) {
        return explicitTimes100;
    }

    return static_cast<uint16_t>(fallbackWholeMa * 100U);
}

uint32_t EstimateChannelCurrentTimes100(const uint8_t level,
                                        const uint16_t fullScaleCurrentMaTimes100) {
    return (static_cast<uint32_t>(level) * static_cast<uint32_t>(fullScaleCurrentMaTimes100) + 127U) / 255U;
}

float FindNextLowerChannelScale01(const uint8_t inputLevel,
                                  const uint8_t outputLevel,
                                  const float currentScale01) {
    if (inputLevel == 0U || outputLevel == 0U) {
        return 0.0f;
    }

    const float boundary = (static_cast<float>(outputLevel) - 0.5f) /
                           static_cast<float>(inputLevel);
    const float belowBoundary = std::nextafter(boundary, 0.0f);
    return (belowBoundary < currentScale01) ? belowBoundary : 0.0f;
}

float EmitterScale(const float baseline, const LedFrameAccent& accent,
                   const size_t index) {
    if (!accent.Includes(index)) {
        return baseline;
    }
    const float requested = baseline * static_cast<float>(accent.scale256) / 256.0f;
#ifdef LITCH_RELEASE_PROFILE
    constexpr float ceiling = LITCH_MAX_EFFECTIVE_BRIGHTNESS_SCALE01;
#else
    constexpr float ceiling = 1.0f;
#endif
    const float accentCeiling = accent.ceilingScale256 == 0U ? ceiling :
        static_cast<float>(std::min<uint16_t>(accent.ceilingScale256, 256U)) / 256.0f;
    return std::min(requested, accentCeiling);
}

} // namespace

float BudgetAwareLedPipeline::Clamp01(const float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

uint8_t BudgetAwareLedPipeline::ScaleU8(const uint8_t v, const float s01) {
    const float clamped = Clamp01(s01);
    const float scaled = static_cast<float>(v) * clamped;
    const int rounded = static_cast<int>(scaled + 0.5f);
    int bounded = rounded;
    if (bounded < 0) {
        bounded = 0;
    }
    if (bounded > 255) {
        bounded = 255;
    }
    return static_cast<uint8_t>(bounded);
}

void BudgetAwareLedPipeline::SetBrightnessScale01(const float scale01) {
    m_cfg.brightnessScale01 = Clamp01(scale01);
}

Rgb BudgetAwareLedPipeline::PresentOutputRgb(
    const Rgb color, const float emitterScale01) const {
    Rgb presented{ScaleU8(color.r, emitterScale01),
                  ScaleU8(color.g, emitterScale01),
                  ScaleU8(color.b, emitterScale01)};
    if (m_cfg.enableS3OpticalColorCalibration) {
        presented = rgb_optical_calibration::PresentForS3(presented);
    }
    const uint16_t gain = std::min<uint16_t>(m_cfg.outputBrightnessScale256, 256U);
    const auto scale = [gain](const uint8_t channel) {
        return static_cast<uint8_t>(
            (static_cast<uint16_t>(channel) * gain + 128U) >> 8U);
    };
    return {scale(presented.r), scale(presented.g), scale(presented.b)};
}

uint16_t BudgetAwareLedPipeline::EstimateCurrentMa(const Rgb* frameRgb, const size_t frameRgbCount, const float brightnessScale01,
                                                     const LedFrameAccent& accent) const {
    const float s01 = Clamp01(brightnessScale01);
    const uint16_t ledCount = m_renderer.GetLedCount();
    const size_t n = BoundedFrameCount(frameRgbCount, ledCount);

    const uint16_t redCurrentMaTimes100 =
        ResolveChannelCurrentMaTimes100(m_cfg.redChannelMaxCurrentMaTimes100, m_cfg.channelMaxCurrentMa);
    const uint16_t greenCurrentMaTimes100 =
        ResolveChannelCurrentMaTimes100(m_cfg.greenChannelMaxCurrentMaTimes100, m_cfg.channelMaxCurrentMa);
    const uint16_t blueCurrentMaTimes100 =
        ResolveChannelCurrentMaTimes100(m_cfg.blueChannelMaxCurrentMaTimes100, m_cfg.channelMaxCurrentMa);
    const uint16_t whiteCurrentMaTimes100 =
        ResolveChannelCurrentMaTimes100(m_cfg.whiteChannelMaxCurrentMaTimes100, m_cfg.channelMaxCurrentMa);

    uint32_t totalMaTimes100 = 0;
    for (size_t i = 0; i < n; ++i) {
        const Rgb c = frameRgb[i];
        const float emitterScale = EmitterScale(s01, accent, i);
        const Rgb presented = PresentOutputRgb(c, emitterScale);
        RgbwChannels channels{presented.r, presented.g, presented.b, 0U};
        if (m_cfg.enableRgbwAutoMapping) {
            channels = AutoMapRgbToRgbw(
                Rgb{channels.r, channels.g, channels.b},
                m_cfg.rgbwCoolToWarmThreshold, m_cfg.rgbwWhiteMixScale256);
        }

        totalMaTimes100 += EstimateChannelCurrentTimes100(channels.r, redCurrentMaTimes100);
        totalMaTimes100 += EstimateChannelCurrentTimes100(channels.g, greenCurrentMaTimes100);
        totalMaTimes100 += EstimateChannelCurrentTimes100(channels.b, blueCurrentMaTimes100);
        totalMaTimes100 += EstimateChannelCurrentTimes100(channels.w, whiteCurrentMaTimes100);
    }

    // Round up so a fractional milliamp cannot cross the configured ceiling.
    const uint32_t totalMa = (totalMaTimes100 + 99U) / 100U;
    if (totalMa > UINT16_MAX) {
        return UINT16_MAX;
    }

    return static_cast<uint16_t>(totalMa);
}

float BudgetAwareLedPipeline::FindNextLowerScale01(const Rgb* frameRgb,
                                                   const size_t frameRgbCount,
                                                   const float currentScale01) const {
    const float current = Clamp01(currentScale01);
    float next = 0.0f;
    const uint16_t ledCount = m_renderer.GetLedCount();
    const size_t n = BoundedFrameCount(frameRgbCount, ledCount);
    for (size_t i = 0U; i < n; ++i) {
        const Rgb color = frameRgb[i];
        const float red = FindNextLowerChannelScale01(
            color.r, ScaleU8(color.r, current), current);
        const float green = FindNextLowerChannelScale01(
            color.g, ScaleU8(color.g, current), current);
        const float blue = FindNextLowerChannelScale01(
            color.b, ScaleU8(color.b, current), current);
        next = std::max(next, std::max(red, std::max(green, blue)));
    }
    return next;
}

BudgetAwareLedPipeline::AppliedScaleResult BudgetAwareLedPipeline::ResolveSafeScale(
    const Rgb* frameRgb,
    const size_t frameRgbCount,
    const float proposedScale01,
    const uint16_t allowedLedCurrentMa) const {
    const float proposed = Clamp01(proposedScale01);
    const uint16_t proposedCurrentMa = EstimateCurrentMa(frameRgb, frameRgbCount, proposed);
    if (proposedCurrentMa <= allowedLedCurrentMa) {
        return AppliedScaleResult{proposed, proposedCurrentMa};
    }

    // First cross only the nearest RGB rounding boundary. This escapes an
    // RGBW remapping discontinuity without unnecessarily dimming the frame.
    const float nearby = FindNextLowerScale01(frameRgb, frameRgbCount, proposed);
    const uint16_t nearbyCurrentMa = EstimateCurrentMa(frameRgb, frameRgbCount, nearby);
    if (nearbyCurrentMa <= allowedLedCurrentMa) {
        return AppliedScaleResult{nearby, nearbyCurrentMa};
    }

    // The checked zero scale is the safe fallback. Every brighter candidate
    // retained by this bounded search has passed the post-mapping estimator.
    float safe = 0.0f;
    float unsafe = nearby;
    uint16_t safeCurrentMa = 0U;
    for (uint8_t step = 0U; step < SAFE_SCALE_SEARCH_STEPS; ++step) {
        const float candidate = (safe + unsafe) * 0.5f;
        const uint16_t candidateCurrentMa = EstimateCurrentMa(frameRgb, frameRgbCount, candidate);
        if (candidateCurrentMa <= allowedLedCurrentMa) {
            safe = candidate;
            safeCurrentMa = candidateCurrentMa;
        } else {
            unsafe = candidate;
        }
    }
    return AppliedScaleResult{safe, safeCurrentMa};
}

void BudgetAwareLedPipeline::RenderFrame(const Rgb* frameRgb, const size_t frameRgbCount,
                                         const LedFrameAccent& accent) {
    float brightness01 = Clamp01(m_cfg.brightnessScale01);

#ifdef LITCH_RELEASE_PROFILE
    if (brightness01 > LITCH_MAX_EFFECTIVE_BRIGHTNESS_SCALE01) {
        brightness01 = LITCH_MAX_EFFECTIVE_BRIGHTNESS_SCALE01;
    }
#endif

    const uint16_t estimateMa = EstimateCurrentMa(frameRgb, frameRgbCount, brightness01);
    m_powerPolicy.SetEstimatedLedCurrentMa(estimateMa);

    const float budgetScale01 = Clamp01(m_powerPolicy.GetLedBrightnessScale01());
    const float proposedScale01 = Clamp01(brightness01 * budgetScale01);
    uint16_t allowedLedCurrentMa = m_powerPolicy.GetAllowedLedCurrentMa();
    const AppliedScaleResult applied = (budgetScale01 == 1.0f)
                                           ? AppliedScaleResult{brightness01, estimateMa}
                                           : ResolveSafeScale(frameRgb, frameRgbCount,
                                                              proposedScale01,
                                                              allowedLedCurrentMa);

    // Resolve normal output first. Accent may use spare current, but can never
    // dim unmarked required cues by changing the shared brightness multiplier.
    LedFrameAccent effectiveAccent = accent;
    effectiveAccent.scale256 = std::min<uint16_t>(accent.scale256, LedFrameAccent::MAX_SCALE256);
    uint16_t actualCurrentMa = applied.currentMa;
    uint16_t requestedCurrentMa = estimateMa;
    if (accent.mask != nullptr && accent.scale256 != 256U) {
        actualCurrentMa = EstimateCurrentMa(frameRgb, frameRgbCount,
                                           applied.scale01, effectiveAccent);
        // Baseline demand is not the hardware ceiling. Let the existing policy
        // arbitrate accent demand while retaining the already-safe base scale.
        requestedCurrentMa = actualCurrentMa;
        m_powerPolicy.SetEstimatedLedCurrentMa(actualCurrentMa);
        allowedLedCurrentMa = m_powerPolicy.GetAllowedLedCurrentMa();
        if (actualCurrentMa > allowedLedCurrentMa) {
            uint16_t safe = effectiveAccent.scale256 >= 256U ? 256U : 0U;
            uint16_t unsafe = effectiveAccent.scale256;
            while (unsafe - safe > 1U) {
                const uint16_t candidate = static_cast<uint16_t>((safe + unsafe) / 2U);
                effectiveAccent.scale256 = candidate;
                if (EstimateCurrentMa(frameRgb, frameRgbCount, applied.scale01,
                                      effectiveAccent) <= allowedLedCurrentMa) {
                    safe = candidate;
                } else {
                    unsafe = candidate;
                }
            }
            effectiveAccent.scale256 = safe;
            actualCurrentMa = EstimateCurrentMa(frameRgb, frameRgbCount,
                                               applied.scale01, effectiveAccent);
        }
    }

    if (accent.mask != nullptr && accent.scale256 != 256U) {
        m_powerPolicy.SetEstimatedLedCurrentMa(actualCurrentMa);
    }
    m_renderer.Clear();

    const uint16_t ledCount = m_renderer.GetLedCount();
    const size_t n = BoundedFrameCount(frameRgbCount, ledCount);

    for (size_t i = 0; i < n; ++i) {
        const Rgb c = frameRgb[i];
        const float emitterScale = EmitterScale(applied.scale01, effectiveAccent, i);
        const Rgb presented = PresentOutputRgb(c, emitterScale);
        m_renderer.SetLedRgb(static_cast<uint16_t>(i), presented.r,
                             presented.g, presented.b);
    }

    m_renderer.Show();

    m_last.estimatedLedCurrentMa = requestedCurrentMa;
    m_last.appliedLedCurrentMa = actualCurrentMa;
    m_last.appliedAccentScale256 = effectiveAccent.scale256;
    m_last.powerLimited = budgetScale01 < 1.0f ||
        effectiveAccent.scale256 < std::min<uint16_t>(accent.scale256, LedFrameAccent::MAX_SCALE256);
    m_last.appliedBrightnessScale01 = applied.scale01 *
        static_cast<float>(std::min<uint16_t>(m_cfg.outputBrightnessScale256, 256U)) / 256.0f;
    m_last.decision = m_powerPolicy.GetDecision();
}

} // namespace modules
