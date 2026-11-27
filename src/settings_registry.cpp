#include "modules/settings_registry.hpp"
#include "modules/assistance_preferences.hpp"
#include "modules/light_square_appearance.hpp"

#include <cstdint>
#include <cstring>

#include "modules/player_end_palette.hpp"
#include "modules/user_text.hpp"
#include "modules/ui_color_keys.hpp"
#include "modules/ui_color_wheel.hpp"

namespace modules::settings_registry {

namespace {

bool EqualsNoCase(const char* lhs, const char* rhs) {
    if (!lhs || !rhs) {
        return false;
    }

    size_t i = 0;
    for (;; ++i) {
        char a = lhs[i];
        char b = rhs[i];

        if (a >= 'a' && a <= 'z') {
            a = static_cast<char>(a - ('a' - 'A'));
        }
        if (b >= 'a' && b <= 'z') {
            b = static_cast<char>(b - ('a' - 'A'));
        }

        if (a != b) {
            return false;
        }
        if (a == '\0') {
            return true;
        }
    }
}

const fxe::SettingDescriptor* LookupByKeyNoCase(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return nullptr;
    }

    size_t descriptorCount = 0;
    const fxe::SettingDescriptor* descriptors = EnumerateAll(descriptorCount);
    for (size_t i = 0; i < descriptorCount; ++i) {
        const fxe::SettingDescriptor& descriptor = descriptors[i];
        if (EqualsNoCase(key, descriptor.key)) {
            return &descriptor;
        }
    }

    return nullptr;
}

bool TryReadUInt32(JsonVariantConst value, uint32_t& outValue) {
    if (value.is<uint32_t>()) {
        outValue = value.as<uint32_t>();
        return true;
    }

    if (value.is<uint64_t>()) {
        const uint64_t asU64 = value.as<uint64_t>();
        if (asU64 > static_cast<uint64_t>(UINT32_MAX)) {
            return false;
        }
        outValue = static_cast<uint32_t>(asU64);
        return true;
    }

    if (value.is<int64_t>()) {
        const int64_t asI64 = value.as<int64_t>();
        if (asI64 < 0 || asI64 > static_cast<int64_t>(UINT32_MAX)) {
            return false;
        }
        outValue = static_cast<uint32_t>(asI64);
        return true;
    }

    return false;
}

bool IsPieceWheelSettingKey(const char* key) {
    return modules::player_end_palette::IsLegacyLogicalKey(key) ||
           modules::player_end_palette::IsPhysicalEndKey(key);
}

const char* OtherPhysicalPieceWheelSettingKey(const char* storageKey) {
    if (std::strcmp(storageKey,
                    modules::ui_color_keys::P1_END_PIECE_WHEEL) == 0) {
        return modules::ui_color_keys::P2_END_PIECE_WHEEL;
    }
    if (std::strcmp(storageKey,
                    modules::ui_color_keys::P2_END_PIECE_WHEEL) == 0) {
        return modules::ui_color_keys::P1_END_PIECE_WHEEL;
    }
    return nullptr;
}

uint8_t DefaultPieceWheelValue(const char* key) {
    if (std::strcmp(key, modules::ui_color_keys::P1_PIECE_WHEEL) == 0 ||
        std::strcmp(key, modules::ui_color_keys::P1_END_PIECE_WHEEL) == 0) {
        return 170U;
    }
    if (std::strcmp(key, modules::ui_color_keys::P2_PIECE_WHEEL) == 0) {
        return 0U;
    }
    return 0U;
}

uint8_t ResolveSeparatedPieceWheelValue(const uint8_t proposed,
                                        const uint8_t other) {
    if (!modules::ui_color_wheel::IsTooClose(
            proposed, other, modules::ui_color_wheel::MIN_SEPARATION)) {
        return proposed;
    }

    const uint8_t offset = modules::ui_color_wheel::MIN_SEPARATION;
    const uint8_t lowerAllowed = static_cast<uint8_t>(other - offset);
    const uint8_t upperAllowed = static_cast<uint8_t>(other + offset);
    const uint8_t distanceToLower =
        modules::ui_color_wheel::CircularDistance(proposed, lowerAllowed);
    const uint8_t distanceToUpper =
        modules::ui_color_wheel::CircularDistance(proposed, upperAllowed);
    return (distanceToLower <= distanceToUpper) ? lowerAllowed : upperAllowed;
}

uint32_t CoercePieceWheelValueForPair(const fxe::SettingsStore& settings,
                                      const char* storageKey,
                                      const uint32_t proposedRaw) {
    if (!modules::player_end_palette::IsPhysicalEndKey(storageKey)) {
        return proposedRaw;
    }

    const char* otherKey = OtherPhysicalPieceWheelSettingKey(storageKey);
    if (otherKey == nullptr) {
        return proposedRaw;
    }

    const uint8_t proposed =
        modules::ui_color_wheel::ClampWheelValue01To255(proposedRaw);
    const uint8_t other = static_cast<uint8_t>(
        modules::player_end_palette::ReadSettingUInt32(
            settings, otherKey, DefaultPieceWheelValue(otherKey)));

    return static_cast<uint32_t>(ResolveSeparatedPieceWheelValue(proposed, other));
}

ValidateAndSetResult PrepareUInt32(const fxe::SettingsStore& settings,
                                   const fxe::SettingDescriptor& descriptor,
                                   JsonVariantConst value,
                                   PreparedSettingMutation& outMutation) {
    uint32_t numericValue = 0;
    if (!TryReadUInt32(value, numericValue)) {
        return ValidateAndSetResult::TypeMismatch;
    }

    if (numericValue < descriptor.min || numericValue > descriptor.max) {
        return ValidateAndSetResult::OutOfRange;
    }

    const char* const storageKey = outMutation.storageKey != nullptr
                                       ? outMutation.storageKey
                                       : descriptor.key;
    numericValue =
        CoercePieceWheelValueForPair(settings, storageKey, numericValue);
    outMutation.uintValue = numericValue;
    outMutation.changed = ReadCurrentUInt32(settings, descriptor) != numericValue;
    return ValidateAndSetResult::Ok;
}

ValidateAndSetResult PrepareBool(const fxe::SettingsStore& settings,
                                 const fxe::SettingDescriptor& descriptor,
                                 JsonVariantConst value,
                                 PreparedSettingMutation& outMutation) {
    bool boolValue = false;
    if (value.is<bool>()) {
        boolValue = value.as<bool>();
    } else {
        uint32_t numericValue = 0;
        if (!TryReadUInt32(value, numericValue)) {
            return ValidateAndSetResult::TypeMismatch;
        }
        if (numericValue > 1U) {
            return ValidateAndSetResult::OutOfRange;
        }
        boolValue = (numericValue != 0U);
    }

    outMutation.boolValue = boolValue;
    outMutation.changed =
        settings.ReadValue(descriptor.key, descriptor.defaultUint != 0U) != boolValue;
    return ValidateAndSetResult::Ok;
}

ValidateAndSetResult PrepareString(const fxe::SettingsStore& settings,
                                   const fxe::SettingDescriptor& descriptor,
                                   JsonVariantConst value,
                                   PreparedSettingMutation& outMutation) {
    if (!value.is<const char*>()) {
        return ValidateAndSetResult::TypeMismatch;
    }

    const char* strValue = value.as<const char*>();
    if (strValue == nullptr) {
        return ValidateAndSetResult::TypeMismatch;
    }
    const JsonString text = value.as<JsonString>();
    if (std::strcmp(descriptor.key, "BLE_DEVICE_NAME") == 0 &&
        !user_text::IsSafeDisplayName(
            strValue, text.size(), 20U, 31U)) {
        return ValidateAndSetResult::OutOfRange;
    }
    for (size_t i = 0U; i < text.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(strValue[i]);
        if (byte < 32U || byte == 127U) {
            return ValidateAndSetResult::OutOfRange;
        }
    }

    const char* defaultValue = descriptor.defaultStr != nullptr
                                   ? descriptor.defaultStr
                                   : "";
    const char* currentValue = settings.ReadValue(descriptor.key, defaultValue);
    outMutation.stringValue = strValue;
    outMutation.changed = currentValue == nullptr ||
                          std::strcmp(currentValue, strValue) != 0;
    return ValidateAndSetResult::Ok;
}

ValidateAndSetResult PrepareEnum(const fxe::SettingsStore& settings,
                                 const fxe::SettingDescriptor& descriptor,
                                 JsonVariantConst value,
                                 PreparedSettingMutation& outMutation) {
    if (!value.is<const char*>()) {
        return ValidateAndSetResult::TypeMismatch;
    }

    const char* strValue = value.as<const char*>();
    if (strValue == nullptr || descriptor.allowedValues == nullptr || descriptor.allowedCount == 0U) {
        return ValidateAndSetResult::InvalidEnumValue;
    }
    if (std::strlen(strValue) != value.as<JsonString>().size()) {
        return ValidateAndSetResult::InvalidEnumValue;
    }

    bool found = false;
    for (size_t i = 0; i < descriptor.allowedCount; ++i) {
        const char* allowed = descriptor.allowedValues[i];
        if (allowed != nullptr && std::strcmp(allowed, strValue) == 0) {
            found = true;
            break;
        }
    }

    if (!found) {
        return ValidateAndSetResult::InvalidEnumValue;
    }

    const char* defaultValue = descriptor.defaultStr != nullptr
                                   ? descriptor.defaultStr
                                   : "";
    const char* currentValue = settings.ReadValue(descriptor.key, defaultValue);
    outMutation.stringValue = strValue;
    outMutation.changed = currentValue == nullptr ||
                          std::strcmp(currentValue, strValue) != 0;
    return ValidateAndSetResult::Ok;
}

} // namespace

const char* ValidateAndSetResultToString(const ValidateAndSetResult result) {
    switch (result) {
        case ValidateAndSetResult::Ok:
            return "ok";
        case ValidateAndSetResult::UnknownKey:
            return "unknown_key";
        case ValidateAndSetResult::TypeMismatch:
            return "type_mismatch";
        case ValidateAndSetResult::OutOfRange:
            return "out_of_range";
        case ValidateAndSetResult::InvalidEnumValue:
            return "invalid_enum_value";
    }

    return "unknown_error";
}

const fxe::SettingDescriptor* LookupByKey(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return nullptr;
    }

    size_t descriptorCount = 0;
    const fxe::SettingDescriptor* descriptors = EnumerateAll(descriptorCount);
    for (size_t i = 0; i < descriptorCount; ++i) {
        const fxe::SettingDescriptor& descriptor = descriptors[i];
        if (std::strcmp(key, descriptor.key) == 0) {
            return &descriptor;
        }
    }

    return nullptr;
}

bool UsesMustSaveTier(const char* key) {
    const fxe::SettingDescriptor* descriptor = LookupByKey(key);
    if (descriptor == nullptr) {
        descriptor = LookupByKeyNoCase(key);
    }
    return descriptor != nullptr && descriptor->IsForceSave();
}

bool IsDirectAppAccessible(const fxe::SettingDescriptor& descriptor) {
    return !descriptor.IsRetired() &&
           ((descriptor.IsVisibleToApp() && !descriptor.IsInternal()) ||
           modules::player_end_palette::IsPhysicalEndKey(descriptor.key) ||
           modules::assistance_preferences::IsKey(descriptor.key) ||
           std::strcmp(descriptor.key, "BLE_DEVICE_NAME") == 0);
}

uint32_t ReadCurrentUInt32(const fxe::SettingsStore& settings,
                           const fxe::SettingDescriptor& descriptor) {
    if (std::strcmp(descriptor.key, light_square_appearance::KEY_BRIGHTNESS) == 0)
        return light_square_appearance::ReadBrightness(settings);
    return modules::player_end_palette::ReadSettingUInt32(
        settings, descriptor.key, descriptor.defaultUint);
}

bool CanonicalizeLoadedSettings(JsonDocument& doc) {
    bool mutated = modules::player_end_palette::CanonicalizeLoadedSettings(doc);
    // Saved preferences keep their ordinal meaning through the replacement.
    // Mutation ingress accepts only the current catalogue, never old-peer aliases.
    const char* placement = doc["SETTLE_ANIM_STYLE"] | "";
    if (std::strcmp(placement, "glow") == 0 || std::strcmp(placement, "pulse") == 0) {
        doc["SETTLE_ANIM_STYLE"] = std::strcmp(placement, "glow") == 0
            ? "landing_flash" : "mini_orbit";
        mutated = true;
    }

    size_t descriptorCount = 0;
    const fxe::SettingDescriptor* descriptors = EnumerateAll(descriptorCount);
    for (size_t i = 0; i < descriptorCount; ++i) {
        const fxe::SettingDescriptor& descriptor = descriptors[i];
        if (descriptor.IsRetired()) {
            if (doc.containsKey(descriptor.key)) {
                doc.remove(descriptor.key);
                mutated = true;
            }
            continue;
        }
        if (descriptor.type != fxe::SettingType::Bool) {
            continue;
        }

        if (!doc.containsKey(descriptor.key)) {
            continue;
        }

        const JsonVariant value = doc[descriptor.key];
        if (value.is<bool>()) {
            continue;
        }

        uint32_t numericValue = 0;
        if (!TryReadUInt32(value.as<JsonVariantConst>(), numericValue) ||
            numericValue > 1U) {
            continue;
        }

        doc[descriptor.key] = (numericValue != 0U);
        mutated = true;
    }

    return mutated;
}

ValidateAndSetResult PrepareMutation(const fxe::SettingsStore& settings,
                                     const char* key,
                                     const JsonVariantConst value,
                                     PreparedSettingMutation& outMutation) {
    outMutation = PreparedSettingMutation{};
    const fxe::SettingDescriptor* descriptor = LookupByKey(key);
    if (descriptor == nullptr) {
        descriptor = LookupByKeyNoCase(key);
    }
    if (descriptor == nullptr || descriptor->IsRetired()) {
        return ValidateAndSetResult::UnknownKey;
    }
    outMutation.descriptor = descriptor;
    outMutation.storageKey =
        modules::player_end_palette::ResolvePhysicalStorageKey(
            settings, descriptor->key);

    switch (descriptor->type) {
        case fxe::SettingType::UInt32:
            return PrepareUInt32(settings, *descriptor, value, outMutation);
        case fxe::SettingType::Bool:
            return PrepareBool(settings, *descriptor, value, outMutation);
        case fxe::SettingType::String:
            return PrepareString(settings, *descriptor, value, outMutation);
        case fxe::SettingType::Enum:
            return PrepareEnum(settings, *descriptor, value, outMutation);
    }

    return ValidateAndSetResult::TypeMismatch;
}

void ApplyPreparedMutation(fxe::SettingsStore& settings,
                           const PreparedSettingMutation& mutation) {
    if (mutation.descriptor == nullptr) {
        return;
    }

    const fxe::SettingDescriptor& descriptor = *mutation.descriptor;
    const char* const storageKey = mutation.storageKey != nullptr
                                       ? mutation.storageKey
                                       : descriptor.key;
    if (std::strcmp(descriptor.key, "BRIGHTNESS") == 0 && mutation.uintValue == 0U &&
        !settings.Doc().containsKey("WHITE_SQUARES_ON")) {
        // A new zero is near-dark; only older zero records without this flag
        // mean that light-square backgrounds were disabled.
        settings.Doc()["WHITE_SQUARES_ON"] = 1U;
    }
    switch (descriptor.type) {
        case fxe::SettingType::UInt32:
            if (std::strcmp(storageKey, light_square_appearance::KEY_BRIGHTNESS) == 0) {
                settings.Doc()[storageKey] = light_square_appearance::StoredBrightness(mutation.uintValue);
                return;
            }
            settings.Doc()[storageKey] = mutation.uintValue;
            if (IsPieceWheelSettingKey(descriptor.key) ||
                std::strcmp(descriptor.key, "GAME_FIRST_PLAYER") == 0) {
                modules::player_end_palette::SynchronizeLegacyAliases(
                    settings.Doc());
            }
            return;
        case fxe::SettingType::Bool:
            settings.Doc()[storageKey] = mutation.boolValue;
            return;
        case fxe::SettingType::String:
        case fxe::SettingType::Enum:
            settings.Doc()[storageKey].set(
                const_cast<char*>(mutation.stringValue != nullptr
                                      ? mutation.stringValue
                                      : ""));
            return;
    }
}

ValidateAndSetResult ValidateAndSet(fxe::SettingsStore& settings,
                                    const char* key,
                                    const JsonVariantConst value) {
    PreparedSettingMutation mutation;
    const ValidateAndSetResult result =
        PrepareMutation(settings, key, value, mutation);
    if (result == ValidateAndSetResult::Ok) {
        ApplyPreparedMutation(settings, mutation);
    }
    return result;
}

} // namespace modules::settings_registry
