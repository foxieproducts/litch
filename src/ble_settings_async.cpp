#include "modules/ble_settings_async.hpp"

#if defined(LITCH_BLE)

#include <cstring>

#include <NimBLEDevice.h>

namespace modules::ble_settings_async {

size_t GetNotificationPayloadLimit(uint16_t connectionHandle) {
    const NimBLEServer* const server = NimBLEDevice::getServer();
    if (server == nullptr) return 20U;
    if (connectionHandle != 0xffffU) {
        const uint16_t mtu = server->getPeerMTU(connectionHandle);
        return mtu >= 23U ? mtu - 3U : 20U;
    }
    // The SDK's indexed peer-info lookup can address the wrong slot after a
    // disconnect. Enumerate actual handles; this bounded SDK copy is only on
    // the settings-response path, never the per-frame console drain.
    const auto peers = server->getPeerDevices();
    if (peers.empty()) return 20U;
    size_t limit = 512U;
    for (const uint16_t handle : peers) {
        const uint16_t mtu = server->getPeerMTU(handle);
        const size_t payload = mtu >= 23U ? mtu - 3U : 20U;
        if (payload < limit) limit = payload;
    }
    return limit;
}

bool SendAsyncResponse(NimBLECharacteristic& characteristic,
                       const uint16_t subscriptionValue,
                       const char* payload,
                       const size_t payloadLen,
                       const bool ok,
                       const char* compactPayload,
                       const size_t compactPayloadLen,
                       const uint16_t connectionHandle) {
    if ((subscriptionValue & 0x03U) == 0U) {
        return false;
    }

    const size_t maxAsyncLen = GetNotificationPayloadLimit(connectionHandle);

    const char* txPayload = payload;
    size_t txLen = payloadLen;
    if (txPayload == nullptr || txLen == 0U || txLen > maxAsyncLen) {
        if (compactPayload != nullptr && compactPayloadLen != 0U &&
            compactPayloadLen <= maxAsyncLen) {
            txPayload = compactPayload;
            txLen = compactPayloadLen;
        } else {
            txPayload = ok ? "{\"ok\":true}" : "{\"ok\":false}";
            txLen = std::strlen(txPayload);
        }
    }

    if ((subscriptionValue & 0x02U) != 0U) {
        return characteristic.indicate(reinterpret_cast<const uint8_t*>(txPayload), txLen, connectionHandle);
    } else if ((subscriptionValue & 0x01U) != 0U) {
        return characteristic.notify(reinterpret_cast<const uint8_t*>(txPayload), txLen, connectionHandle);
    }
    return false;
}

} // namespace modules::ble_settings_async

#endif
