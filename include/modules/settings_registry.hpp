#pragma once

#include <cstddef>
#include <cstdint>

#include "foxie_embedded/setting_descriptor.hpp"
#include "foxie_embedded/settings_store.hpp"

namespace modules::settings_registry {

enum class ValidateAndSetResult : uint8_t {
    Ok = 0,
    UnknownKey,
    TypeMismatch,
    OutOfRange,
    InvalidEnumValue,
};

struct PreparedSettingMutation {
    const fxe::SettingDescriptor* descriptor = nullptr;
    const char* storageKey = nullptr;
    uint32_t uintValue = 0U;
    bool boolValue = false;
    const char* stringValue = nullptr;
    bool changed = false;
};

[[nodiscard]] const char* ValidateAndSetResultToString(ValidateAndSetResult result);

[[nodiscard]] const fxe::SettingDescriptor* EnumerateAll(size_t& outCount);

[[nodiscard]] const fxe::SettingDescriptor* LookupByKey(const char* key);

[[nodiscard]] bool UsesMustSaveTier(const char* key);

// Hidden physical-end palette keys are available only to direct app reads and
// writes. They stay out of enumeration so older apps retain one color control
// per logical player instead of displaying duplicate controls.
[[nodiscard]] bool IsDirectAppAccessible(const fxe::SettingDescriptor& descriptor);

[[nodiscard]] uint32_t ReadCurrentUInt32(
    const fxe::SettingsStore& settings,
    const fxe::SettingDescriptor& descriptor);

[[nodiscard]] bool CanonicalizeLoadedSettings(JsonDocument& doc);

[[nodiscard]] ValidateAndSetResult PrepareMutation(
    const fxe::SettingsStore& settings,
    const char* key,
    JsonVariantConst value,
    PreparedSettingMutation& outMutation);

void ApplyPreparedMutation(fxe::SettingsStore& settings,
                           const PreparedSettingMutation& mutation);

[[nodiscard]] ValidateAndSetResult ValidateAndSet(fxe::SettingsStore& settings,
                                                  const char* key,
                                                  JsonVariantConst value);

} // namespace modules::settings_registry
