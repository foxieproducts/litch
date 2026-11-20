#pragma once

#include <cstring>

#include "modules/settings_store.hpp"
#include "modules/ui_types.hpp"

namespace modules::assistance_preferences {

enum class Feature : uint8_t { LegalMoves, CheckCue, AutomaticHints };

inline constexpr const char* KEYS[] = {
    "P1_LEGAL_MOVES", "P2_LEGAL_MOVES",
    "P1_CHECK_CUE", "P2_CHECK_CUE",
    "P1_AUTO_HINTS", "P2_AUTO_HINTS",
};

inline const char* Key(const UiOwner owner, const Feature feature) {
    if (owner != UiOwner::P1 && owner != UiOwner::P2) return nullptr;
    return KEYS[static_cast<uint8_t>(feature) * 2U +
                (owner == UiOwner::P2 ? 1U : 0U)];
}

inline bool IsKey(const char* key) {
    for (const char* candidate : KEYS) {
        if (key != nullptr && std::strcmp(candidate, key) == 0) return true;
    }
    return false;
}

inline UiOwner AutomaticHintsOwner(const char* key) {
    if (key == nullptr) return UiOwner::None;
    if (std::strcmp(key, KEYS[4]) == 0) return UiOwner::P1;
    if (std::strcmp(key, KEYS[5]) == 0) return UiOwner::P2;
    return UiOwner::None;
}

inline bool Enabled(const SettingsStore& settings, const UiOwner owner,
                    const Feature feature) {
    const char* key = Key(owner, feature);
    return key != nullptr && settings.ReadValue(
        key, feature != Feature::AutomaticHints);
}

inline bool CpuOwns(const SettingsStore& settings, const UiOwner owner) {
    if (owner == UiOwner::None ||
        settings.ReadValue("VS_CPU_LEVEL", uint32_t{0U}) == 0U) return false;
    const uint32_t player = settings.ReadValue("VS_CPU_PLAYER", uint32_t{2U});
    return owner == (player == 1U ? UiOwner::P1 : UiOwner::P2);
}

inline bool AutomaticHintsEffective(const SettingsStore& settings,
                                     const UiOwner owner) {
    return !CpuOwns(settings, owner) &&
           Enabled(settings, owner, Feature::AutomaticHints);
}

} // namespace modules::assistance_preferences
