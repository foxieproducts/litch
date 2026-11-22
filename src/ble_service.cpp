#include "modules/ble_service.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>
#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#endif

#include "logger.hpp"
#include "modules/ble_service_internal.hpp"
#include "modules/user_text.hpp"

namespace modules {

#if defined(LITCH_BLE)
#if defined(LITCH_BLE_OTA)
namespace {

class OtaIngressLockGuard {
  public:
    explicit OtaIngressLockGuard(std::atomic_flag& lock) : m_lock(lock) {
        while (m_lock.test_and_set(std::memory_order_acquire)) {
        }
    }

    ~OtaIngressLockGuard() { m_lock.clear(std::memory_order_release); }

    OtaIngressLockGuard(const OtaIngressLockGuard&) = delete;
    OtaIngressLockGuard& operator=(const OtaIngressLockGuard&) = delete;

  private:
    std::atomic_flag& m_lock;
};

bool IsRetiredDirectFrameStorePacket(const uint8_t* const data,
                                     const size_t len) {
    return data != nullptr && len >= 3U && data[0U] == 0x4dU &&
        data[1U] == 2U && data[2U] >= 18U && data[2U] <= 26U;
}

} // namespace
#endif

namespace ble_internal {

BleService* g_bleService = nullptr;


} // namespace ble_internal

bool BleService::TryDequeueConsoleInputChar(char& outChar) {
    return m_consoleInput.TryDequeue(outChar);
}

bool BleService::IsJsonEnvelope(const char* data, const size_t len) const {
    if (data == nullptr || len == 0) {
        return false;
    }

    size_t first = 0;
    while (first < len &&
           (data[first] == ' ' || data[first] == '\n' || data[first] == '\r' || data[first] == '\t')) {
        ++first;
    }
    if (first >= len) {
        return false;
    }

    return data[first] == '{';
}

void BleService::OnCommandCharacteristicWrite(const char* data, const size_t len) {
    if (m_recoveryOnly || !OwnerProductAccess() || data == nullptr || len == 0) {
        return;
    }

    BleEvent event;
    event.type = BleEventType::ConsoleInput;
    const size_t copyLen = std::min(len, ble_internal::MAX_CONSOLE_EVENT_BYTES);
    if (copyLen == 0) {
        return;
    }

    if (len > ble_internal::MAX_CONSOLE_EVENT_BYTES) {
        m_droppedEvents.fetch_add(1, std::memory_order_acq_rel);
        return;
    }

    for (size_t i = 0; i < copyLen; ++i) {
        event.payload.consoleInput.input[i] = data[i];
    }
    event.payload.consoleInput.input[copyLen] = '\0';
    event.payload.consoleInput.len = static_cast<uint8_t>(copyLen);
    event.payload.consoleInput.connectionEpoch =
        m_bleSettings.CurrentConnectionEpoch();

    if (!m_eventQueue.Enqueue(event)) {
        m_droppedEvents.fetch_add(1, std::memory_order_acq_rel);
    }
}

void BleService::PollConnectionRenewal() {
    const uint32_t epoch = m_connectionRenewalEpoch.exchange(0U, std::memory_order_acq_rel);
    const uint16_t handle = m_connectionRenewalHandle.load(std::memory_order_relaxed);
    if (epoch == 0U || epoch == m_connectionRenewalIssuedEpoch || m_server == nullptr ||
        handle == ble_internal::INVALID_CONN_HANDLE ||
        epoch != m_ownerNativeEpoch.load(std::memory_order_acquire) ||
        handle != m_ownerNativeHandle.load(std::memory_order_acquire) ||
        epoch != m_bleSettings.CurrentConnectionEpoch() ||
        !m_connectionSessionActivated.load(std::memory_order_acquire)) return;
    // The idle watchdog may already own this same native disconnect. Its flag
    // belongs to the validated current lifetime and clears on actual renewal.
    if (m_staleDisconnectIssued) return;
    const bool accepted = m_server->disconnect(handle);
    if (accepted) {
        m_connectionRenewalIssuedEpoch = epoch;
        // The existing idle watchdog shares this pending native disconnect.
        m_staleDisconnectIssued = true;
    }
    // A rejected SDK call consumes this request only. An ordinary later
    // bootstrap probe may request again; existing idle recovery remains live.
    logger::Info("ble: physical renewal request accepted=%u", accepted ? 1U : 0U);
}

void BleService::OnOtaControlCharacteristicWrite(const char* data, const size_t len) {
    if (data == nullptr || len == 0) {
        return;
    }
    if (m_connectionSessionActivated.load(std::memory_order_acquire)) {
        // This exact bootstrap probe is emitted only before the app knows this
        // link's profile. A local GATT client may have restarted while the
        // peripheral still owns the prior activated session. Retain that
        // session until a real native disconnect; never reopen Hello in place.
        static constexpr char probe[] = "{\"cmd\":\"info\"}";
        if (len == sizeof(probe) - 1U && std::memcmp(data, probe, len) == 0) {
            const uint32_t epoch = m_ownerNativeEpoch.load(std::memory_order_acquire);
            const uint16_t handle = m_ownerNativeHandle.load(std::memory_order_acquire);
            if (epoch != 0U && handle != ble_internal::INVALID_CONN_HANDLE &&
                epoch == m_bleSettings.CurrentConnectionEpoch() &&
                epoch == m_ownerNativeEpoch.load(std::memory_order_acquire) &&
                m_connectionSessionActivated.load(std::memory_order_acquire)) {
                m_connectionRenewalHandle.store(handle, std::memory_order_relaxed);
                m_connectionRenewalEpoch.store(epoch, std::memory_order_release);
            }
        }
        logger::Warn("BLE OTA control rejected on retired direct ingress");
        m_droppedEvents.fetch_add(1U, std::memory_order_acq_rel);
        return;
    }

    bool oversizeRejected = false;
    bool enqueued = false;
#if defined(LITCH_BLE_OTA)
    {
        OtaIngressLockGuard guard(m_otaIngressLock);
        const BleOtaControlIngressClassification classification =
            ClassifyBleOtaControlIngress(
                data, len, ble_internal::MAX_OTA_CONTROL_EVENT_BYTES);
        if (classification.beginCandidate) {
            (void)m_otaChunkQueue.AdvanceEpoch();
        }
        const uint32_t sessionEpoch = m_otaChunkQueue.CurrentEpoch();
        if (!classification.acceptForQueue) {
            oversizeRejected = true;
            const BleOtaRejectedBeginSignal rejectedBegin =
                ResolveBleOtaBeginIngressRejectSignal(
                    classification.beginCandidate,
                    false,
                    sessionEpoch,
                    classification.rejectReason);
            if (rejectedBegin.cleanupEpoch != 0U) {
                m_otaRejectedBeginCandidateEpoch =
                    rejectedBegin.cleanupEpoch;
                m_otaRejectedBeginCandidateReason =
                    rejectedBegin.reason;
            }
        } else {
            BleEvent event;
            event.type = BleEventType::OtaControl;
            event.payload.otaControl = BleOtaControlEvent{};
            for (size_t i = 0; i < len; ++i) {
                event.payload.otaControl.input[i] = data[i];
            }
            event.payload.otaControl.input[len] = '\0';
            event.payload.otaControl.len = static_cast<uint8_t>(len);
            event.payload.otaControl.sessionEpoch = sessionEpoch;
            event.payload.otaControl.connectionEpoch =
                m_bleSettings.CurrentConnectionEpoch();
            event.ingressOrdinal = m_nextOtaIngressOrdinal++;
            enqueued = m_eventQueue.Enqueue(event);
            const BleOtaRejectedBeginSignal rejectedBegin =
                ResolveBleOtaBeginIngressRejectSignal(
                    classification.beginCandidate,
                    enqueued,
                    sessionEpoch,
                    BleOtaRejectedBeginReason::EventQueueFull);
            if (rejectedBegin.cleanupEpoch != 0U) {
                m_otaRejectedBeginCandidateEpoch =
                    rejectedBegin.cleanupEpoch;
                m_otaRejectedBeginCandidateReason =
                    rejectedBegin.reason;
            }
        }
    }
#else
    if (len > ble_internal::MAX_OTA_CONTROL_EVENT_BYTES) {
        logger::Warn("BLE OTA control packet dropped: len=%lu max=%lu",
                     static_cast<unsigned long>(len),
                     static_cast<unsigned long>(ble_internal::MAX_OTA_CONTROL_EVENT_BYTES));
        m_droppedEvents.fetch_add(1, std::memory_order_acq_rel);
        return;
    }

    BleEvent event;
    event.type = BleEventType::OtaControl;
    event.payload.otaControl = BleOtaControlEvent{};
    for (size_t i = 0; i < len; ++i) {
        event.payload.otaControl.input[i] = data[i];
    }
    event.payload.otaControl.input[len] = '\0';
    event.payload.otaControl.len = static_cast<uint8_t>(len);
    event.payload.otaControl.connectionEpoch =
        m_bleSettings.CurrentConnectionEpoch();
    enqueued = m_eventQueue.Enqueue(event);
#endif
    if (oversizeRejected) {
        logger::Warn("BLE OTA control packet dropped: len=%lu max=%lu",
                     static_cast<unsigned long>(len),
                     static_cast<unsigned long>(ble_internal::MAX_OTA_CONTROL_EVENT_BYTES));
        m_droppedEvents.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    if (!enqueued) {
        logger::Warn("BLE OTA control packet dropped: event queue full");
        m_droppedEvents.fetch_add(1, std::memory_order_acq_rel);
    }
}

#if defined(LITCH_BLE_OTA)
bool BleService::OnOtaDataCharacteristicWrite(const uint8_t* data, const size_t len) {
    if (!m_recoveryOnly && !OwnerProductAccess()) return false;
    if (m_connectionSessionActivated.load(std::memory_order_acquire)) {
        logger::Warn("BLE OTA data rejected on retired direct ingress");
        m_droppedEvents.fetch_add(1U, std::memory_order_acq_rel);
        return false;
    }
    // Store is connection-owned. Only an active firmware update may treat an
    // otherwise Store-shaped payload as opaque image bytes.
    if (!IsOtaInProgress() && IsRetiredDirectFrameStorePacket(data, len)) {
        logger::Warn("BLE Store packet rejected on retired OTA-data ingress");
        m_droppedEvents.fetch_add(1U, std::memory_order_acq_rel);
        return false;
    }
    bool enqueued = false;
    {
        OtaIngressLockGuard guard(m_otaIngressLock);
        const uint32_t ingressOrdinal = m_nextOtaIngressOrdinal++;
        enqueued = m_otaChunkQueue.Enqueue(data, len, ingressOrdinal);
    }
    if (!enqueued) {
        logger::Warn("BLE OTA data packet rejected before staging: len=%lu max=%lu",
                     static_cast<unsigned long>(len),
                     static_cast<unsigned long>(ble_internal::MAX_OTA_CHUNK_BYTES));
        ++m_otaChunkEnqueueDrops;
        m_droppedEvents.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }

    ++m_otaChunkEnqueueCount;
    return true;
}
#endif

bool BleService::ReadEnabledSetting(const SettingsStore& settings) const {
    if (m_factoryBootOnly) return true;
    const auto& doc = settings.Doc();
    if (!doc.containsKey(ble_internal::BLE_ENABLED_KEY)) {
        return true;
    }

    return settings.ReadValue(ble_internal::BLE_ENABLED_KEY, static_cast<uint32_t>(1U)) != 0U;
}

fxe::FixedString<32> BleService::ReadDeviceNameSetting(const SettingsStore& settings) const {
    fxe::FixedString<32> outName(DEFAULT_DEVICE_NAME);
    const auto& doc = settings.Doc();

    if (!doc.containsKey(ble_internal::BLE_DEVICE_NAME_KEY)) {
        return outName;
    }

    const char* rawName = settings.ReadValue(ble_internal::BLE_DEVICE_NAME_KEY, DEFAULT_DEVICE_NAME);
    if (rawName == nullptr || rawName[0] == '\0') {
        return outName;
    }

    const size_t byteCount = std::strlen(rawName);
    if (!user_text::IsSafeDisplayName(rawName, byteCount, 20U, 31U)) {
        return outName;
    }
    fxe::FixedString<32> validated;
    return validated.Assign(rawName) ? validated : outName;
}

void BleService::OnConnected(const uint16_t connHandle,
                             const char* peerAddress,
                             const uint16_t mtu,
                             const uint8_t connectionCount) {
    // Native callback replay belongs to the same link. It must not revoke the
    // consent lease, advance its epoch or synthesize another security request.
    if (OwnerPairingEnabled() &&
        m_ownerNativeEpoch.load(std::memory_order_acquire) != 0U &&
        m_ownerNativeHandle.load(std::memory_order_acquire) == connHandle) return;
    RevokeOwnerAdmission();
    m_ownerNativeHandle.store(connHandle, std::memory_order_release);
    // The projected connection event may wait behind flash work. A fresh
    // link must not inherit the previous link's retired direct ingress while
    // its pre-session capability request is already arriving.
    m_connectionSessionActivated.store(false, std::memory_order_release);
    m_consoleStreamSubscribed.store(false, std::memory_order_release);
    m_consoleStreamSnapshotRequested.store(false, std::memory_order_release);
    m_consoleSnapshotReceipt.store(0U, std::memory_order_release);
    BleConnectionChangeEvent event;
    event.connected = true;
    event.connHandle = connHandle;
    event.mtu = mtu;
    event.reason = 0;
    event.connectionCount = connectionCount;
    event.callbackAtMs = fxe::Millis();
    event.connectionEpoch =
        m_bleSettings.AdvanceConnectionEpoch();
    m_ownerNativeEpoch.store(event.connectionEpoch, std::memory_order_release);
    m_ownerForeignEpoch.store(0U, std::memory_order_release);
    m_ownerNativeFault.store(false, std::memory_order_release);
    if (peerAddress != nullptr) {
        const std::string_view peer(peerAddress);
        const size_t maxCopy = sizeof(event.peerAddress) - 1U;
        const size_t copyLen = (peer.size() < maxCopy) ? peer.size() : maxCopy;
        for (size_t i = 0; i < copyLen; ++i) {
            event.peerAddress[i] = peer[i];
        }
        event.peerAddress[copyLen] = '\0';
    }
    bool enqueued = false;
#if defined(LITCH_BLE_OTA)
    {
        OtaIngressLockGuard guard(m_otaIngressLock);
        (void)m_otaChunkQueue.AdvanceEpoch();
        enqueued = m_connectionProjection.Publish(event);
    }
#else
    enqueued = m_connectionProjection.Publish(event);
#endif
    if (!enqueued) {
        m_droppedEvents.fetch_add(1, std::memory_order_acq_rel);
    }
    ObserveOwnerNativePeer(connHandle);
}

void BleService::OnDisconnected(uint16_t connHandle, int reason) {
    if (connHandle != m_ownerNativeHandle.load(std::memory_order_acquire)) return;
    OnDisconnected(reason);
}

void BleService::OnDisconnected(const int reason) {
    RevokeOwnerAdmission();
    m_ownerNativeHandle.store(ble_internal::INVALID_CONN_HANDLE, std::memory_order_release);
    m_ownerNativeEpoch.store(0U, std::memory_order_release);
    m_consoleStreamSubscribed.store(false, std::memory_order_release);
    m_consoleStreamSnapshotRequested.store(false, std::memory_order_release);
    m_consoleSnapshotReceipt.store(0U, std::memory_order_release);
#if defined(LITCH_BLE_OTA)
    m_otaDisconnectPending.store(true, std::memory_order_release);
#endif
    BleConnectionChangeEvent event;
    event.connected = false;
    event.connHandle = ble_internal::INVALID_CONN_HANDLE;
    event.mtu = 0;
    event.reason = reason;
    event.connectionCount = 0;
    event.callbackAtMs = fxe::Millis();
    event.connectionEpoch =
        m_bleSettings.AdvanceConnectionEpoch();
    event.peerAddress[0] = '\0';
    bool enqueued = false;
#if defined(LITCH_BLE_OTA)
    {
        OtaIngressLockGuard guard(m_otaIngressLock);
        (void)m_otaChunkQueue.AdvanceEpoch();
        enqueued = m_connectionProjection.Publish(event);
    }
#else
    enqueued = m_connectionProjection.Publish(event);
#endif
    if (!enqueued) {
        m_droppedEvents.fetch_add(1, std::memory_order_acq_rel);
    }
}

void BleService::OnLinkObservation(const uint16_t handle, const uint16_t mtu,
                                  const uint8_t txPhy, const uint8_t rxPhy,
                                  const bool phyUpdateCompleted) {
    if (!OwnerCallbackAllowed(handle, true)) return;
    BleEvent event;
    event.type = BleEventType::LinkObservation;
    event.payload.linkObservation = {handle, mtu, txPhy, rxPhy,
                                    m_bleSettings.CurrentConnectionEpoch()};
    event.payload.linkObservation.phyUpdateCompleted = phyUpdateCompleted;
    if (!m_eventQueue.Enqueue(event)) m_droppedEvents.fetch_add(1U);
}

void BleService::OnNativeDataLengthChange(const uint16_t handle,
                                        const BleDataLengthObservation& value) {
    if (!OwnerCallbackAllowed(handle, true)) return;
    BleEvent event;
    event.type = BleEventType::LinkObservation;
    event.payload.linkObservation.handle = handle;
    event.payload.linkObservation.connectionEpoch = m_bleSettings.CurrentConnectionEpoch();
    event.payload.linkObservation.dataLength = value;
    event.payload.linkObservation.dataLengthObserved = true;
    if (!m_eventQueue.Enqueue(event)) m_droppedEvents.fetch_add(1U);
}

void BleService::OnConnectionParametersChanged(
    const uint16_t intervalUnits,
    const uint16_t latency,
    const uint16_t supervisionTimeoutUnits,
    const bool initial, const uint16_t handle) {
    BleEvent event;
    event.type = BleEventType::ConnectionParameters;
    event.payload.connectionParameters.handle = handle;
    event.payload.connectionParameters.intervalUnits = intervalUnits;
    event.payload.connectionParameters.latency = latency;
    event.payload.connectionParameters.supervisionTimeoutUnits =
        supervisionTimeoutUnits;
    event.payload.connectionParameters.connectionEpoch =
        m_bleSettings.CurrentConnectionEpoch();
    event.payload.connectionParameters.initial = initial;
    if (!m_eventQueue.Enqueue(event)) {
        m_droppedEvents.fetch_add(1, std::memory_order_acq_rel);
    }
}

void BleService::OnConsoleStreamSubscriptionChanged(const uint16_t subValue) {
    if (m_recoveryOnly) return;
    const uint32_t connectionEpoch = m_bleSettings.CurrentConnectionEpoch();
    m_consoleStreamSubscriptionValue.store(subValue, std::memory_order_relaxed);
    m_consoleStreamSubscriptionEpoch.store(connectionEpoch, std::memory_order_release);
    m_consoleStreamSubscriptionRevision.fetch_add(1U, std::memory_order_acq_rel);
    if (!m_bleSettings.OwnsConnectionEpoch(connectionEpoch)) {
        return;
    }
    const bool subscribed = (subValue & 0x03U) != 0U;
    const bool wasSubscribed = m_consoleStreamSubscribed.exchange(
        subscribed, std::memory_order_acq_rel);
    if (subscribed && !wasSubscribed) {
        m_consoleStreamSnapshotRequested.store(true, std::memory_order_release);
    }
}

void BleService::OnOtaControlSubscriptionChanged(const uint16_t subValue) {
    const uint32_t connectionEpoch = m_bleSettings.CurrentConnectionEpoch();
    m_otaControlSubscriptionValue.store(subValue, std::memory_order_relaxed);
    m_otaControlSubscriptionEpoch.store(connectionEpoch, std::memory_order_release);
    m_otaControlSubscriptionRevision.fetch_add(1U, std::memory_order_acq_rel);
    if (m_bleSettings.OwnsConnectionEpoch(connectionEpoch)) {
        m_otaControlSubscribed.store((subValue & 0x03U) != 0U, std::memory_order_release);
    }
}

#endif

} // namespace modules
