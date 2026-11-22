#include "modules/player_end_palette.hpp"

#include <cstring>

#include "modules/ui_color_keys.hpp"
#include "modules/ui_color_wheel.hpp"

namespace modules::player_end_palette {
namespace {

constexpr const char* MIGRATION_MARKER = "PLAYER_PALETTE_V1";

uint8_t ReadDocumentWheel(const JsonDocument& doc,
                          const char* key,
                          const uint8_t defaultValue) {
    const uint32_t raw = doc[key] | static_cast<uint32_t>(defaultValue);
    return modules::ui_color_wheel::ClampWheelValue01To255(raw);
}

uint8_t ReadPhysicalEndWheel(const fxe::SettingsStore& settings,
                             const bool p2End) {
    const JsonDocument& doc = settings.Doc();
    const char* const canonicalKey = p2End
                                         ? modules::ui_color_keys::P2_END_PIECE_WHEEL
                                         : modules::ui_color_keys::P1_END_PIECE_WHEEL;
    if (doc.containsKey(canonicalKey)) {
        return modules::ui_color_wheel::ReadWheelValue01To255(
            settings,
            canonicalKey,
            p2End ? P2_END_DEFAULT_WHEEL : P1_END_DEFAULT_WHEEL);
    }

    const char* const legacyKey = p2End ? modules::ui_color_keys::P2_PIECE_WHEEL
                                      : modules::ui_color_keys::P1_PIECE_WHEEL;
    const uint8_t legacyDefault =
        std::strcmp(legacyKey, modules::ui_color_keys::P1_PIECE_WHEEL) == 0
            ? P1_END_DEFAULT_WHEEL
            : P2_END_DEFAULT_WHEEL;
    return modules::ui_color_wheel::ReadWheelValue01To255(
        settings, legacyKey, legacyDefault);
}

bool SetWheel(JsonDocument& doc, const char* key, const uint8_t value) {
    if (doc.containsKey(key) && doc[key].is<uint32_t>() &&
        doc[key].as<uint32_t>() == value) {
        return false;
    }
    doc[key] = static_cast<uint32_t>(value);
    return true;
}

} // namespace

bool IsLegacyLogicalKey(const char* key) {
    return key != nullptr &&
           (std::strcmp(key, modules::ui_color_keys::P1_PIECE_WHEEL) == 0 ||
            std::strcmp(key, modules::ui_color_keys::P2_PIECE_WHEEL) == 0);
}

bool IsPhysicalEndKey(const char* key) {
    return key != nullptr &&
           (std::strcmp(key, modules::ui_color_keys::P1_END_PIECE_WHEEL) == 0 ||
            std::strcmp(key, modules::ui_color_keys::P2_END_PIECE_WHEEL) == 0);
}

const char* ResolvePhysicalStorageKey(const fxe::SettingsStore& settings,
                                      const char* requestedKey) {
    (void)settings;
    if (requestedKey == nullptr) {
        return nullptr;
    }
    if (IsPhysicalEndKey(requestedKey)) {
        return requestedKey;
    }
    if (std::strcmp(requestedKey, modules::ui_color_keys::P1_PIECE_WHEEL) == 0) {
        return modules::ui_color_keys::P1_END_PIECE_WHEEL;
    }
    if (std::strcmp(requestedKey, modules::ui_color_keys::P2_PIECE_WHEEL) == 0) {
        return modules::ui_color_keys::P2_END_PIECE_WHEEL;
    }
    return requestedKey;
}

uint32_t ReadSettingUInt32(const fxe::SettingsStore& settings,
                           const char* key,
                           const uint32_t defaultValue) {
    if (key == nullptr) {
        return defaultValue;
    }
    if (IsPhysicalEndKey(key)) {
        const bool p2End = std::strcmp(
                               key,
                               modules::ui_color_keys::P2_END_PIECE_WHEEL) == 0;
        return ReadPhysicalEndWheel(settings, p2End);
    }
    if (IsLegacyLogicalKey(key)) {
        const char* const physicalKey = ResolvePhysicalStorageKey(settings, key);
        const bool p2End = std::strcmp(
                               physicalKey,
                               modules::ui_color_keys::P2_END_PIECE_WHEEL) == 0;
        return ReadPhysicalEndWheel(settings, p2End);
    }
    return settings.ReadValue(key, defaultValue);
}

bool CanonicalizeLoadedSettings(JsonDocument& doc) {
    const bool migrated = doc[MIGRATION_MARKER].is<uint32_t>() &&
                          doc[MIGRATION_MARKER].as<uint32_t>() == 1U;
    const bool hasPhysicalP1 = doc.containsKey(
        modules::ui_color_keys::P1_END_PIECE_WHEEL);
    const bool hasPhysicalP2 = doc.containsKey(
        modules::ui_color_keys::P2_END_PIECE_WHEEL);
    uint8_t physicalP1 = ReadDocumentWheel(
        doc,
        modules::ui_color_keys::P1_END_PIECE_WHEEL,
        P1_END_DEFAULT_WHEEL);
    uint8_t physicalP2 = ReadDocumentWheel(
        doc,
        modules::ui_color_keys::P2_END_PIECE_WHEEL,
        P2_END_DEFAULT_WHEEL);

    const uint8_t legacyP1 = ReadDocumentWheel(
        doc,
        modules::ui_color_keys::P1_PIECE_WHEEL,
        P1_END_DEFAULT_WHEEL);
    const uint8_t legacyP2 = ReadDocumentWheel(
        doc,
        modules::ui_color_keys::P2_PIECE_WHEEL,
        P2_END_DEFAULT_WHEEL);
    // First upgrade protects canonical colors from the old swapped aliases.
    // Once synchronized provenance has survived a save, a later alias-only
    // edit/reset belongs to downgraded firmware and must remain recoverable.
    // Missing aliases use their defaults, always for the same end slot.
    if (!hasPhysicalP1 || (migrated && legacyP1 != physicalP1)) physicalP1 = legacyP1;
    if (!hasPhysicalP2 || (migrated && legacyP2 != physicalP2)) physicalP2 = legacyP2;

    modules::ui_color_wheel::EnsureSeparated(physicalP1, physicalP2);
    bool mutated = false;
    mutated = SetWheel(doc,
                       modules::ui_color_keys::P1_END_PIECE_WHEEL,
                       physicalP1) ||
              mutated;
    mutated = SetWheel(doc,
                       modules::ui_color_keys::P2_END_PIECE_WHEEL,
                       physicalP2) ||
              mutated;

    mutated = SetWheel(doc,
                       modules::ui_color_keys::P1_PIECE_WHEEL,
                       physicalP1) ||
              mutated;
    mutated = SetWheel(doc,
                       modules::ui_color_keys::P2_PIECE_WHEEL,
                       physicalP2) ||
              mutated;
    if (!migrated) {
        doc[MIGRATION_MARKER] = 1U;
        mutated = true;
    }
    return mutated;
}

void SynchronizeLegacyAliases(JsonDocument& doc) {
    const uint8_t physicalP1 = ReadDocumentWheel(
        doc,
        modules::ui_color_keys::P1_END_PIECE_WHEEL,
        P1_END_DEFAULT_WHEEL);
    const uint8_t physicalP2 = ReadDocumentWheel(
        doc,
        modules::ui_color_keys::P2_END_PIECE_WHEEL,
        P2_END_DEFAULT_WHEEL);
    doc[modules::ui_color_keys::P1_PIECE_WHEEL] = static_cast<uint32_t>(physicalP1);
    doc[modules::ui_color_keys::P2_PIECE_WHEEL] = static_cast<uint32_t>(physicalP2);
}

} // namespace modules::player_end_palette
