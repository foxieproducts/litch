#pragma once

#include <cstdint>
#include <string_view>

#include "foxie_embedded/settings_store.hpp"

namespace hardware::i2c_clock_settings {

inline constexpr const char* SETTING_KEY = "I2C_CLOCK_HZ";
inline constexpr uint32_t DEFAULT_CLOCK_HZ = 100000U;

// Diagnostic rates; 1 MHz is experimental, beyond the fitted sensors' ratings.
[[nodiscard]] bool TryParse(std::string_view token, uint32_t& outHz);

// Invalid or absent persisted values retain the conservative boot default.
[[nodiscard]] uint32_t Read(const fxe::SettingsStore& settings);

// Validate before changing RAM or deferred-save intent. Remains usable when
// persistence is disabled for the session; the caller applies the runtime clock.
[[nodiscard]] bool TrySelect(fxe::SettingsStore& settings,
                             std::string_view token,
                             uint32_t& outHz);

} // namespace hardware::i2c_clock_settings
