#pragma once

#include <cstdint>

#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/led_power_envelope.hpp"

namespace modules::led_render_policy {

constexpr uint16_t LEGACY_BROWNOUT_LED_CURRENT_LIMIT_MA = 450U;
// One owner for the unchanged compositor boost. Bounded cues can compensate
// before this stage instead of clipping a complementary fade at its peak.
constexpr uint16_t COMPOSER_GLOBAL_BRIGHTNESS_BOOST_SCALE256 = 320U;
// Only bright amber taps spend spare current above the ordinary emitter level.
constexpr uint16_t CAPTURE_REMOVAL_ACCENT_SCALE256 = 384U;
constexpr uint16_t CaptureTapScale(const uint64_t tapMask) {
    return tapMask != 0ULL ? CAPTURE_REMOVAL_ACCENT_SCALE256 : 256U;
}
constexpr uint16_t DUCK_REMINDER_ACCENT_SCALE256 = 384U;

struct PowerPolicyInputs {
    bool isS3 = false;
    bool brownoutLimitLatched = false;
};

inline PowerBudgetPolicyConfig MakePowerBudgetPolicyConfig(
    const PowerPolicyInputs inputs) {
    PowerBudgetPolicyConfig cfg;
    cfg.allowUsbPower = false;
    cfg.hysteresisMa = 0U;

    if (!inputs.isS3) {
        cfg.maxLedCurrentMa = inputs.brownoutLimitLatched
                                  ? LEGACY_BROWNOUT_LED_CURRENT_LIMIT_MA
                                  : S3_MAX_LED_CURRENT_MA;
        cfg.maxTotalCurrentMa = cfg.maxLedCurrentMa;
        return cfg;
    }

    const LedPowerEnvelope envelope = SelectS3LedPowerEnvelope(
        inputs.brownoutLimitLatched);
    cfg.maxLedCurrentMa = envelope.maxLedCurrentMa;
    cfg.maxTotalCurrentMa = envelope.maxTotalCurrentMa;
    cfg.fixedSystemCurrentMa = envelope.fixedSystemCurrentMa;
    return cfg;
}

inline PowerBudgetPolicyConfig MakeConservativePowerBudgetPolicyConfig(
    const PowerPolicyInputs inputs) {
    if (!inputs.isS3) {
        return MakePowerBudgetPolicyConfig(inputs);
    }
    return MakePowerBudgetPolicyConfig(PowerPolicyInputs{true, true});
}

inline BudgetAwareLedPipelineConfig MakePipelineConfig(
    const bool enableRgbwAutoMapping,
    const uint8_t rgbwCoolToWarmThreshold) {
    BudgetAwareLedPipelineConfig cfg;
    // This is the production fallback for all builds, not the legacy 20mA
    // default in BudgetAwareLedPipelineConfig.
    cfg.channelMaxCurrentMa = S3_CHANNEL_MAX_CURRENT_MA;
    cfg.enableRgbwAutoMapping = enableRgbwAutoMapping;
    cfg.rgbwCoolToWarmThreshold = rgbwCoolToWarmThreshold;
    if (enableRgbwAutoMapping) {
        cfg.enableS3OpticalColorCalibration = true;
        cfg.redChannelMaxCurrentMaTimes100 = S3_RED_CURRENT_MA_TIMES100;
        cfg.greenChannelMaxCurrentMaTimes100 = S3_GREEN_CURRENT_MA_TIMES100;
        cfg.blueChannelMaxCurrentMaTimes100 = S3_BLUE_CURRENT_MA_TIMES100;
        cfg.whiteChannelMaxCurrentMaTimes100 = S3_WHITE_CURRENT_MA_TIMES100;
    }
    return cfg;
}

} // namespace modules::led_render_policy
