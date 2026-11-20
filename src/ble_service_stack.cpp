#include "modules/ble_service.hpp"
#include "modules/game_persistence_coordinator.hpp"

#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#if defined(ARDUINO)
#include <nimble/nimble/host/services/gap/include/services/gap/ble_svc_gap.h>
#endif
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
#include <esp_bt.h>
#endif
#endif
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/ble_service_internal.hpp"

namespace modules {

#if defined(LITCH_BLE)
namespace {

using namespace ble_internal;

class BleServerCallbacks final : public NimBLEServerCallbacks {
  public:
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        if (g_bleService == nullptr) {
            return;
        }

        if (!g_bleService->OwnerAcceptsNewConnection(connInfo.getConnHandle()) && pServer != nullptr) {
            pServer->disconnect(connInfo.getConnHandle());
            return;
        }
        const auto address = connInfo.getAddress();
        const uint8_t* const bytes = address.getVal();
        char peerAddress[18];
        std::snprintf(peerAddress, sizeof(peerAddress), "%02x:%02x:%02x:%02x:%02x:%02x",
                      bytes[5], bytes[4], bytes[3], bytes[2], bytes[1], bytes[0]);
        const uint16_t mtu = static_cast<uint16_t>(connInfo.getMTU());
        uint8_t connectionCount = 0;
        if (pServer != nullptr) {
            connectionCount = static_cast<uint8_t>(pServer->getConnectedCount());
        }

        g_bleService->OnConnected(connInfo.getConnHandle(), peerAddress, mtu, connectionCount);
        g_bleService->OnConnectionParametersChanged(
            connInfo.getConnInterval(),
            connInfo.getConnLatency(),
            connInfo.getConnTimeout(),
            true, connInfo.getConnHandle());
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        (void)pServer;
        (void)connInfo;
        if (g_bleService != nullptr) {
            g_bleService->OnDisconnected(connInfo.getConnHandle(), reason);
        }
    }

    void onConnParamsUpdate(NimBLEConnInfo& connInfo) override {
        if (g_bleService == nullptr) {
            return;
        }
        g_bleService->OnConnectionParametersChanged(
            connInfo.getConnInterval(), connInfo.getConnLatency(),
            connInfo.getConnTimeout(), false, connInfo.getConnHandle());
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
        if (g_bleService) g_bleService->OnLinkObservation(info.getConnHandle(), mtu, 0U, 0U);
    }
    void onPhyUpdate(NimBLEConnInfo& info, uint8_t txPhy, uint8_t rxPhy) override {
        if (g_bleService) g_bleService->OnLinkObservation(info.getConnHandle(), 0U, txPhy, rxPhy, true);
    }
};

class CommandCharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
  public:
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        if (g_bleService == nullptr || characteristic == nullptr ||
            !g_bleService->OwnerCallbackAllowed(connInfo.getConnHandle())) {
            return;
        }

        const auto value = characteristic->getValue();
        if (value.size() != 0U) {
            g_bleService->OnCommandCharacteristicWrite(value.c_str(), value.size());
        }
    }
};

class OtaControlCharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
  public:
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        if (g_bleService == nullptr || characteristic == nullptr ||
            !g_bleService->OwnerCallbackAllowed(connInfo.getConnHandle(), true)) {
            return;
        }

        const auto value = characteristic->getValue();
        if (value.size() != 0U) {
            g_bleService->OnOtaControlCharacteristicWrite(value.c_str(), value.size());
        }
    }

    void onSubscribe(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo, uint16_t subValue) override {
        (void)characteristic;
        (void)connInfo;
        if (g_bleService != nullptr && g_bleService->OwnerCallbackAllowed(connInfo.getConnHandle(), true)) {
            g_bleService->OnOtaControlSubscriptionChanged(subValue);
        }
    }
};

#if defined(LITCH_BLE_OTA)
class OtaDataCharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
  public:
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        if (g_bleService == nullptr || characteristic == nullptr ||
            !g_bleService->OwnerCallbackAllowed(connInfo.getConnHandle())) {
            return;
        }

        // NimBLE already returns an owned value snapshot. Keep that snapshot
        // through ingress instead of allocating a second std::string per chunk.
        const auto value = characteristic->getValue();
        if (value.size() != 0U) {
            const auto* const bytes =
                reinterpret_cast<const uint8_t*>(value.data());
            (void)g_bleService->OnOtaDataCharacteristicWrite(
                bytes, value.size());
        }
    }
};
#endif

class LitchcraftCharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
  public:
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
        if (!g_bleService || !characteristic ||
            !g_bleService->OwnerCallbackAllowed(connInfo.getConnHandle())) return;
        const auto value=characteristic->getValue();
        g_bleService->OnLitchcraftWrite(reinterpret_cast<const uint8_t*>(value.data()),value.size());
    }
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& connInfo, uint16_t value) override {
        if (g_bleService && g_bleService->OwnerCallbackAllowed(connInfo.getConnHandle(), true)) g_bleService->OnLitchcraftSubscription(value);
    }
};
LitchcraftCharacteristicCallbacks g_litchcraftCallbacks;

class ConnectionSessionIngressCallbacks final
    : public NimBLECharacteristicCallbacks {
  public:
    void onWrite(NimBLECharacteristic* characteristic,
                 NimBLEConnInfo& connInfo) override {
        if (g_bleService == nullptr || characteristic == nullptr ||
            !g_bleService->OwnerCallbackAllowed(connInfo.getConnHandle(), true)) return;
        const auto value = characteristic->getValue();
        g_bleService->OnConnectionSessionWrite(
            reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
};

class ConnectionSessionEgressCallbacks final
    : public NimBLECharacteristicCallbacks {
  public:
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& connInfo,
                     const uint16_t value) override {
        if (g_bleService != nullptr && g_bleService->OwnerCallbackAllowed(connInfo.getConnHandle(), true)) {
            g_bleService->OnConnectionSessionSubscription(value);
        }
    }
};

ConnectionSessionIngressCallbacks g_connectionSessionIngressCallbacks;
ConnectionSessionEgressCallbacks g_connectionSessionEgressCallbacks;

class ConsoleStreamCharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
  public:
    void onSubscribe(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo, uint16_t subValue) override {
        (void)characteristic;
        (void)connInfo;
        if (g_bleService != nullptr && g_bleService->OwnerCallbackAllowed(connInfo.getConnHandle(), true)) {
            g_bleService->OnConsoleStreamSubscriptionChanged(subValue);
        }
    }
};

BleServerCallbacks g_callbacks;
CommandCharacteristicCallbacks g_commandCallbacks;
ConsoleStreamCharacteristicCallbacks g_consoleStreamCallbacks;
OtaControlCharacteristicCallbacks g_otaControlCallbacks;
#if defined(LITCH_BLE_OTA)
OtaDataCharacteristicCallbacks g_otaDataCallbacks;
#endif

} // namespace

void BleService::InitStack(const SettingsStore* settings) {
    using namespace ble_internal;

    if (m_initialized) {
        return;
    }
    if (g_bleService == this) {
        ShutdownStack();
        if (g_bleService == this) return;
    }

    if (!m_recoveryOnly &&
        (settings == nullptr || (!ReadEnabledSetting(*settings) && !OwnerPairingEnabled()))) {
        m_enabled = false;
        return;
    }

    m_settings = m_recoveryOnly ? nullptr : const_cast<SettingsStore*>(settings);
    m_deviceName = m_recoveryOnly
        ? fxe::FixedString<32>(RECOVERY_DEVICE_NAME) : ReadDeviceNameSetting(*settings);
    m_enabled = m_recoveryOnly || ReadEnabledSetting(*settings);

    // Native restore can access NVS before host synchronization returns.
    g_bleService = this;
    // Set the owned name directly in GAP's bounded buffer before registering
    // a server or starting advertising. NimBLE's C++ name argument owns a string.
    if (!NimBLEDevice::init({}) || ble_svc_gap_device_name_set(m_deviceName.CStr()) != 0) {
        logger::Warn("ble: NimBLE init failed");
        if (StopNativeStack()) g_bleService = nullptr;
        m_enabled = false;
        return;
    }

    (void)NimBLEDevice::setMTU(BLE_PREFERRED_MTU);
    (void)NimBLEDevice::setPower(BLE_TX_POWER_DBM);

    m_server = NimBLEDevice::createServer();
    if (m_server == nullptr) {
        logger::Warn("ble: createServer failed");
        // A failed construction cannot leave retained SDK objects for the next
        // initialization attempt. Retaining them creates duplicate/stale GAP
        // state, including the prior advertising name.
        if (StopNativeStack()) g_bleService = nullptr;
        m_enabled = false;
        return;
    }

    m_server->setCallbacks(&g_callbacks, false);

    m_baseService = m_server->createService(BASE_SERVICE_UUID);
    if (m_baseService == nullptr) {
        logger::Warn("ble: createService failed");
        ShutdownStack();
        return;
    }

    // Bonded phones can retain base attribute handles across a normal/recovery
    // reboot. Keep this existing skeleton stable; callbacks and capability
    // admission, not absence of attributes, close recovery's domain access.
    {
        m_commandCharacteristic = m_baseService->createCharacteristic(
            COMMAND_CHARACTERISTIC_UUID,
            NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
        if (m_commandCharacteristic != nullptr) {
            m_commandCharacteristic->setCallbacks(&g_commandCallbacks);
        }

        m_consoleStreamCharacteristic = m_baseService->createCharacteristic(
            CONSOLE_STREAM_CHARACTERISTIC_UUID,
            NIMBLE_PROPERTY::NOTIFY);
        if (m_consoleStreamCharacteristic != nullptr) {
            m_consoleStreamCharacteristic->setCallbacks(&g_consoleStreamCallbacks);
        }
    }

    if (!m_factoryBootOnly) {
    m_litchcraftCharacteristic=m_baseService->createCharacteristic(
        LITCHCRAFT_CHARACTERISTIC_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
            NIMBLE_PROPERTY::NOTIFY,
        244U);
    if (m_litchcraftCharacteristic) m_litchcraftCharacteristic->setCallbacks(&g_litchcraftCallbacks);
    }

    m_connectionSessionIngressCharacteristic =
        m_baseService->createCharacteristic(
            CONNECTION_SESSION_INGRESS_CHARACTERISTIC_UUID,
            NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE,
            ConnectionSession::OBJECT_PACKET_BYTES);
    if (m_connectionSessionIngressCharacteristic != nullptr) {
        m_connectionSessionIngressCharacteristic->setCallbacks(
            &g_connectionSessionIngressCallbacks);
    }
    m_connectionSessionEgressCharacteristic =
        m_baseService->createCharacteristic(
            CONNECTION_SESSION_EGRESS_CHARACTERISTIC_UUID,
            NIMBLE_PROPERTY::NOTIFY,
            ConnectionSession::REVERSE_OBJECT_PACKET_BYTES);
    if (m_connectionSessionEgressCharacteristic != nullptr) {
        m_connectionSessionEgressCharacteristic->setCallbacks(
            &g_connectionSessionEgressCallbacks);
    }

    m_otaControlCharacteristic = m_baseService->createCharacteristic(
        OTA_CONTROL_CHARACTERISTIC_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY,
        OTA_CONTROL_CHARACTERISTIC_MAX_LEN);
    if (m_otaControlCharacteristic != nullptr) {
        m_otaControlCharacteristic->setCallbacks(&g_otaControlCallbacks);
    }

#if defined(LITCH_BLE_OTA)
    m_otaDataCharacteristic = m_baseService->createCharacteristic(
        OTA_DATA_CHARACTERISTIC_UUID,
        NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE);
    if (m_otaDataCharacteristic != nullptr) {
        m_otaDataCharacteristic->setCallbacks(&g_otaDataCallbacks);
    }
#endif

    if (!m_baseService->start()) {
        logger::Warn("ble: base service registration failed");
        ShutdownStack();
        return;
    }

    m_bleSettings.Configure(settings, &m_eventQueue, &m_droppedEvents);
    if (!m_recoveryOnly) {
        m_settingsService = m_server->createService(SETTINGS_SERVICE_UUID);
        if (m_settingsService == nullptr) {
            logger::Warn("ble: create settings service failed");
            ShutdownStack();
            return;
        }

        if (!m_bleSettings.CreateCharacteristics(*m_settingsService)) {
            ShutdownStack();
            return;
        }
        if (!m_settingsService->start()) {
            logger::Warn("ble: settings service registration failed");
            ShutdownStack();
            return;
        }

        if (m_factorySavedVerifierRequested && !ConfigureFactoryRadioEndpoint(*settings)) {
            logger::Warn("ble: factory verifier endpoint unavailable");
        }
    }

    m_advertising = NimBLEDevice::getAdvertising();
    if (m_advertising == nullptr) {
        logger::Warn("ble: advertising init failed");
        ShutdownStack();
        return;
    }

    if (!ConfigureAdvertising()) {
        logger::Warn("ble: discovery payload configuration failed");
        ShutdownStack();
        return;
    }

    if (!InitOwnerPairingNative()) { ShutdownStack(); return; }
    m_initialized = true;
    m_connected.store(false, std::memory_order_release);
    m_mtu.store(0, std::memory_order_release);
    m_connectionCount.store(0, std::memory_order_release);
    m_consoleOutputLen = 0;
    m_lastConsoleNotifyMs = 0;
    m_connectionHandle = INVALID_CONN_HANDLE;
    m_connectedAtMs = 0;
    m_lastClientActivityMs = 0;
    m_staleDisconnectIssued = false;
    m_consoleStreamSubscribed.store(false, std::memory_order_release);
    m_consoleStreamSnapshotRequested.store(false, std::memory_order_release);
    m_consoleSnapshotReceipt.store(0U, std::memory_order_release);
    m_otaControlSubscribed.store(false, std::memory_order_release);
#if defined(LITCH_BLE_OTA)
    m_otaDisconnectPending.store(false, std::memory_order_release);
    AbortOtaTransfer(true, true);
    m_otaStrictProfileArmed = false;
    if (!m_recoveryOnly && m_litchcraftGame) {
        m_litchcraftGame->Litchcraft().EndConnection();
        WakeLitchcraftFrameIngress();
    }
#endif
    m_otaInProgress = false;
    m_otaSimulationMode = false;
    m_otaPendingReboot = false;
    m_otaRebootArmedAtMs = 0U;
    m_otaRebootDisconnectAtMs = 0U;
    m_otaRebootDisconnectIssued = false;
    m_otaExpectedSize = 0;
    m_otaBytesReceived = 0;
    m_lastOtaStatusMs = 0;
    m_otaProgressPercent = 0;
#if defined(LITCH_BLE_OTA)
    m_otaLastDisplayedPercent = 255;
#endif
    (void)m_peerAddress.Assign("");

    logger::Info("ble: initialized (name=%s)", m_deviceName.CStr());
    StartAdvertising();
}

bool BleService::StopNativeStack() {
    // clearAll deletes SDK objects even when host stop fails. Keep both the
    // tree and native NVS binding alive until callbacks have actually stopped.
    const bool nativeStopped = NimBLEDevice::deinit(false);
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    // NimBLE 2.3.7 ignores nimble_port_deinit errors and clears its init flag.
    // Its boolean alone cannot prove that the S3 controller actually stopped.
    const bool controllerStopped =
        esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE;
#else
    const bool controllerStopped = true;
#endif
    if (!nativeStopped || !controllerStopped) {
        logger::Warn("ble: native stop incomplete; retaining native binding for recovery");
        m_initialized = false;
        m_enabled = false;
        return false;
    }
    return NimBLEDevice::deinit(true);
}

bool BleService::ShutdownStack() {
    // Stop() can reach teardown directly from the USB Console. The admitted
    // clear worker retains the stack until main-loop polling releases it.
    if (m_ownerClearToken != 0U) return false;
    RevokeOwnerAdmission();
    m_ownerNativeHandle.store(ble_internal::INVALID_CONN_HANDLE, std::memory_order_release);
    m_ownerNativeEpoch.store(0U, std::memory_order_release);
    // Factory GATT owns its client until cancellation has actually unwound.
    // Update retries teardown; the cooperative input/render loop never waits.
    if (!QuiesceFactoryBleDut()) {
        return false;
    }
    StopAdvertising();

#if defined(LITCH_BLE_OTA)
    m_otaDisconnectPending.store(false, std::memory_order_release);
    AbortOtaTransfer(true, true);
    m_otaStrictProfileArmed = false;
#endif

    // A server owns SDK allocations before required GATT registration succeeds.
    // Retire that partial tree too, so the existing Update retry starts clean.
    const bool ownedStack = m_initialized || m_server != nullptr || ble_internal::g_bleService == this;
    if (ownedStack && !StopNativeStack()) return false;
    if (ownedStack) m_linkObservation = {};
    ClearPendingConnectionControlAction();

    m_settings = nullptr;
    m_bleSettings.Reset();
    m_server = nullptr;
    m_baseService = nullptr;
    m_settingsService = nullptr;
    m_advertising = nullptr;
    m_commandCharacteristic = nullptr;
    m_consoleStreamCharacteristic = nullptr;
    m_otaControlCharacteristic = nullptr;
    m_litchcraftCharacteristic=nullptr;
    m_litchcraftSubscribed.store(false);
    m_connectionSessionIngressCharacteristic = nullptr;
    m_connectionSessionEgressCharacteristic = nullptr;
    m_connectionSessionSubscribed.store(false, std::memory_order_release);
#if !defined(LITCH_BLE_OTA)
    if (m_litchcraftGame) m_litchcraftGame->Litchcraft().EndConnection();
#endif
#if defined(LITCH_BLE_OTA)
    m_otaDataCharacteristic = nullptr;
#endif
    m_initialized = false;
    m_enabled = false;
    m_connected.store(false, std::memory_order_release);
    m_mtu.store(0, std::memory_order_release);
    m_connectionCount.store(0, std::memory_order_release);
    m_consoleOutputLen = 0;
    m_lastConsoleNotifyMs = 0;
    m_connectionHandle = INVALID_CONN_HANDLE;
    m_connectedAtMs = 0;
    m_lastClientActivityMs = 0;
    m_staleDisconnectIssued = false;
    m_consoleStreamSubscribed.store(false, std::memory_order_release);
    m_consoleStreamSnapshotRequested.store(false, std::memory_order_release);
    m_consoleSnapshotReceipt.store(0U, std::memory_order_release);
    m_otaControlSubscribed.store(false, std::memory_order_release);
    m_otaInProgress = false;
    m_otaSimulationMode = false;
    m_otaPendingReboot = false;
    m_otaRebootArmedAtMs = 0U;
    m_otaRebootDisconnectAtMs = 0U;
    m_otaRebootDisconnectIssued = false;
    m_otaExpectedSize = 0;
    m_otaBytesReceived = 0;
    m_lastOtaStatusMs = 0;
    m_otaProgressPercent = 0;
#if defined(LITCH_BLE_OTA)
    m_otaLastDisplayedPercent = 255;
#endif
    (void)m_peerAddress.Assign("");

    if (ownedStack) {
        // Shutdown is followed by construction of a new server and complete
        // characteristic tree. Delete the SDK-owned objects as well as stopping
        // the controller; otherwise a rename/reconnect can retain the previous
        // GAP name and callbacks until a full board reboot.
        logger::Info("ble: stopped");
    }
    ReleaseFactoryRadioEndpoint();

    if (ble_internal::g_bleService == this) {
        ble_internal::g_bleService = nullptr;
    }
    return true;
}

bool BleService::StopAdvertising() {
    if (m_advertising == nullptr || !m_advertising->isAdvertising()) {
        return true;
    }

    return m_advertising->stop() && !m_advertising->isAdvertising();
}

#endif

} // namespace modules
