#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <ArduinoJson.h>

#include "foxie_embedded/settings_store.hpp"

struct RuntimeSettings {
    uint8_t brightness = 5;
    bool diagnosticsEnabled = false;

    uint32_t buttonPressCount = 0;
};

// Schema helpers.
bool MigrateSettings(JsonDocument& doc);
bool ScrubSettings(JsonDocument& doc);

// Construct a SettingsStoreConfig with project hooks pre-wired.
fxe::SettingsStoreConfig MakeSettingsStoreConfig(const char* path);

// Runtime cache helpers.
void LoadRuntimeSettings(const fxe::SettingsStore& store, RuntimeSettings& out);
void PersistRuntimeSettings(fxe::SettingsStore& store, const RuntimeSettings& settings);

// Console helpers.
size_t EnumerateSettingKeys(const char* prefix, const char** outMatches, size_t outMatchesCap);

// Return true if handled; false means use library defaults.
bool SettingsWritePolicy(std::string_view key, bool* outForceSaveNow);
