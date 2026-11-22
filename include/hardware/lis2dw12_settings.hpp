#pragma once

#include "foxie_embedded/settings_store.hpp"
#include "hardware/lis2dw12.hpp"

namespace hardware::lis2dw12_settings {

inline constexpr const char* ENABLED_KEY = "MOTION_WAKE_ENABLED";
inline constexpr const char* MODE_KEY = "MOTION_WAKE_MODE";
inline constexpr const char* MOTION_THRESHOLD_KEY =
    "MOTION_WAKE_THRESHOLD_MG";
inline constexpr const char* MOTION_DURATION_KEY =
    "MOTION_WAKE_DURATION_MS";
inline constexpr const char* TAP_THRESHOLD_KEY = "TAP_WAKE_THRESHOLD_MG";
inline constexpr const char* TAP_SHOCK_KEY = "TAP_WAKE_SHOCK_MS";

inline constexpr const char* DEFAULT_MODE_TOKEN = "tap";
inline constexpr uint16_t DEFAULT_MOTION_THRESHOLD_MG = 125U;
inline constexpr uint16_t DEFAULT_MOTION_DURATION_MS = 80U;
inline constexpr uint16_t DEFAULT_TAP_THRESHOLD_MG = Lis2dw12Config{}.tapThresholdMg;
inline constexpr uint16_t DEFAULT_TAP_SHOCK_MS = Lis2dw12Config{}.tapShockMs;

[[nodiscard]] Lis2dw12Config Read(const fxe::SettingsStore& settings);
[[nodiscard]] const char* ModeToken(Lis2dw12WakeMode mode);

} // namespace hardware::lis2dw12_settings
