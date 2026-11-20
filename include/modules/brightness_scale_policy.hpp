#pragma once

#include <cstdint>

namespace modules::brightness_scale_policy {

// Q8.8 scale factors used by the shipped render path.
constexpr uint16_t MAX_EFFECTIVE_SCALE256 = 230U;
constexpr uint16_t USER_MAX_EFFECTIVE_SCALE256 = MAX_EFFECTIVE_SCALE256;
// The dark-room endpoint is one quarter of the selected ceiling, subject to
// the minimum scale below that preserves board guidance at low slider levels.
constexpr uint16_t AUTOMATIC_BASE_MAX_SCALE256 =
    static_cast<uint16_t>((static_cast<uint32_t>(MAX_EFFECTIVE_SCALE256) *
                           25U +
                           50U) /
                          100U);
constexpr uint16_t MIN_EFFECTIVE_SCALE256 = 12U;

// Required square/status cues keep the proven global envelope. At level zero,
// the compositor separately reduces ordinary pieces to code1 and omits the dim
// background; a lower global scale erases king brackets and turn indications.
constexpr uint16_t NEAR_DARK_SCALE256 = 8U;

constexpr uint8_t ClampLevel0To8(const uint32_t level) {
    return level > 8U ? 8U : static_cast<uint8_t>(level);
}

// Computes the final Q8.8 global render scale. Ambient interpolation is only
// active with a usable fitted-sensor sample and the persisted preference on.
// Automatic startup/fallback retains its dark-room base; manual uses the full
// user range. Auto dims within that same user ceiling, never above it. Sensor
// acquisition is independent. Boot fade is intentionally last.
inline uint16_t ComputeEffectiveScale256(const uint32_t requestedLevel,
                                         const bool hasAmbientSensor,
                                         const float ambientFactor01,
                                         const uint16_t fadeScale256,
                                         const bool automaticBrightness = true) {
    const uint8_t level = ClampLevel0To8(requestedLevel);
    if (level == 0U) {
        // Explicit near-dark choice: ambient light must not brighten it. The
        // normal sleep/boot fade still owns deliberate off transitions.
        return static_cast<uint16_t>((NEAR_DARK_SCALE256 * fadeScale256 + 128U) >> 8U);
    }
    const uint16_t levelMaximum = automaticBrightness
        ? AUTOMATIC_BASE_MAX_SCALE256 : USER_MAX_EFFECTIVE_SCALE256;
    const uint16_t unclamped = static_cast<uint16_t>(
        (static_cast<uint32_t>(level) * levelMaximum + 4U) /
        8U);
    const uint16_t minimum = unclamped < MIN_EFFECTIVE_SCALE256
                                 ? MIN_EFFECTIVE_SCALE256
                                 : unclamped;

    const uint16_t userMaximum = static_cast<uint16_t>(
        (static_cast<uint32_t>(level) * USER_MAX_EFFECTIVE_SCALE256 + 4U) / 8U);
    uint16_t base = minimum;
    if (automaticBrightness && hasAmbientSensor) {
        const uint32_t span = userMaximum - minimum;
        const float ambient = ambientFactor01 > 1.0f ? 1.0f
            : ambientFactor01 > 0.0f ? ambientFactor01 : 0.0f;
        const uint32_t scaledSpan = static_cast<uint32_t>(
            static_cast<float>(span) * ambient + 0.5f);
        const uint32_t lerp = minimum + scaledSpan;
        base = lerp > userMaximum
                   ? userMaximum
                   : static_cast<uint16_t>(lerp);
    }

    const uint32_t applied =
        (static_cast<uint32_t>(base) * fadeScale256 + 128U) >> 8U;
    return applied > 256U ? 256U : static_cast<uint16_t>(applied);
}

} // namespace modules::brightness_scale_policy
