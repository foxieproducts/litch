#pragma once

#include <ArduinoJson.h>

#include <cstdint>

#include "modules/settings_store.hpp"

namespace modules::player_end_palette {

inline constexpr uint8_t P1_END_DEFAULT_WHEEL = 170U;
inline constexpr uint8_t P2_END_DEFAULT_WHEEL = 0U;

[[nodiscard]] bool IsLegacyLogicalKey(const char* key);
[[nodiscard]] bool IsPhysicalEndKey(const char* key);

// Returns the canonical physical-end storage key addressed by either a direct
// physical key or a deployed logical-player compatibility key.
[[nodiscard]] const char* ResolvePhysicalStorageKey(
    const fxe::SettingsStore& settings,
    const char* requestedKey);

// Reads a canonical physical-end value or its same-numbered end alias.
// Opening-player choice never changes this mapping.
[[nodiscard]] uint32_t ReadSettingUInt32(
    const fxe::SettingsStore& settings,
    const char* key,
    uint32_t defaultValue);

// First migration preserves canonical colors over old swapped aliases and
// persists PLAYER_PALETTE_V1. Later alias edits/resets from downgraded firmware
// import into the same end slot, then aliases are synchronized again.
[[nodiscard]] bool CanonicalizeLoadedSettings(JsonDocument& doc);

// Synchronizes deployed logical aliases from canonical physical-end values.
// Call after a physical palette mutation.
void SynchronizeLegacyAliases(JsonDocument& doc);

} // namespace modules::player_end_palette
