#include "modules/ble_service.hpp"

#include <algorithm>

#if defined(LITCH_BLE)
#include <Arduino.h>
#include <NimBLEDevice.h>
#endif

#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/ble_service_internal.hpp"

namespace modules {

void BleService::SetHardwareRevision(
    const ble_device_capabilities::HardwareRevision revision) {
    m_hardwareRevision = revision;
}

void BleService::Begin(const SettingsStore& settings) {
#if defined(LITCH_BLE)
    if (m_recoveryOnly) return;
    m_settings = const_cast<SettingsStore*>(&settings);
    BeginQueues();
    InitStack(&settings);
#else
    (void)settings;
#endif
}

void BleService::BeginRecovery() {
#if defined(LITCH_BLE)
    if (m_initialized) return;
    m_recoveryOnly = true;
    m_settings = nullptr;
    m_litchcraftGame = nullptr;
#if defined(LITCH_BLE_OTA)
    m_otaStoreSaveRequest = nullptr;
    m_otaStoreSavePending = nullptr;
    m_otaStoreBeginBusy = nullptr;
    m_litchcraftFrameIngressCallback = nullptr;
#endif
    BeginQueues();
    InitStack(nullptr);
#endif
}

#if defined(LITCH_BLE)
void BleService::BeginQueues() {
    if (!m_eventQueue.Begin()) {
        logger::Warn("ble: event queue init failed");
    }
    if (!m_connectionProjection.Begin()) {
        logger::Warn("ble: connection projection init failed");
    }
#if defined(LITCH_BLE_OTA)
    if (!m_otaChunkQueue.Begin()) {
        logger::Warn("ble: ota queue init failed");
    }
#endif
}
#endif

void BleService::SetSettingsMutationCallback(
    const SettingsMutationCallback callback,
    void* const context) {
    m_settingsMutationCallback = callback;
    m_settingsMutationContext = context;
}

void BleService::InvokeSettingsMutationCallback(const char* const key) {
    if (m_settingsMutationCallback != nullptr) {
        m_settingsMutationCallback(m_settingsMutationContext, key);
    }
}


#if defined(LITCH_BLE)
bool BleService::ConfigureAdvertising() {
    using namespace ble_internal;
    m_advertising->clearData();
    m_advertising->enableScanResponse(true);
    if (!m_advertising->addServiceUUID(BASE_SERVICE_UUID) ||
        (m_factoryVerifierEndpointEnabled &&
         !m_advertising->addServiceUUID(FACTORY_RADIO_SERVICE_UUID))) return false;

    // Keep the discovery UUID in the primary packet and reserve the response
    // for the name. Legacy AD fields have two bytes of overhead. Stored names
    // are validated UTF-8; back up from a cut inside a continuation sequence.
    constexpr size_t MAX_ADVERTISED_NAME_BYTES = 29U;
    const auto name = m_deviceName.View();
    size_t bytes = std::min(name.size(), MAX_ADVERTISED_NAME_BYTES);
    if (bytes < name.size()) {
        while (bytes > 0U && (static_cast<uint8_t>(name[bytes]) & 0xc0U) == 0x80U) --bytes;
    }
    std::array<uint8_t, MAX_ADVERTISED_NAME_BYTES + 2U> field{};
    field[0U] = static_cast<uint8_t>(bytes + 1U);
    field[1U] = bytes == name.size() ? 0x09U : 0x08U; // Complete/Shortened Local Name.
    std::copy_n(name.begin(), bytes, field.begin() + 2U);
    // SDK-owned payload allocation is bounded to one legacy packet. NimBLE
    // copies it into the advertising owner and releases that copy at teardown.
    NimBLEAdvertisementData response;
    if (!response.addData(field.data(), bytes + 2U) ||
        !m_advertising->setScanResponseData(response)) return false;

    // Secondary service discovery is optional; it must never displace a name.
    if (!m_recoveryOnly) (void)m_advertising->addServiceUUID(SETTINGS_SERVICE_UUID);
    return true;
}
#endif

void BleService::StartAdvertising() {
#if defined(LITCH_BLE)
    if (m_ownerQuiescing.load(std::memory_order_acquire) || !m_initialized || !m_enabled ||
        m_connected.load(std::memory_order_acquire) || m_advertising == nullptr) {
        return;
    }

    // A callback can publish between lifecycle drain and Update. Inspect its
    // synchronized projection without consuming the main loop's activation.
    BleConnectionChangeEvent pendingConnection;
    if (m_connectionProjection.TryPeek(pendingConnection) &&
        pendingConnection.connected &&
        pendingConnection.connectionEpoch ==
            m_bleSettings.CurrentConnectionEpoch()) {
        return;
    }

    // Advertising can stop independently during host recovery. The main loop
    // retries through this shared quiescence and lifecycle gate.
    if (m_advertising->isAdvertising()) {
        return;
    }

    if (m_advertising->start()) {
        logger::Info("ble: advertising started");
    } else {
        logger::Warn("ble: advertising start failed");
    }
#endif
}

bool BleService::Stop() {
#if defined(LITCH_BLE)
    const bool stopped = ShutdownStack();
#else
    const bool stopped = true;
#endif
    m_enabled = false;
    return stopped;
}

bool BleService::IsEnabled() const {
    return m_enabled;
}

const char* BleService::GetDeviceName() const {
    return m_deviceName.CStr();
}

bool BleService::ConsumeIdentifyStart(uint8_t& outNumber,
                                      uint8_t& outRed,
                                      uint8_t& outGreen,
                                      uint8_t& outBlue,
                                      bool& outPadTwoDigits,
                                      uint32_t& outDurationMs) {
#if defined(LITCH_BLE)
    if (!m_identifyStartPending) {
        return false;
    }

    m_identifyStartPending = false;
    outNumber = m_identifyNumber;
    outRed = m_identifyRed;
    outGreen = m_identifyGreen;
    outBlue = m_identifyBlue;
    outPadTwoDigits = m_identifyPadTwoDigits;
    outDurationMs = m_identifyDurationMs;
    return true;
#else
    (void)outNumber;
    (void)outRed;
    (void)outGreen;
    (void)outBlue;
    (void)outPadTwoDigits;
    (void)outDurationMs;
    return false;
#endif
}

bool BleService::ConsumeIdentifyStop() {
#if defined(LITCH_BLE)
    if (!m_identifyStopPending) {
        return false;
    }

    m_identifyStopPending = false;
    return true;
#else
    return false;
#endif
}

} // namespace modules
