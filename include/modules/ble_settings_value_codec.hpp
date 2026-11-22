#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

#include "foxie_embedded/setting_descriptor.hpp"
#include "modules/settings_store.hpp"

namespace modules::ble_settings_value_codec {

bool IsAppVisible(const fxe::SettingDescriptor& descriptor);
bool TryResolveVisibleSettingIndex(const char* key, uint16_t& outIndex);
bool WriteCurrentValue(const SettingsStore& settings,
                       const fxe::SettingDescriptor& descriptor,
                       JsonVariant valueVariant);
bool SerializeCurrentValueForSnapshot(const SettingsStore& settings,
                                      const fxe::SettingDescriptor& descriptor,
                                      char* outValue,
                                      size_t outCapacity);

} // namespace modules::ble_settings_value_codec
