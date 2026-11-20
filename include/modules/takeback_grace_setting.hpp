#pragma once

#include <cstdint>

#include "modules/settings_store.hpp"

namespace modules::takeback_grace_setting {

static constexpr const char* KEY = "TAKEBACK_GRACE_SECONDS";
static constexpr uint32_t DEFAULT_SECONDS = 0U;
static constexpr uint32_t MIN_SECONDS = 0U;
static constexpr uint32_t MAX_SECONDS = 10U;

inline uint32_t ClampSeconds(const uint32_t seconds) {
    return seconds > MAX_SECONDS ? MAX_SECONDS : seconds;
}

inline uint32_t ReadSeconds(const SettingsStore& settings) {
    return ClampSeconds(settings.ReadValue(KEY, DEFAULT_SECONDS));
}

inline uint32_t ToDurationMs(const uint32_t seconds) {
    return ClampSeconds(seconds) * 1000U;
}

} // namespace modules::takeback_grace_setting
