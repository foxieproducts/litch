#include "app_settings.hpp"

#include <algorithm>

#include "foxie_embedded/logger.hpp"

namespace {
static constexpr const char* KEY_BRIGHTNESS = "BRIGHTNESS";
static constexpr const char* KEY_DIAGNOSTICS = "DIAGNOSTICS";
static constexpr const char* KEY_BUTTON_PRESS_COUNT = "BUTTON_PRESS_COUNT";

static constexpr uint8_t MIN_BRIGHTNESS = 0;
static constexpr uint8_t MAX_BRIGHTNESS = 10;

bool StartsWith(const char* s, const char* prefix) {
    if (!s || !prefix) {
        return false;
    }

    while (*prefix) {
        if (*s++ != *prefix++) {
            return false;
        }
    }

    return true;
}
} // namespace

bool MigrateSettings(JsonDocument& doc) {
    bool changed = false;

    // Example migration: rename BRIGHT -> BRIGHTNESS.
    if (doc.containsKey("BRIGHT") && !doc.containsKey(KEY_BRIGHTNESS)) {
        doc[KEY_BRIGHTNESS] = doc["BRIGHT"];
        doc.remove("BRIGHT");
        changed = true;
    }

    return changed;
}

bool ScrubSettings(JsonDocument& doc) {
    bool changed = false;

    // Clamp brightness.
    const uint32_t brightness = doc[KEY_BRIGHTNESS] | static_cast<uint32_t>(5);
    const uint32_t clamped = std::min<uint32_t>(MAX_BRIGHTNESS, std::max<uint32_t>(MIN_BRIGHTNESS, brightness));
    if (brightness != clamped) {
        doc[KEY_BRIGHTNESS] = clamped;
        changed = true;
    }

    // Force diagnostics to be 0/1.
    const uint32_t diag = doc[KEY_DIAGNOSTICS] | 0U;
    const uint32_t diag01 = (diag != 0U) ? 1U : 0U;
    if (diag != diag01) {
        doc[KEY_DIAGNOSTICS] = diag01;
        changed = true;
    }

    return changed;
}

fxe::SettingsStoreConfig MakeSettingsStoreConfig(const char* path) {
    fxe::SettingsStoreConfig cfg;
    cfg.path = path;

    cfg.migrateFunc = [](JsonDocument& doc) {
        return MigrateSettings(doc);
    };

    cfg.scrubFunc = [](JsonDocument& doc, fxe::SettingsRemovedKeys*) {
        return ScrubSettings(doc);
    };

    return cfg;
}

void LoadRuntimeSettings(const fxe::SettingsStore& store, RuntimeSettings& out) {
    const JsonObjectConst root = store.Doc().as<JsonObjectConst>();

    out.brightness = static_cast<uint8_t>(root[KEY_BRIGHTNESS] | static_cast<uint32_t>(out.brightness));
    out.diagnosticsEnabled = (root[KEY_DIAGNOSTICS] | 0U) != 0U;
    out.buttonPressCount = static_cast<uint32_t>(root[KEY_BUTTON_PRESS_COUNT] | out.buttonPressCount);
}

void PersistRuntimeSettings(fxe::SettingsStore& store, const RuntimeSettings& settings) {
    JsonDocument& doc = store.Doc();

    doc[KEY_BRIGHTNESS] = static_cast<uint32_t>(settings.brightness);
    doc[KEY_DIAGNOSTICS] = settings.diagnosticsEnabled ? 1U : 0U;
    doc[KEY_BUTTON_PRESS_COUNT] = settings.buttonPressCount;

    store.MarkDirty();
}

size_t EnumerateSettingKeys(const char* prefix, const char** outMatches, const size_t outMatchesCap) {
    if (!outMatches || outMatchesCap == 0) {
        return 0;
    }

    const char* keys[] = {KEY_BRIGHTNESS, KEY_DIAGNOSTICS, KEY_BUTTON_PRESS_COUNT};

    size_t count = 0;
    for (const char* key : keys) {
        if (count >= outMatchesCap) {
            break;
        }
        if (!prefix || prefix[0] == '\0' || StartsWith(key, prefix)) {
            outMatches[count++] = key;
        }
    }

    return count;
}

bool SettingsWritePolicy(const std::string_view key, bool* outForceSaveNow) {
    if (!outForceSaveNow) {
        return false;
    }

    // Example policy: use the short must-save tier for brightness.
    if (key == KEY_BRIGHTNESS) {
        *outForceSaveNow = true;
        return true;
    }

    return false;
}
