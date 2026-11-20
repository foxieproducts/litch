#include "hardware/lis2dw12_settings.hpp"

#include <algorithm>
#include <cstring>

namespace hardware::lis2dw12_settings {
namespace {

uint16_t ReadBounded(const fxe::SettingsStore& settings,
                     const char* key,
                     const uint16_t defaultValue,
                     const uint16_t minimum,
                     const uint16_t maximum) {
    const uint32_t value = settings.ReadValue(
        key, static_cast<uint32_t>(defaultValue));
    return static_cast<uint16_t>(
        std::clamp<uint32_t>(value, minimum, maximum));
}

Lis2dw12WakeMode ParseMode(const char* token) {
    if (token != nullptr && std::strcmp(token, "tap") == 0) {
        return Lis2dw12WakeMode::Tap;
    }
    if (token != nullptr && std::strcmp(token, "motion_and_tap") == 0) {
        return Lis2dw12WakeMode::MotionAndTap;
    }
    return Lis2dw12WakeMode::Motion;
}

} // namespace

Lis2dw12Config Read(const fxe::SettingsStore& settings) {
    Lis2dw12Config config;
    config.enabled = settings.ReadValue(ENABLED_KEY, true);
    config.mode = ParseMode(settings.ReadValue(MODE_KEY, DEFAULT_MODE_TOKEN));
    config.motionThresholdMg = ReadBounded(
        settings, MOTION_THRESHOLD_KEY, DEFAULT_MOTION_THRESHOLD_MG, 31U, 1969U);
    config.motionDurationMs = ReadBounded(
        settings, MOTION_DURATION_KEY, DEFAULT_MOTION_DURATION_MS, 0U, 240U);
    config.tapThresholdMg = ReadBounded(
        settings, TAP_THRESHOLD_KEY, DEFAULT_TAP_THRESHOLD_MG, 63U, 1938U);
    config.tapShockMs = ReadBounded(
        settings, TAP_SHOCK_KEY, DEFAULT_TAP_SHOCK_MS, 0U, 60U);
    return config;
}

const char* ModeToken(const Lis2dw12WakeMode mode) {
    switch (mode) {
        case Lis2dw12WakeMode::Tap:
            return "tap";
        case Lis2dw12WakeMode::MotionAndTap:
            return "motion_and_tap";
        case Lis2dw12WakeMode::Motion:
        default:
            return "motion";
    }
}

} // namespace hardware::lis2dw12_settings
