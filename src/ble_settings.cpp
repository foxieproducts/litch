#include "modules/ble_settings.hpp"

#if defined(LITCH_BLE)

#include <ArduinoJson.h>

#include <cstddef>
#include <cstring>
#include <string_view>

#include <NimBLEDevice.h>

#include "logger.hpp"
#include "modules/ble_control_protocol.hpp"
#include "modules/ble_settings_async.hpp"
#include "modules/ble_settings_enumerate.hpp"
#include "modules/ble_settings_value_codec.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/settings_registry.hpp"

namespace modules {

namespace {

constexpr char SETTINGS_ENUMERATE_CHARACTERISTIC_UUID[] = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0102";
constexpr char SETTINGS_RW_CHARACTERISTIC_UUID[] = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0103";
constexpr uint16_t SETTINGS_CHARACTERISTIC_MAX_LEN = 512;

BleSettings* g_bleSettings = nullptr;

class BleSettingsEnumerateCallbacks final : public NimBLECharacteristicCallbacks {
  public:
    void onRead(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        if (g_bleSettings == nullptr || pCharacteristic == nullptr) {
            return;
        }
        if (!g_bleSettings->AllowsConnection(connInfo.getConnHandle())) {
            pCharacteristic->setValue(reinterpret_cast<const uint8_t*>("[]"), 2U);
            return;
        }
        g_bleSettings->OnEnumerateRead(*pCharacteristic);
    }
};

class BleSettingsReadWriteCallbacks final : public NimBLECharacteristicCallbacks {
  public:
    void onRead(NimBLECharacteristic* characteristic, NimBLEConnInfo& connection) override {
        if (characteristic != nullptr && (g_bleSettings == nullptr ||
            !g_bleSettings->AllowsConnection(connection.getConnHandle())))
            characteristic->setValue(reinterpret_cast<const uint8_t*>("{}"), 2U);
    }
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        if (g_bleSettings == nullptr || pCharacteristic == nullptr) {
            return;
        }

        if (!g_bleSettings->AllowsConnection(connInfo.getConnHandle())) return;
        // NimBLE 2.3.7 returns an owned ATT snapshot, up to this attribute's
        // 512-byte capacity. Its callback-local lifetime covers parsing.
        const auto value = pCharacteristic->getValue();
        const std::string_view payload = value.size() == 0U
            ? std::string_view{} : std::string_view(value.c_str(), value.size());
        g_bleSettings->OnReadWritePayload(payload);
    }

    void onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, uint16_t subValue) override {
        (void)pCharacteristic;
        (void)connInfo;
        if (g_bleSettings == nullptr || !g_bleSettings->AllowsConnection(connInfo.getConnHandle(), true)) {
            return;
        }
        g_bleSettings->OnReadWriteSubscribe(subValue);
    }
};

BleSettingsEnumerateCallbacks g_enumerateCallbacks;
BleSettingsReadWriteCallbacks g_readWriteCallbacks;

} // namespace

void BleSettings::Configure(const SettingsStore* settings,
                            BleEventQueue* eventQueue,
                            std::atomic<uint32_t>* droppedEvents) {
    ResetConnectionState();
    m_settings = settings;
    m_eventQueue = eventQueue;
    m_droppedEvents = droppedEvents;
    m_lastNotifiedRevision = (settings != nullptr) ? settings->GetRevision() : 0;
    m_pendingChangedIndices = 0U;
    for (size_t i = 0; i < SNAPSHOT_CAPACITY; ++i) {
        m_snapshot[i] = SnapshotEntry{};
    }
    if (settings != nullptr) {
        BuildInitialSnapshot(*settings);
    }
}

bool BleSettings::CreateCharacteristics(NimBLEService& settingsService) {
    m_enumerateCharacteristic = settingsService.createCharacteristic(SETTINGS_ENUMERATE_CHARACTERISTIC_UUID,
                                                                     NIMBLE_PROPERTY::READ,
                                                                     SETTINGS_CHARACTERISTIC_MAX_LEN);
    if (m_enumerateCharacteristic == nullptr) {
        logger::Warn("ble: create settings enumerate characteristic failed");
        return false;
    }

    m_readWriteCharacteristic = settingsService.createCharacteristic(
        SETTINGS_RW_CHARACTERISTIC_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY |
            NIMBLE_PROPERTY::INDICATE,
        SETTINGS_CHARACTERISTIC_MAX_LEN);
    if (m_readWriteCharacteristic == nullptr) {
        logger::Warn("ble: create settings rw characteristic failed");
        return false;
    }

    g_bleSettings = this;
    m_enumerateCharacteristic->setCallbacks(&g_enumerateCallbacks);
    m_enumerateCharacteristic->setValue(reinterpret_cast<const uint8_t*>("[]"), 2U);

    m_readWriteCharacteristic->setCallbacks(&g_readWriteCallbacks);
    m_readWriteCharacteristic->setValue(reinterpret_cast<const uint8_t*>("{}"), 2U);
    return true;
}

void BleSettings::Reset() {
    ResetConnectionState();
    m_enumerateCharacteristic = nullptr;
    m_readWriteCharacteristic = nullptr;
    if (g_bleSettings == this) {
        g_bleSettings = nullptr;
    }
}

uint32_t BleSettings::AdvanceConnectionEpoch() {
    return m_ingressConnectionEpoch.fetch_add(
               1U, std::memory_order_acq_rel) +
           1U;
}

uint32_t BleSettings::CurrentConnectionEpoch() const {
    return m_ingressConnectionEpoch.load(std::memory_order_acquire);
}

bool BleSettings::OwnsConnectionEpoch(const uint32_t connectionEpoch) const {
    return connectionEpoch != 0U &&
           connectionEpoch == CurrentConnectionEpoch() &&
           connectionEpoch == m_activeConnectionEpoch.load(std::memory_order_acquire);
}

void BleSettings::ResetConnectionState() {
    m_activeConnectionEpoch.store(AdvanceConnectionEpoch(), std::memory_order_release);
    ReleaseTrackedControlRequests();
    m_subscriptionValue.store(0U, std::memory_order_release);
    m_ingressSubscriptionValue.store(0U, std::memory_order_release);
    m_ingressSubscriptionEpoch.store(0U, std::memory_order_release);
    m_ingressSubscriptionRevision.fetch_add(1U, std::memory_order_acq_rel);
    m_enumerateCursor = 0U;
}

bool BleSettings::ActivateConnectionEpoch(const uint32_t connectionEpoch) {
    if (connectionEpoch == 0U ||
        connectionEpoch != CurrentConnectionEpoch()) {
        return false;
    }
    m_activeConnectionEpoch.store(connectionEpoch, std::memory_order_release);
    ReleaseTrackedControlRequests();
    for (;;) {
        const uint32_t revision =
            m_ingressSubscriptionRevision.load(std::memory_order_acquire);
        const uint16_t projectedSubscription =
            m_ingressSubscriptionEpoch.load(std::memory_order_acquire) == connectionEpoch
                ? m_ingressSubscriptionValue.load(std::memory_order_acquire)
                : 0U;
#if defined(LITCH_NATIVE)
        if (m_subscriptionActivationTestHook != nullptr) {
            const auto hook = m_subscriptionActivationTestHook;
            void* const context = m_subscriptionActivationTestContext;
            m_subscriptionActivationTestHook = nullptr;
            m_subscriptionActivationTestContext = nullptr;
            hook(context);
        }
#endif
        m_subscriptionValue.store(projectedSubscription, std::memory_order_release);
        if (m_ingressSubscriptionRevision.load(std::memory_order_acquire) == revision) {
            break;
        }
    }
    m_enumerateCursor = 0U;
    return true;
}

void BleSettings::ReleaseTrackedControlRequests() {
    for (size_t index = 0U; index < m_trackedControlRequestCount; ++index) {
        const uint16_t requestId = m_trackedControlRequestIds[index];
        if (requestId != 0U && m_controlMutationService != nullptr) {
            (void)m_controlMutationService->AbandonCompletion(requestId);
        }
        m_trackedControlRequestIds[index] = 0U;
    }
    m_trackedControlRequestCount = 0U;
}

void BleSettings::OnEnumerateRead(NimBLECharacteristic& characteristic) {
    if (!m_productAccess.load(std::memory_order_acquire) || m_settings == nullptr) {
        characteristic.setValue(reinterpret_cast<const uint8_t*>("[]"), 2U);
        m_enumerateCursor = 0;
        return;
    }

    size_t nextIndex = 0;
    bool done = true;
    modules::ble_settings_enumerate::BuildResponse(
        *m_settings,
        characteristic,
        m_enumerateCursor,
        nextIndex,
        done);
    m_enumerateCursor = done ? 0 : nextIndex;
}

} // namespace modules

#endif
