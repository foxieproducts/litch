#include "modules/ble_service.hpp"

#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#endif

#include <cstring>

namespace modules {

#if defined(LITCH_BLE)
bool BleService::HandleRuntimeHealthRequest(const char* const data,
                                            const size_t len,
                                            const uint32_t connectionEpoch) {
    if (ble_runtime_health_protocol::IsHealthRequest(data, len)) {
        if (m_bleSettings.OwnsConnectionEpoch(connectionEpoch)) {
            NotifyRuntimeHealth(connectionEpoch);
        }
        return true;
    }

    if (!ble_runtime_health_protocol::IsHealthRecheckRequest(data, len)) {
        return false;
    }

    if (!m_bleSettings.OwnsConnectionEpoch(connectionEpoch)) {
        return true;
    }

    const bool succeeded = m_runtimeHealthRecheckCallback != nullptr &&
                           m_runtimeHealthRecheckCallback(
                               m_runtimeHealthRecheckContext);
    NotifyRuntimeHealthRecheckResult(succeeded, connectionEpoch);
    return true;
}

void BleService::NotifyRuntimeHealth(const uint32_t connectionEpoch) {
    if (m_runtimeHealth == nullptr || m_otaControlCharacteristic == nullptr ||
        !m_bleSettings.OwnsConnectionEpoch(connectionEpoch)) {
        return;
    }

    char payload[ble_runtime_health_protocol::HEALTH_RESPONSE_BYTES + 1U] = {};
    if (!ble_runtime_health_protocol::FormatHealthResponse(
            m_runtimeHealth->Snapshot(), payload, sizeof(payload))) {
        return;
    }

    if (!m_connected.load(std::memory_order_acquire) ||
        !m_bleSettings.OwnsConnectionEpoch(connectionEpoch)) {
        return;
    }

    (void)NotifyOtaJson(payload);
}

void BleService::NotifyRuntimeHealthRecheckResult(
    const bool succeeded,
    const uint32_t connectionEpoch) {
    if (m_otaControlCharacteristic == nullptr ||
        !m_bleSettings.OwnsConnectionEpoch(connectionEpoch) ||
        !m_connected.load(std::memory_order_acquire)) {
        return;
    }

    const char* const payload =
        succeeded
            ? ble_runtime_health_protocol::HEALTH_RECHECK_SUCCEEDED
            : ble_runtime_health_protocol::HEALTH_RECHECK_FAILED;
    if (m_bleSettings.OwnsConnectionEpoch(connectionEpoch) &&
        m_connected.load(std::memory_order_acquire)) {
        (void)NotifyOtaJson(payload);
    }
}
#endif

} // namespace modules
