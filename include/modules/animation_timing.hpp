#pragma once

#include <cstdint>
#include <cmath>

#include "modules/rgb.hpp"

namespace modules::animation {

struct AnimationContext {
    uint32_t elapsedMs = 0;
};

enum class PlaybackMode : uint8_t {
    Loop = 0,
    OneShot = 1,
    PingPongLoop = 2,
    PingPongOneShot = 3,
};

enum class Waveform : uint8_t {
    Constant = 0,
    Blink = 1,
    Triangle = 2,
    Saw = 3,
    ReverseSaw = 4,
    DoublePulse = 5,
    Breathe = 6,
};

enum class BlendMode : uint8_t {
    Replace = 0,
    AlphaBlend = 1,
    Add = 2,
};

struct PhaseToken {
    uint32_t periodMs = 1;
    PlaybackMode playback = PlaybackMode::Loop;
    uint8_t phaseOffset255 = 0;
};

struct CurveToken {
    Waveform waveform = Waveform::Constant;
    uint8_t minValue = 0;
    uint8_t maxValue = 255;
    uint8_t onWidth255 = 128;
    uint8_t gapWidth255 = 28;
};

struct ValueToken {
    PhaseToken phase{};
    CurveToken curve{};
};

struct AnimationSample {
    uint8_t progress255 = 0;
    bool active = false;
    bool finished = false;
};

struct BlinkToken {
    uint32_t periodMs = 1;
    uint32_t onMs = 0;
};

struct AnimationTokens {
    BlinkToken moverPieceBlink{};
    ValueToken interactionPickupPulse{};
    ValueToken invalidBoardRainbow{};
    PhaseToken checkmateMotion{};
    ValueToken checkmatePulse{};
    ValueToken drawShimmer{};
    ValueToken stalemateShimmer{};
    ValueToken hintCauldronBreath{};
    ValueToken hintBrewAcknowledgment{};
};

inline constexpr AnimationTokens LED_FRAME_TOKENS{
    BlinkToken{800U, 600U},
    ValueToken{
        PhaseToken{420U, PlaybackMode::Loop, 0U},
        CurveToken{Waveform::Triangle, 80U, 255U, 128U, 28U},
    },
    ValueToken{
        PhaseToken{960U, PlaybackMode::Loop, 0U},
        CurveToken{Waveform::Saw, 0U, 255U, 128U, 28U},
    },
    PhaseToken{1100U, PlaybackMode::Loop, 0U},
    ValueToken{
        PhaseToken{2200U, PlaybackMode::Loop, 0U},
        CurveToken{Waveform::Triangle, 0U, 255U, 128U, 28U},
    },
    ValueToken{
        PhaseToken{2400U, PlaybackMode::Loop, 0U},
        CurveToken{Waveform::Triangle, 18U, 46U, 128U, 28U},
    },
    ValueToken{
        PhaseToken{2800U, PlaybackMode::Loop, 0U},
        CurveToken{Waveform::Triangle, 14U, 36U, 128U, 28U},
    },
    ValueToken{
        PhaseToken{1600U, PlaybackMode::Loop, 0U},
        CurveToken{Waveform::Breathe, 10U, 255U, 128U, 28U},
    },
    ValueToken{
        PhaseToken{150U, PlaybackMode::OneShot, 0U},
        CurveToken{Waveform::Breathe, 0U, 46U, 128U, 28U},
    },
};

inline uint8_t BlendValue(const uint8_t base,
                          const uint8_t over,
                          const uint8_t alpha) {
    return modules::AlphaBlendU8(base, over, alpha);
}

inline modules::Rgb BlendRgb(const modules::Rgb base,
                             const modules::Rgb over,
                             const uint8_t alpha) {
    return modules::Rgb{
        BlendValue(base.r, over.r, alpha),
        BlendValue(base.g, over.g, alpha),
        BlendValue(base.b, over.b, alpha),
    };
}

inline modules::Rgb AddRgbSaturating(const modules::Rgb base,
                                     const modules::Rgb over) {
    return modules::AddRgbSaturating(base, over);
}

inline void ApplyBlend(modules::Rgb& target,
                       const modules::Rgb color,
                       const uint8_t alpha,
                       const BlendMode mode) {
    switch (mode) {
        case BlendMode::Replace:
            target = color;
            return;
        case BlendMode::AlphaBlend:
            target = BlendRgb(target, color, alpha);
            return;
        case BlendMode::Add:
            target = AddRgbSaturating(target, BlendRgb(modules::RGB_BLACK, color, alpha));
            return;
    }
}

inline AnimationSample SamplePhase(const AnimationContext& context,
                                   const PhaseToken& token) {
    AnimationSample sample{};

    const uint32_t periodMs = (token.periodMs == 0U) ? 1U : token.periodMs;
    const uint32_t offsetMs = (static_cast<uint32_t>(token.phaseOffset255) * periodMs + 127U) / 255U;
    const uint32_t elapsedMs = context.elapsedMs + offsetMs;

    switch (token.playback) {
        case PlaybackMode::Loop: {
            sample.active = true;
            sample.progress255 = static_cast<uint8_t>(((elapsedMs % periodMs) * 255U) / periodMs);
            return sample;
        }

        case PlaybackMode::OneShot: {
            if (elapsedMs >= periodMs) {
                sample.finished = true;
                return sample;
            }

            sample.active = true;
            sample.progress255 = static_cast<uint8_t>((elapsedMs * 255U) / periodMs);
            return sample;
        }

        case PlaybackMode::PingPongLoop:
        case PlaybackMode::PingPongOneShot: {
            const uint32_t cycleMs = periodMs * 2U;
            if (token.playback == PlaybackMode::PingPongOneShot && elapsedMs >= cycleMs) {
                sample.finished = true;
                return sample;
            }

            const uint32_t cycleElapsedMs = (cycleMs == 0U) ? 0U : (elapsedMs % cycleMs);
            const bool reverse = cycleElapsedMs >= periodMs;
            const uint32_t legElapsedMs = reverse
                                              ? static_cast<uint32_t>(cycleMs - cycleElapsedMs)
                                              : cycleElapsedMs;
            const uint32_t boundedLegElapsedMs = (legElapsedMs > periodMs) ? periodMs : legElapsedMs;
            sample.active = true;
            sample.progress255 = static_cast<uint8_t>((boundedLegElapsedMs * 255U) / periodMs);
            return sample;
        }
    }

    return sample;
}

inline uint8_t SampleCurve(const AnimationSample& sample,
                           const CurveToken& token) {
    const uint8_t minValue = token.minValue < token.maxValue ? token.minValue : token.maxValue;
    const uint8_t maxValue = token.minValue > token.maxValue ? token.minValue : token.maxValue;
    if (!sample.active) {
        return minValue;
    }

    const auto scaleBetween = [&](const uint8_t level255) -> uint8_t {
        const uint16_t span = static_cast<uint16_t>(maxValue - minValue);
        return static_cast<uint8_t>(minValue +
                                    ((static_cast<uint16_t>(level255) * span + 127U) / 255U));
    };

    switch (token.waveform) {
        case Waveform::Constant:
            return maxValue;

        case Waveform::Blink:
            return (sample.progress255 < token.onWidth255) ? maxValue : minValue;

        case Waveform::Triangle: {
            const uint16_t up = (sample.progress255 <= 127U)
                                    ? static_cast<uint16_t>(sample.progress255) * 2U
                                    : static_cast<uint16_t>(255U - sample.progress255) * 2U;
            const uint8_t level255 = (up > 255U) ? 255U : static_cast<uint8_t>(up);
            return scaleBetween(level255);
        }

        case Waveform::Breathe: {
            // The output pipeline scales emitter values linearly. Shape only
            // this temporal envelope with gamma 2.2; do not gamma the hue again.
            constexpr float FULL_TURN_RADIANS = 6.28318530718F;
            const float phase = static_cast<float>(sample.progress255) / 255.0F;
            const float smooth = (1.0F - std::cos(FULL_TURN_RADIANS * phase)) * 0.5F;
            const float corrected = std::pow(smooth < 0.0F ? 0.0F : smooth, 2.2F);
            return scaleBetween(static_cast<uint8_t>(corrected * 255.0F + 0.5F));
        }

        case Waveform::Saw:
            return scaleBetween(sample.progress255);

        case Waveform::ReverseSaw:
            return scaleBetween(static_cast<uint8_t>(255U - sample.progress255));

        case Waveform::DoublePulse: {
            const uint8_t pulseWidth255 = (token.onWidth255 == 0U) ? 64U : token.onWidth255;
            const uint8_t gap255 = token.gapWidth255;
            uint8_t localProgress255 = 0;

            if (sample.progress255 < pulseWidth255) {
                localProgress255 = static_cast<uint8_t>((static_cast<uint16_t>(sample.progress255) * 255U) /
                                                        pulseWidth255);
            } else {
                const uint16_t secondPulseStart = static_cast<uint16_t>(pulseWidth255) + gap255;
                const uint16_t secondPulseEnd = secondPulseStart + pulseWidth255;
                if (sample.progress255 >= secondPulseStart && sample.progress255 < secondPulseEnd) {
                    localProgress255 = static_cast<uint8_t>(
                        ((static_cast<uint16_t>(sample.progress255) - secondPulseStart) * 255U) /
                        pulseWidth255);
                }
            }

            const uint16_t up = (localProgress255 <= 127U)
                                    ? static_cast<uint16_t>(localProgress255) * 2U
                                    : static_cast<uint16_t>(255U - localProgress255) * 2U;
            const uint8_t level255 = (up > 255U) ? 255U : static_cast<uint8_t>(up);
            return scaleBetween(level255);
        }
    }

    return minValue;
}

inline uint8_t SampleValue(const AnimationContext& context,
                           const ValueToken& token) {
    return SampleCurve(SamplePhase(context, token.phase), token.curve);
}

inline uint8_t TriangleWave255(const AnimationContext& context,
                               const uint32_t periodMs) {
    ValueToken token;
    token.phase.periodMs = periodMs;
    token.phase.playback = PlaybackMode::Loop;
    token.curve.waveform = Waveform::Triangle;
    token.curve.minValue = 0U;
    token.curve.maxValue = 255U;
    return SampleValue(context, token);
}

inline bool IsBlinkOn(const AnimationContext& context,
                      const BlinkToken& token) {
    ValueToken valueToken;
    valueToken.phase.periodMs = token.periodMs;
    valueToken.phase.playback = PlaybackMode::Loop;
    valueToken.curve.waveform = Waveform::Blink;
    valueToken.curve.minValue = 0U;
    valueToken.curve.maxValue = 255U;
    const uint32_t period = (token.periodMs == 0U) ? 1U : token.periodMs;
    const uint32_t onMs = (token.onMs > period) ? period : token.onMs;
    valueToken.curve.onWidth255 = static_cast<uint8_t>((onMs * 255U) / period);
    return SampleValue(context, valueToken) >= 128U;
}

} // namespace modules::animation
