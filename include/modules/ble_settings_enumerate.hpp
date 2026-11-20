#pragma once

#include "modules/settings_store.hpp"

#if defined(LITCH_BLE)
class NimBLECharacteristic;
#endif

namespace modules::ble_settings_enumerate {

#if defined(LITCH_BLE)
void BuildResponse(const SettingsStore& settings,
                   NimBLECharacteristic& characteristic,
                   size_t startIndex,
                   size_t& outNextIndex,
                   bool& outDone);
#endif

} // namespace modules::ble_settings_enumerate