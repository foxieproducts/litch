#pragma once

#include "modules/settings_store.hpp"

namespace modules::minimal_animations_setting {

inline constexpr const char* KEY = "MINIMAL_ANIMATIONS";

inline bool Read(const modules::SettingsStore& settings) {
    return settings.ReadValue(KEY, false);
}

} // namespace modules::minimal_animations_setting
