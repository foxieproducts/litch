#include "modules/ble_settings_value_codec.hpp"

#include <cstdio>
#include <cstring>

#include "modules/settings_registry.hpp"

namespace modules::ble_settings_value_codec {

bool IsAppVisible(const fxe::SettingDescriptor& descriptor) {
    return descriptor.IsVisibleToApp() && !descriptor.IsInternal() &&
           !descriptor.IsRetired();
}

bool TryResolveVisibleSettingIndex(const char* key, uint16_t& outIndex) {
    if (key == nullptr || key[0] == '\0') {
        return false;
    }

    size_t descriptorCount = 0;
    const fxe::SettingDescriptor* descriptors = modules::settings_registry::EnumerateAll(descriptorCount);
    size_t visibleIndex = 0;
    for (size_t i = 0; descriptors != nullptr && i < descriptorCount; ++i) {
        const fxe::SettingDescriptor& descriptor = descriptors[i];
        if (!descriptor.IsVisibleToApp() || descriptor.IsInternal()) {
            continue;
        }

        if (!descriptor.IsRetired() && std::strcmp(descriptor.key, key) == 0) {
            if (visibleIndex > 0xFFFFU) {
                return false;
            }
            outIndex = static_cast<uint16_t>(visibleIndex);
            return true;
        }

        ++visibleIndex;
    }

    return false;
}

bool WriteCurrentValue(const SettingsStore& settings,
                       const fxe::SettingDescriptor& descriptor,
                       JsonVariant valueVariant) {
    switch (descriptor.type) {
        case fxe::SettingType::UInt32:
            valueVariant.set(modules::settings_registry::ReadCurrentUInt32(
                settings, descriptor));
            return true;
        case fxe::SettingType::Bool: {
            const bool defaultBool = descriptor.defaultUint != 0U;
            valueVariant.set(settings.ReadValue(descriptor.key, defaultBool));
            return true;
        }
        case fxe::SettingType::String:
        case fxe::SettingType::Enum: {
            const char* defaultStr = (descriptor.defaultStr != nullptr) ? descriptor.defaultStr : "";
            valueVariant.set(settings.ReadValue(descriptor.key, defaultStr));
            return true;
        }
    }

    return false;
}

bool SerializeCurrentValueForSnapshot(const SettingsStore& settings,
                                      const fxe::SettingDescriptor& descriptor,
                                      char* outValue,
                                      const size_t outCapacity) {
    if (outValue == nullptr || outCapacity == 0U) {
        return false;
    }

    switch (descriptor.type) {
        case fxe::SettingType::UInt32: {
            const uint32_t value = modules::settings_registry::ReadCurrentUInt32(
                settings, descriptor);
            const int written = std::snprintf(outValue, outCapacity, "%lu", static_cast<unsigned long>(value));
            return written > 0 && static_cast<size_t>(written) < outCapacity;
        }
        case fxe::SettingType::Bool: {
            const bool defaultBool = descriptor.defaultUint != 0U;
            const bool value = settings.ReadValue(descriptor.key, defaultBool);
            const char* token = value ? "true" : "false";
            const size_t len = std::strlen(token);
            if (len >= outCapacity) {
                return false;
            }
            std::memcpy(outValue, token, len + 1U);
            return true;
        }
        case fxe::SettingType::String:
        case fxe::SettingType::Enum: {
            const char* defaultStr = (descriptor.defaultStr != nullptr) ? descriptor.defaultStr : "";
            const char* value = settings.ReadValue(descriptor.key, defaultStr);
            if (value == nullptr) {
                value = "";
            }
            const size_t len = std::strlen(value);
            if (len >= outCapacity) {
                return false;
            }
            std::memcpy(outValue, value, len + 1U);
            return true;
        }
    }

    return false;
}

} // namespace modules::ble_settings_value_codec
