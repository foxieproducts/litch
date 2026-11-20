#include "foxie_embedded/millis.hpp"
#include "modules/ble_service.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include <algorithm>
#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#endif
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

  private:
    std::atomic_flag& m_lock;
};

} // namespace
#endif

void BleService::OnLitchcraftWrite(const uint8_t* bytes, const size_t size) {
    if (m_recoveryOnly || !OwnerProductAccess()) return;
    if (m_connectionSessionActivated.load(std::memory_order_acquire)) {
        m_droppedEvents.fetch_add(1U, std::memory_order_acq_rel);
        return;
    }
    // V1 stays at ATT-minimum control frames. V2 canonical chunks reuse this
    // same bounded event payload after negotiated MTU; no second ingress lane.
    BleEvent event;
    bool enqueued = MakeLitchcraftIngressEvent(
        bytes, size, m_bleSettings.CurrentConnectionEpoch(), event);
#if defined(LITCH_BLE_OTA)
    if (enqueued) {
        OtaIngressLockGuard guard(m_otaIngressLock);
        event.ingressOrdinal = m_nextOtaIngressOrdinal++;
        enqueued = m_eventQueue.Enqueue(event);
    }
#else
    if (enqueued) enqueued = m_eventQueue.Enqueue(event);
#endif
    if (!enqueued) {
        m_litchcraftIngressOverflow.store(true);
    }
}
void BleService::OnLitchcraftSubscription(const uint16_t value) {
    if (m_recoveryOnly) return;
    const uint32_t connectionEpoch = m_bleSettings.CurrentConnectionEpoch();
    m_litchcraftSubscriptionValue.store(value, std::memory_order_relaxed);
    m_litchcraftSubscriptionEpoch.store(connectionEpoch, std::memory_order_release);
    m_litchcraftSubscriptionRevision.fetch_add(1U, std::memory_order_acq_rel);
    if (m_bleSettings.OwnsConnectionEpoch(connectionEpoch)) {
        m_litchcraftSubscribed.store((value & 1U) != 0U, std::memory_order_release);
    }
}

void BleService::UpdateLitchcraftNotifications() {
    if (!OwnerProductAccess() || !m_litchcraftGame) {
        return;
    }
#if defined(LITCH_BLE_OTA)
    WakeLitchcraftFrameIngress();
#endif
    auto& art = m_litchcraftGame->Litchcraft();
    if (m_litchcraftIngressOverflow.exchange(false)) {
        art.RequireResync();
    }
    if (!IsConnected() || !m_litchcraftSubscribed.load() || !m_litchcraftCharacteristic) {
        return;
    }
    if (m_connectionSession.ObjectStreamToken() != 0U) return;
    const uint32_t epoch = m_bleSettings.CurrentConnectionEpoch();
    // A bounded burst leaves Hall, rendering, and other BLE streams responsive.
    for (uint8_t count = 0U; count < 6U; ++count) {
#if defined(LITCH_BLE_OTA)
        if (m_connectionSession.ObjectStreamToken() == 0U &&
            m_litchcraftFrameResponsePeekCallback != nullptr &&
            m_litchcraftFrameResponseAcceptCallback != nullptr) {
            std::array<uint8_t, 19U> response{};
            size_t responseSize = 0U;
            if (m_litchcraftFrameResponsePeekCallback(
                    m_litchcraftFrameIngressContext, response.data(),
                    response.size(), responseSize)) {
                if (!OwnerProductAccess() || !m_bleSettings.OwnsConnectionEpoch(epoch) ||
                    !m_litchcraftCharacteristic->notify(
                        response.data(), responseSize, m_connectionHandle)) {
                    break;
                }
                m_litchcraftFrameResponseAcceptCallback(
                    m_litchcraftFrameIngressContext);
                continue;
            }
        }
#endif
        LitchcraftSession::BulkPacket packet;
        const size_t maximumPayload = GetMtu() > 3U
            ? std::min<size_t>(244U, GetMtu() - 3U) : 20U;
        if (!art.PeekPacket(packet, maximumPayload) || !m_bleSettings.OwnsConnectionEpoch(epoch)) {
            break;
        }
        // The exact-payload overload reports enqueue success. Attribute-only
        // notify() may coalesce several scheduled updates into the last value.
        if (!OwnerProductAccess() || !m_litchcraftCharacteristic->notify(
                packet.bytes.data(), packet.size, m_connectionHandle)) {
            break;
        }
        art.AcceptPacket();
    }
}
#endif
} // namespace modules
