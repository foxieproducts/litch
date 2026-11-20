#include "modules/ble_service.hpp"

#if defined(LITCH_BLE) && defined(LITCH_HW_VARIANT_S3)
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_system.h>

#include <new>

#include "logger.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/ble_service_internal.hpp"
#include "modules/hardware_test_record.hpp"

namespace modules {

namespace {

class FactoryRadioCharacteristicCallbacks final
    : public NimBLECharacteristicCallbacks {
  public:
    explicit FactoryRadioCharacteristicCallbacks(BleService& owner)
        : m_owner(owner) {}

    void onWrite(NimBLECharacteristic* characteristic,
                 NimBLEConnInfo& connInfo) override {
        (void)connInfo;
        if (characteristic == nullptr) {
            return;
        }
        if (characteristic->getLength() != FACTORY_RADIO_WIRE_FRAME_SIZE) return;
        const auto value = characteristic->getValue<FactoryRadioWireFrame>();
        m_owner.OnFactoryRadioCharacteristicWrite(value.data(), value.size());
    }

  private:
    BleService& m_owner;
};

} // namespace

bool BleService::ConfigureFactoryRadioEndpoint(
    const SettingsStore& settings) {
    if (!m_factoryVerifierSupported) {
        return true;
    }
    HardwareTestRecord record;
    const char* const encoded = settings.ReadValue(
        HARDWARE_TEST_RECORD_SETTING_KEY, "");
    if (!DecodeHardwareTestRecord(encoded, record) ||
        !IsFactoryVerifierEligible(record)) {
        return true;
    }

    return CreateFactoryRadioEndpoint();
}

bool BleService::CreateFactoryRadioEndpoint() {
    if (m_factoryVerifierEndpointEnabled) {
        return true;
    }
    if (m_factoryRadioCallbacks == nullptr) {
        static_assert(sizeof(FactoryRadioCharacteristicCallbacks) <=
                          sizeof(m_factoryRadioCallbackStorage),
                      "factory callback must fit owner storage");
        static_assert(alignof(FactoryRadioCharacteristicCallbacks) <= alignof(std::max_align_t),
                      "factory callback alignment exceeds owner storage");
        m_factoryRadioCallbacks = new (m_factoryRadioCallbackStorage)
            FactoryRadioCharacteristicCallbacks(*this);
    }
    if (m_factoryRadioService == nullptr) {
        m_factoryRadioService = m_server->createService(ble_internal::FACTORY_RADIO_SERVICE_UUID);
    }
    if (m_factoryRadioCallbacks == nullptr ||
        m_factoryRadioService == nullptr) {
        logger::Warn("ble: factory verifier service allocation failed");
        return false;
    }
    m_factoryRadioCharacteristic =
        m_factoryRadioService->createCharacteristic(
            ble_internal::FACTORY_RADIO_CHARACTERISTIC_UUID,
            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE,
            FACTORY_RADIO_WIRE_FRAME_SIZE);
    if (m_factoryRadioCharacteristic == nullptr) {
        logger::Warn("ble: factory verifier characteristic allocation failed");
        return false;
    }
    m_factoryRadioCharacteristic->setCallbacks(m_factoryRadioCallbacks);
    m_factoryRadioService->start();
    m_factoryVerifierHandshake.SetEligible(true);
    m_factoryVerifierConfiguredEnabled.store(true, std::memory_order_release);
    m_factoryVerifierRuntimeAllowed.store(false, std::memory_order_release);
    m_factoryVerifierEndpointEnabled = true;
    logger::Info("ble: factory verifier endpoint enabled");
    return true;
}

void BleService::ReleaseFactoryRadioEndpoint() {
    m_factoryVerifierGeneration.fetch_add(1U, std::memory_order_acq_rel);
    if (m_factoryRadioCallbacks != nullptr) {
        static_cast<FactoryRadioCharacteristicCallbacks*>(m_factoryRadioCallbacks)
            ->~FactoryRadioCharacteristicCallbacks();
    }
    m_factoryRadioCallbacks = nullptr;
    m_factoryRadioService = nullptr;
    m_factoryRadioCharacteristic = nullptr;
    m_factoryVerifierHandshake.SetEligible(false);
    m_factoryVerifierConfiguredEnabled.store(false, std::memory_order_release);
    m_factoryVerifierRuntimeAllowed.store(false, std::memory_order_release);
    m_factoryVerifierProofPending.store(false, std::memory_order_release);
    m_factoryVerifierEndpointEnabled = false;
}

bool BleService::EnableFactoryVerifierEndpoint(
    const SettingsStore& settings) {
    if (!m_factoryVerifierSupported) {
        return false;
    }
    if (m_factoryVerifierEndpointEnabled) {
        SetFactoryVerifierConfiguredEnabled(true);
        return true;
    }
    if (!m_initialized || m_server == nullptr || m_advertising == nullptr) {
        return false;
    }
    StopAdvertising();
    const bool configured = ConfigureFactoryRadioEndpoint(settings);
    if (configured && m_factoryVerifierEndpointEnabled) {
        m_advertising->addServiceUUID(
            ble_internal::FACTORY_RADIO_SERVICE_UUID);
    }
    StartAdvertising();
    return configured && m_factoryVerifierEndpointEnabled;
}

void BleService::OnFactoryRadioCharacteristicWrite(
    const uint8_t* const data,
    const size_t len) {
    if (!m_factoryVerifierEndpointEnabled || m_factoryRadioCharacteristic == nullptr) {
        return;
    }
    FactoryRadioFrame inbound;
    FactoryRadioFrame outbound;
    if (!DecodeFactoryRadioFrame(data, len, inbound) ||
        inbound.channel != FactoryRadioChannel::Ble) {
        return;
    }
    if (!m_factoryVerifierRuntimeAllowed.load(std::memory_order_acquire) ||
        m_factoryVerifierProofPending.load(std::memory_order_acquire) ||
        (IsFactoryBleDutRunning() && HasFactoryBleCandidate())) {
        // NimBLE stores a write before this callback. Restore only an exact
        // completed Response during terminal grace; never publish proof twice.
        const uint32_t generation =
            m_factoryVerifierGeneration.load(std::memory_order_acquire);
        if (m_factoryVerifierConfiguredEnabled.load(std::memory_order_acquire) &&
            generation == m_factoryVerifierCallbackGeneration &&
            m_factoryVerifierHandshake.WasHandshakeCompleted() &&
            inbound.type == FactoryRadioFrameType::Response &&
            m_factoryVerifierHandshake.Handle(inbound, 0U, outbound)) {
            FactoryRadioWireFrame terminal{};
            if (EncodeFactoryRadioFrame(outbound, terminal)) {
                m_factoryRadioCharacteristic->setValue(terminal.data(), terminal.size());
            }
        }
        return;
    }
    const uint32_t nowMs = fxe::Millis();
    const uint32_t generation = m_factoryVerifierGeneration.load(std::memory_order_acquire);
    if (generation != m_factoryVerifierCallbackGeneration ||
        static_cast<uint32_t>(nowMs - m_factoryVerifierLastFrameMs) >= 2000U) {
        m_factoryVerifierCallbackGeneration = generation;
        m_factoryVerifierHandshake.SetEligible(false);
        m_factoryVerifierHandshake.SetEligible(true);
    }
    if (!m_factoryVerifierHandshake.Handle(inbound, esp_random(), outbound)) {
        return;
    }
    m_factoryVerifierLastFrameMs = nowMs;
    FactoryRadioWireFrame wire{};
    if (!EncodeFactoryRadioFrame(outbound, wire)) {
        return;
    }
    m_factoryRadioCharacteristic->setValue(wire.data(), wire.size());
    if (!m_factoryVerifierHandshake.WasHandshakeCompleted()) {
        return;
    }
    m_factoryProofGeneration.store(generation, std::memory_order_relaxed);
    m_factoryProofSessionId.store(inbound.sessionId,
                                  std::memory_order_relaxed);
    m_factoryProofDutNonce.store(inbound.senderNonce,
                                 std::memory_order_relaxed);
    m_factoryProofVerifierNonce.store(inbound.echoNonce,
                                      std::memory_order_relaxed);
    m_factoryVerifierProofPending.store(true, std::memory_order_release);
}

bool BleService::EnableTemporaryFactoryVerifierEndpoint() {
    if (!m_factoryVerifierSupported || !m_initialized || !m_enabled ||
        m_server == nullptr ||
        m_advertising == nullptr) {
        return false;
    }
    StopAdvertising();
    const bool created = CreateFactoryRadioEndpoint();
    if (created) {
        SetFactoryVerifierConfiguredEnabled(true);
        m_advertising->addServiceUUID(ble_internal::FACTORY_RADIO_SERVICE_UUID);
    }
    StartAdvertising();
    return created;
}

void BleService::SetFactoryVerifierSupported(const bool supported) {
    m_factoryVerifierSupported = supported;
    if (!supported) {
        SetFactoryVerifierConfiguredEnabled(false);
    }
}

bool BleService::ConsumeFactoryBleVerifierProof(FactoryBleProof& outProof) {
    if (!m_factoryVerifierProofPending.load(std::memory_order_acquire)) {
        return false;
    }
    if (m_factoryProofGeneration.load(std::memory_order_relaxed) !=
        m_factoryVerifierGeneration.load(std::memory_order_acquire)) {
        m_factoryVerifierProofPending.store(false, std::memory_order_release);
        return false;
    }
    // Keep this exact server proof queued while a prior discovery/client owner
    // unwinds and releases its SDK allocations. Normal GATT/server state stays.
    if (!QuiesceFactoryBleDut()) return false;
    if (!m_factoryVerifierProofPending.load(std::memory_order_acquire) ||
        m_factoryProofGeneration.load(std::memory_order_relaxed) !=
            m_factoryVerifierGeneration.load(std::memory_order_acquire)) {
        m_factoryVerifierProofPending.store(false, std::memory_order_release);
        return false;
    }
    outProof.sessionId = m_factoryProofSessionId.load(std::memory_order_relaxed);
    outProof.dutNonce = m_factoryProofDutNonce.load(std::memory_order_relaxed);
    outProof.verifierNonce =
        m_factoryProofVerifierNonce.load(std::memory_order_relaxed);
    // Keep the slot owned while copying so another callback cannot replace
    // individual fields between reads.
    m_factoryVerifierProofPending.store(false, std::memory_order_release);
    return true;
}

void BleService::SetFactoryVerifierRuntimeAllowed(const bool allowed) {
    m_factoryVerifierRuntimeAllowed.store(
        allowed && m_factoryVerifierEndpointEnabled &&
            m_factoryVerifierConfiguredEnabled.load(
                std::memory_order_acquire),
        std::memory_order_release);
}

void BleService::SetFactoryVerifierConfiguredEnabled(const bool enabled) {
    const bool next = enabled && m_factoryVerifierEndpointEnabled;
    const bool previous = m_factoryVerifierConfiguredEnabled.exchange(
        next, std::memory_order_acq_rel);
    if (previous == next) {
        return;
    }
    if (!next) {
        m_factoryVerifierGeneration.fetch_add(1U, std::memory_order_acq_rel);
        m_factoryVerifierRuntimeAllowed.store(false,
                                              std::memory_order_release);
        m_factoryVerifierProofPending.store(false,
                                            std::memory_order_release);
    }
    if (m_advertising == nullptr) {
        return;
    }
    StopAdvertising();
    if (next) {
        (void)m_advertising->addServiceUUID(
            ble_internal::FACTORY_RADIO_SERVICE_UUID);
    } else {
        (void)m_advertising->removeServiceUUID(
            ble_internal::FACTORY_RADIO_SERVICE_UUID);
    }
    StartAdvertising();
}

bool BleService::IsFactoryVerifierEndpointEnabled() const {
    return m_factoryVerifierEndpointEnabled;
}

bool BleService::IsFactoryVerifierConfiguredEnabled() const {
    return m_factoryVerifierConfiguredEnabled.load(
        std::memory_order_acquire);
}

} // namespace modules

#else

namespace modules {

#if defined(LITCH_BLE)
bool BleService::ConfigureFactoryRadioEndpoint(const SettingsStore&) {
    return true;
}

void BleService::ReleaseFactoryRadioEndpoint() {}
#endif

bool BleService::EnableTemporaryFactoryVerifierEndpoint() { return false; }

bool BleService::ConsumeFactoryBleVerifierProof(FactoryBleProof&) {
    return false;
}

bool BleService::EnableFactoryVerifierEndpoint(const SettingsStore&) {
    return false;
}

void BleService::SetFactoryVerifierSupported(bool) {}

void BleService::SetFactoryVerifierConfiguredEnabled(bool) {}

void BleService::SetFactoryVerifierRuntimeAllowed(bool) {}

bool BleService::IsFactoryVerifierEndpointEnabled() const {
    return false;
}

bool BleService::IsFactoryVerifierConfiguredEnabled() const {
    return false;
}

} // namespace modules

#endif
