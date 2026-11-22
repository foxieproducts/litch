#pragma once

#include <cstddef>
#include <cstdint>

#if defined(LITCH_BLE)
class NimBLECharacteristic;
#endif

namespace modules::ble_settings_async {

#if defined(LITCH_BLE)
// Broadcast responses must fit every connected peer, not the local preference.
size_t GetNotificationPayloadLimit(uint16_t connectionHandle = 0xffffU);
[[nodiscard]] bool SendAsyncResponse(NimBLECharacteristic& characteristic,
                       uint16_t subscriptionValue,
                       const char* payload,
                       size_t payloadLen,
                       bool ok,
                       const char* compactPayload = nullptr,
                       size_t compactPayloadLen = 0U,
                       uint16_t connectionHandle = 0xffffU);
#endif

} // namespace modules::ble_settings_async
