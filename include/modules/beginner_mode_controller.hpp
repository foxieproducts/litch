#pragma once

#include <cstdint>

#include "modules/assistance_preferences.hpp"

namespace modules {

class BeginnerModeController final {
  public:
    BeginnerModeController() = default;
    explicit BeginnerModeController(const SettingsStore& settings)
        : m_settings(&settings) {}

    bool IsEnabled(const UiOwner owner) const {
        return m_settings != nullptr && assistance_preferences::Enabled(
            *m_settings, owner, assistance_preferences::Feature::AutomaticHints);
    }

    bool IsEffective(const UiOwner owner) const {
        return m_settings != nullptr &&
            assistance_preferences::AutomaticHintsEffective(*m_settings, owner);
    }

    UiOwner ActiveOwnerForTurn(const UiOwner turnOwner, const bool humanTurn) const {
        if (!humanTurn) {
            return UiOwner::None;
        }

        return IsEffective(turnOwner) ? turnOwner : UiOwner::None;
    }

  private:
    const SettingsStore* m_settings = nullptr;
};

} // namespace modules
