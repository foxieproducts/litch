#include "modules/ble_service.hpp"

#include <algorithm>
#include <cstring>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/ble_control_protocol.hpp"
#include "modules/ble_service_internal.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/settings_state_wire_codec.hpp"

#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#endif

namespace modules {

namespace {
constexpr uint8_t GAME_HISTORY_FRAGMENT_VERSION = 1U;
constexpr uint8_t GAME_HISTORY_FRAGMENT_BEGIN = 1U << 0U;
constexpr uint8_t GAME_HISTORY_FRAGMENT_END = 1U << 1U;
constexpr uint8_t GAME_HISTORY_FRAGMENT_KNOWN =
    GAME_HISTORY_FRAGMENT_BEGIN | GAME_HISTORY_FRAGMENT_END;
constexpr size_t GAME_HISTORY_FRAGMENT_HEADER = 2U;
constexpr size_t GAME_HISTORY_FRAGMENT_BEGIN_HEADER = 4U;
#if defined(LITCH_BLE)
constexpr uint8_t OTA_CONTROL_FRAGMENT_VERSION = 1U;
constexpr uint8_t OTA_CONTROL_FRAGMENT_BEGIN = 1U << 0U;
constexpr uint8_t OTA_CONTROL_FRAGMENT_END = 1U << 1U;
constexpr uint8_t OTA_CONTROL_FRAGMENT_KNOWN =
    OTA_CONTROL_FRAGMENT_BEGIN | OTA_CONTROL_FRAGMENT_END;
constexpr size_t OTA_CONTROL_FRAGMENT_HEADER = 2U;
constexpr size_t OTA_CONTROL_FRAGMENT_BEGIN_HEADER = 4U;
constexpr uint8_t OTA_STATUS_FRAGMENT_VERSION = 1U;
constexpr uint8_t OTA_STATUS_FRAGMENT_BEGIN = 1U << 0U;
constexpr uint8_t OTA_STATUS_FRAGMENT_END = 1U << 1U;
constexpr size_t OTA_STATUS_FRAGMENT_HEADER = 2U;
constexpr size_t OTA_STATUS_FRAGMENT_BEGIN_HEADER = 4U;
constexpr uint8_t LITCHCRAFT_FRAGMENT_VERSION = 1U;
constexpr uint8_t LITCHCRAFT_FRAGMENT_BEGIN = 1U << 0U;
constexpr uint8_t LITCHCRAFT_FRAGMENT_END = 1U << 1U;
constexpr uint8_t LITCHCRAFT_FRAGMENT_KNOWN =
    LITCHCRAFT_FRAGMENT_BEGIN | LITCHCRAFT_FRAGMENT_END;
constexpr size_t LITCHCRAFT_FRAGMENT_HEADER = 2U;
constexpr size_t LITCHCRAFT_FRAGMENT_BEGIN_HEADER = 4U;

#if defined(LITCH_BLE_OTA)
class ConnectionOtaIngressLockGuard final {
  public:
    explicit ConnectionOtaIngressLockGuard(std::atomic_flag& lock)
        : m_lock(lock) {
        while (m_lock.test_and_set(std::memory_order_acquire)) {}
    }
    ~ConnectionOtaIngressLockGuard() {
        m_lock.clear(std::memory_order_release);
    }

  private:
    std::atomic_flag& m_lock;
};
#endif
#endif
} // namespace

#if defined(LITCH_BLE)

void BleService::OnConnectionSessionWrite(const uint8_t* const bytes,
                                          const size_t size) {
    m_sessionIngressCount.fetch_add(1U, std::memory_order_relaxed);
    BleEvent event;
    const uint32_t epoch = m_bleSettings.CurrentConnectionEpoch();
    const bool enqueued = MakeConnectionSessionIngressEvent(bytes, size, epoch, event) &&
        m_eventQueue.Enqueue(event);
#if defined(LITCH_BLE_OTA) && (defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE))
    if (bytes != nullptr && size >= connection_session_codec::HEADER_BYTES &&
        (bytes[0U] & 0x0fU) == uint8_t(connection_session_codec::Kind::Object)) {
        // Header receipt/enqueue evidence only, not authenticated admission.
        // NimBLE's existing characteristic callback is the sole tuple writer.
        m_liveObjectRxRevision.fetch_add(1U);
        m_liveObjectRxEpoch.store(epoch);
        m_liveObjectRxSequence.store(fxe::ReadU32LE(bytes + 6U));
        m_liveObjectRxEnqueued.store(enqueued ? 1U : 0U);
        m_liveObjectRxRevision.fetch_add(1U);
    }
#endif
    if (!enqueued) {
        m_droppedEvents.fetch_add(1U, std::memory_order_acq_rel);
    }
}

void BleService::OnConnectionSessionSubscription(const uint16_t value) {
    const uint32_t connectionEpoch = m_bleSettings.CurrentConnectionEpoch();
    m_connectionSessionSubscriptionValue.store(value,
                                                std::memory_order_relaxed);
    m_connectionSessionSubscriptionEpoch.store(connectionEpoch,
                                                std::memory_order_release);
    m_connectionSessionSubscriptionRevision.fetch_add(
        1U, std::memory_order_acq_rel);
    if (m_bleSettings.OwnsConnectionEpoch(connectionEpoch)) {
        m_connectionSessionSubscribed.store((value & 1U) != 0U,
                                             std::memory_order_release);
    }
}

bool BleService::PublishConnectionState(const uint8_t code,
                                        const uint32_t revision,
                                        const uint8_t* const data,
                                        const size_t len) {
    return !m_recoveryOnly &&
        m_connectionSession.PublishState(code, revision, data, len);
}

void BleService::PublishConnectionPhysicalInputState(
    const uint64_t occupancy) {
    if (m_recoveryOnly) return;
    if (m_connectionPhysicalOccupancyValid &&
        m_connectionPhysicalOccupancy == occupancy) {
        return;
    }
    m_connectionPhysicalOccupancy = occupancy;
    m_connectionPhysicalOccupancyValid = true;
    ++m_connectionPhysicalInputRevision;
    if (m_connectionPhysicalInputRevision == 0U) {
        m_connectionPhysicalInputRevision = 1U;
    }
    const auto body = physical_input_state_wire::Encode(
        m_connectionPhysicalOccupancy, m_connectionButtonBits);
    (void)m_connectionSession.PublishState(
        physical_input_state_wire::STATE_CODE,
        m_connectionPhysicalInputRevision, body.data(), body.size());
}

void BleService::PublishConnectionButtonState(const ButtonEvent& event,
                                              const uint32_t observedAtMs) {
    if (m_recoveryOnly) return;
    (void)observedAtMs;
    if (event.event != fxe::Button::Event::Press &&
        event.event != fxe::Button::Event::Release) {
        return;
    }
    const int8_t index = physical_input_state_wire::PackedSwitchIndex(event);
    if (index < 0) return;
    const uint16_t bit = static_cast<uint16_t>(1U << index);
    const uint16_t next = event.event == fxe::Button::Event::Press
        ? static_cast<uint16_t>(m_connectionButtonBits | bit)
        : static_cast<uint16_t>(m_connectionButtonBits & ~bit);
    if (next == m_connectionButtonBits) return;
    m_connectionButtonBits = next;
    if (!m_connectionPhysicalOccupancyValid) return;
    ++m_connectionPhysicalInputRevision;
    if (m_connectionPhysicalInputRevision == 0U) {
        m_connectionPhysicalInputRevision = 1U;
    }
    const auto body = physical_input_state_wire::Encode(
        m_connectionPhysicalOccupancy, m_connectionButtonBits);
    (void)m_connectionSession.PublishState(
        physical_input_state_wire::STATE_CODE,
        m_connectionPhysicalInputRevision, body.data(), body.size());
}

void BleService::ResetConnectionButtonState() {
    if (m_recoveryOnly) return;
    if (m_connectionButtonBits == 0U) return;
    m_connectionButtonBits = 0U;
    if (!m_connectionPhysicalOccupancyValid) return;
    ++m_connectionPhysicalInputRevision;
    if (m_connectionPhysicalInputRevision == 0U) {
        m_connectionPhysicalInputRevision = 1U;
    }
    const auto body = physical_input_state_wire::Encode(
        m_connectionPhysicalOccupancy, m_connectionButtonBits);
    (void)m_connectionSession.PublishState(
        physical_input_state_wire::STATE_CODE,
        m_connectionPhysicalInputRevision, body.data(), body.size());
}

bool BleService::CanRetainConnectionOtaStatus() const {
    if (!m_connectionSession.IsActivated()) return true;
    if (m_connectionSession.ReverseObjectStreamToken() == 0U) return false;
#if defined(LITCH_BLE_OTA)
    return m_connectionOtaDeferredTerminalStatus == nullptr &&
        (m_connectionOtaStatusPendingSize == 0U ||
         m_connectionOtaStatusReplaceable);
#else
    return m_connectionOtaStatusPendingSize == 0U;
#endif
}

#if defined(LITCH_BLE_OTA)
bool BleService::CanRetainConnectionOtaTerminal() const {
    return !m_connectionSession.IsActivated() ||
        m_connectionOtaDeferredTerminalStatus == nullptr;
}
#endif

void BleService::ResetConnectionOtaStatusOutput() {
    m_connectionOtaStatusPendingSize = 0U;
    m_connectionOtaStatusPendingOffset = 0U;
    m_connectionOtaStatusRevision = 0U;
#if defined(LITCH_BLE_OTA)
    m_connectionOtaStatusReplaceable = false;
    m_connectionOtaProgressDirty = false;
    m_connectionOtaStatusCompletesReboot = false;
    m_connectionOtaDeferredTerminalStatus = nullptr;
    m_connectionOtaDeferredTerminalDetail.Clear();
    m_connectionOtaRebootSequence = 0U;
#endif
}

bool BleService::QueueConnectionOtaStatus(const uint8_t* const data,
                                          const size_t len,
                                          const OtaStatusDelivery delivery) {
    if (!m_connectionSession.IsActivated() ||
        m_connectionSession.ReverseObjectStreamToken() == 0U ||
        data == nullptr ||
        len == 0U || len > m_connectionOtaStatusPending.size() ||
        !CanRetainConnectionOtaStatus()) {
        return false;
    }
    std::copy_n(data, len, m_connectionOtaStatusPending.begin());
    m_connectionOtaStatusPendingSize = static_cast<uint8_t>(len);
    m_connectionOtaStatusPendingOffset = 0U;
#if defined(LITCH_BLE_OTA)
    m_connectionOtaStatusReplaceable =
        delivery == OtaStatusDelivery::Progress;
    m_connectionOtaStatusCompletesReboot =
        delivery == OtaStatusDelivery::Reboot;
#else
    (void)delivery;
#endif
    UpdateConnectionOtaStatusOutput();
    return true;
}

void BleService::UpdateConnectionOtaStatusOutput() {
#if defined(LITCH_BLE_OTA)
    if (m_connectionOtaStatusPendingSize == 0U &&
        m_connectionOtaDeferredTerminalStatus != nullptr) {
        const char* const status = m_connectionOtaDeferredTerminalStatus;
        const auto detail = m_connectionOtaDeferredTerminalDetail;
        m_connectionOtaDeferredTerminalStatus = nullptr;
        m_connectionOtaDeferredTerminalDetail.Clear();
        (void)NotifyOtaStatus(status, detail.CStr());
        return;
    }
    if (m_connectionOtaStatusPendingSize == 0U &&
        m_connectionOtaProgressDirty) {
        m_connectionOtaProgressDirty = false;
        if (m_otaInProgress) (void)NotifyOtaStatus("progress");
        return;
    }
#endif
    if (m_connectionOtaStatusPendingSize == 0U) return;
    constexpr size_t MAX_BODY = sizeof(m_connectionOtaStatusPending) +
        OTA_STATUS_FRAGMENT_BEGIN_HEADER;
    if (m_connectionSession.PacketMaximum() <=
        connection_session_codec::HEADER_BYTES) {
        return;
    }
    const bool begins = m_connectionOtaStatusPendingOffset == 0U;
    const size_t headerSize = begins ? OTA_STATUS_FRAGMENT_BEGIN_HEADER
                                     : OTA_STATUS_FRAGMENT_HEADER;
    const size_t maximumBody = std::min<size_t>(
        m_connectionSession.ReverseObjectMaximumBody(),
        std::min<size_t>(MAX_BODY, m_connectionSession.PacketMaximum() -
            connection_session_codec::HEADER_BYTES));
    if (maximumBody <= headerSize) return;
    const size_t remaining = m_connectionOtaStatusPendingSize -
        m_connectionOtaStatusPendingOffset;
    const size_t count = std::min(maximumBody - headerSize, remaining);
    const bool ends = count == remaining;
    std::array<uint8_t, MAX_BODY> fragment{};
    fragment[0U] = OTA_STATUS_FRAGMENT_VERSION;
    fragment[1U] = (begins ? OTA_STATUS_FRAGMENT_BEGIN : 0U) |
        (ends ? OTA_STATUS_FRAGMENT_END : 0U);
    if (begins) {
        fxe::WriteU16LE(m_connectionOtaStatusPendingSize,
                        fragment.data() + 2U);
    }
    std::copy_n(
        m_connectionOtaStatusPending.data() +
            m_connectionOtaStatusPendingOffset,
        count, fragment.begin() + headerSize);
    uint32_t revision = m_connectionOtaStatusRevision + 1U;
    if (revision == 0U) revision = 1U;
    if (m_connectionSession.PublishReverseObject(
            ConnectionSession::OTA_STATUS_RECEIPT_CODE, revision,
            fragment.data(), headerSize + count)) {
        m_connectionOtaStatusRevision = revision;
        m_connectionOtaStatusPendingOffset = static_cast<uint8_t>(
            m_connectionOtaStatusPendingOffset + count);
        if (ends) {
#if defined(LITCH_BLE_OTA)
            if (m_connectionOtaStatusCompletesReboot &&
                m_connectionOtaRebootSequence == 0U) {
                m_connectionOtaRebootSequence =
                    m_connectionSession.LastPublishedReverseObjectSequence();
            }
#endif
            m_connectionOtaStatusPendingSize = 0U;
            m_connectionOtaStatusPendingOffset = 0U;
#if defined(LITCH_BLE_OTA)
            m_connectionOtaStatusReplaceable = false;
            m_connectionOtaStatusCompletesReboot = false;
#endif
        }
    }
}

#if defined(LITCH_BLE_OTA)
bool BleService::UpdateConnectionFrameStoreOutput() {
    if (m_litchcraftFrameResponsePeekCallback == nullptr ||
        m_litchcraftFrameResponseAcceptCallback == nullptr ||
        m_connectionSession.ReverseObjectStreamToken() == 0U) {
        m_connectionFrameStoreOutputOffset = 0U;
        m_connectionFrameStoreOutputCrc = 0U;
        return false;
    }
    std::array<uint8_t,
               litchcraft_frame_store::STORE_RESPONSE_MAX_BYTES> response{};
    size_t responseSize = 0U;
    if (!m_litchcraftFrameResponsePeekCallback(
            m_litchcraftFrameIngressContext, response.data(),
            response.size(), responseSize) ||
        responseSize < 13U || response[0U] != 0x4dU ||
        response[1U] != 2U) {
        m_connectionFrameStoreOutputOffset = 0U;
        m_connectionFrameStoreOutputCrc = 0U;
        return false;
    }
    const uint32_t crc = fxe::Crc32(response.data(), responseSize);
    if (crc != m_connectionFrameStoreOutputCrc ||
        m_connectionFrameStoreOutputOffset >= responseSize) {
        m_connectionFrameStoreOutputOffset = 0U;
        m_connectionFrameStoreOutputCrc = crc;
    }
    constexpr size_t MAX_BODY = ConnectionSession::REVERSE_OBJECT_PACKET_BYTES -
        connection_session_codec::HEADER_BYTES;
    const size_t maximumBody = std::min<size_t>(
        m_connectionSession.ReverseObjectMaximumBody(),
        std::min<size_t>(MAX_BODY, m_connectionSession.PacketMaximum() -
            connection_session_codec::HEADER_BYTES));
    const bool begins = m_connectionFrameStoreOutputOffset == 0U;
    const size_t headerSize = begins ? LITCHCRAFT_FRAGMENT_BEGIN_HEADER
                                     : LITCHCRAFT_FRAGMENT_HEADER;
    if (maximumBody <= headerSize) return true;
    const size_t remaining = responseSize - m_connectionFrameStoreOutputOffset;
    const size_t count = std::min(maximumBody - headerSize, remaining);
    const bool ends = count == remaining;
    std::array<uint8_t, MAX_BODY> fragment{};
    fragment[0U] = LITCHCRAFT_FRAGMENT_VERSION;
    fragment[1U] = (begins ? LITCHCRAFT_FRAGMENT_BEGIN : 0U) |
        (ends ? LITCHCRAFT_FRAGMENT_END : 0U);
    if (begins) {
        fxe::WriteU16LE(static_cast<uint16_t>(responseSize),
                        fragment.data() + 2U);
    }
    std::copy_n(response.data() + m_connectionFrameStoreOutputOffset, count,
                fragment.begin() + headerSize);
    uint32_t revision = m_connectionLitchcraftOutputRevision + 1U;
    if (revision == 0U) revision = 1U;
    if (!m_connectionSession.PublishReverseObject(
            ConnectionSession::LITCHCRAFT_RECEIPT_CODE, revision,
            fragment.data(), headerSize + count,
            response[2U] == litchcraft_frame_store::STORE_SNAPSHOT_CHUNK)) {
        return true;
    }
    m_connectionLitchcraftOutputRevision = revision;
    m_connectionFrameStoreOutputOffset = static_cast<uint16_t>(
        m_connectionFrameStoreOutputOffset + count);
    if (ends) {
        m_litchcraftFrameResponseAcceptCallback(m_litchcraftFrameIngressContext);
        m_connectionFrameStoreOutputOffset = 0U;
        m_connectionFrameStoreOutputCrc = 0U;
    }
    return true;
}
#endif

void BleService::UpdateConnectionLitchcraftOutput() {
    if (m_litchcraftGame == nullptr ||
        m_connectionSession.ReverseObjectStreamToken() == 0U) {
        m_connectionLitchcraftOutputOffset = 0U;
        return;
    }
    constexpr size_t MAX_BODY = ConnectionSession::OBJECT_PACKET_BYTES -
        connection_session_codec::HEADER_BYTES;
    const size_t maximumBody = std::min<size_t>(
        m_connectionSession.ReverseObjectMaximumBody(),
        std::min<size_t>(MAX_BODY, m_connectionSession.PacketMaximum() -
            connection_session_codec::HEADER_BYTES));
    // Shape new domain packets to fit one Object receipt whenever negotiated
    // capacity permits. Asking for a full ATT packet here would split every
    // large snapshot chunk into two notifications merely to add the envelope.
    constexpr size_t MINIMUM_DOMAIN_PACKET = 20U;
    const size_t domainMaximum = maximumBody >=
            MINIMUM_DOMAIN_PACKET + LITCHCRAFT_FRAGMENT_BEGIN_HEADER
        ? maximumBody - LITCHCRAFT_FRAGMENT_BEGIN_HEADER
        : MINIMUM_DOMAIN_PACKET;
    LitchcraftSession::BulkPacket packet;
    if (!m_litchcraftGame->Litchcraft().PeekPacket(packet, domainMaximum)) {
        m_connectionLitchcraftOutputOffset = 0U;
        return;
    }
    if (packet.size == 0U ||
        m_connectionLitchcraftOutputOffset >= packet.size) {
        m_connectionLitchcraftOutputOffset = 0U;
        return;
    }
    const bool begins = m_connectionLitchcraftOutputOffset == 0U;
    const size_t headerSize = begins ? LITCHCRAFT_FRAGMENT_BEGIN_HEADER
                                     : LITCHCRAFT_FRAGMENT_HEADER;
    if (maximumBody <= headerSize) return;
    const size_t remaining = packet.size - m_connectionLitchcraftOutputOffset;
    const size_t count = std::min(maximumBody - headerSize, remaining);
    const bool ends = count == remaining;
    std::array<uint8_t, MAX_BODY> fragment{};
    fragment[0U] = LITCHCRAFT_FRAGMENT_VERSION;
    fragment[1U] = (begins ? LITCHCRAFT_FRAGMENT_BEGIN : 0U) |
        (ends ? LITCHCRAFT_FRAGMENT_END : 0U);
    if (begins) {
        fxe::WriteU16LE(static_cast<uint16_t>(packet.size),
                        fragment.data() + 2U);
    }
    std::copy_n(
        packet.bytes.data() + m_connectionLitchcraftOutputOffset, count,
        fragment.begin() + headerSize);
    uint32_t revision = m_connectionLitchcraftOutputRevision + 1U;
    if (revision == 0U) revision = 1U;
    if (!m_connectionSession.PublishReverseObject(
            ConnectionSession::LITCHCRAFT_RECEIPT_CODE, revision,
            fragment.data(), headerSize + count,
            m_litchcraftGame->Litchcraft().BulkTransferActive())) {
        return;
    }
    m_connectionLitchcraftOutputRevision = revision;
    m_connectionLitchcraftOutputOffset = static_cast<uint16_t>(
        m_connectionLitchcraftOutputOffset + count);
    if (ends) {
        m_litchcraftGame->Litchcraft().AcceptPacket();
        m_connectionLitchcraftOutputOffset = 0U;
    }
}

void BleService::UpdateConnectionSessionNotifications() {
    if (!IsConnected()) return;
    const uint32_t connectionEpoch =
        m_bleSettings.CurrentConnectionEpoch();
    UpdateOwnerPairing();
    m_connectionSession.Update(connectionEpoch);
    DrainConnectionSessionCommands(connectionEpoch);
    // Republish retained current truth after a reconnect has created a fresh
    // session. Exact repeats are coalesced by ConnectionSession.
    if (!m_recoveryOnly && m_connectionPhysicalOccupancyValid &&
        m_connectionPhysicalInputRevision != 0U) {
        const auto body = physical_input_state_wire::Encode(
            m_connectionPhysicalOccupancy, m_connectionButtonBits);
        (void)m_connectionSession.PublishState(
            physical_input_state_wire::STATE_CODE,
            m_connectionPhysicalInputRevision, body.data(), body.size());
    }
    if (!m_recoveryOnly && m_litchcraftGame != nullptr) {
        auto& litchcraft = m_litchcraftGame->Litchcraft();
        std::array<uint8_t, 10U> state{};
        state[0U] = litchcraft.SelectedFrame();
        state[1U] = static_cast<uint8_t>(litchcraft.FrameCount());
        fxe::WriteU16LE(litchcraft.AnimationFrameElapsedMs(),
                        state.data() + 2U);
        state[4U] =
            (litchcraft.IsActive() ? 1U : 0U) |
            (litchcraft.IsAnimationPlaying() ? 2U : 0U) |
            (litchcraft.IsErasing() ? 4U : 0U) |
            (litchcraft.IsWaitingForLift() ? 8U : 0U) |
            (litchcraft.IsPlaybackTransferSuspended() ? 16U : 0U);
        state[5U] = litchcraft.Target();
        const auto color = litchcraft.Color();
        state[6U] = color.r;
        state[7U] = color.g;
        state[8U] = color.b;
        state[9U] = (litchcraft.UndoCount() != 0U ? 1U : 0U) |
                    (litchcraft.RedoCount() != 0U ? 2U : 0U);
        const uint32_t now = fxe::Millis();
        // Artwork/timing intent can change without changing the compact body.
        // A persistence-only save preserves this live source revision.
        const uint32_t sourceRevision = litchcraft.ConnectionRevision();
        const bool changed =
            sourceRevision != m_connectionSessionLitchcraftSourceRevision ||
            ble_internal::LitchcraftStateNeedsPublication(
                m_connectionSessionLitchcraftStateValid,
                m_connectionSessionLitchcraftState, state,
                now - m_connectionSessionLitchcraftStatePublishedAtMs);
        if (changed) {
            uint32_t nextRevision =
                m_connectionSessionLitchcraftStateRevision + 1U;
            if (nextRevision == 0U) nextRevision = 1U;
            if (m_connectionSession.PublishState(
                    ConnectionSession::LITCHCRAFT_STATE_CODE,
                    nextRevision, state.data(), state.size())) {
                m_connectionSessionLitchcraftState = state;
                m_connectionSessionLitchcraftStateValid = true;
                m_connectionSessionLitchcraftStateRevision = nextRevision;
                m_connectionSessionLitchcraftStatePublishedAtMs = now;
                m_connectionSessionLitchcraftSourceRevision = sourceRevision;
            }
        }
        const auto authority = litchcraft.StoreLiveAuthority();
        std::array<uint8_t, 10U> authorityBody{};
        authorityBody[0U] = 1U;
        authorityBody[1U] = authority.dirty ? 1U : 0U;
        fxe::WriteU32LE(authority.generation, authorityBody.data() + 2U);
        fxe::WriteU32LE(authority.revision, authorityBody.data() + 6U);
        if (!m_connectionSessionStoreAuthorityValid ||
            m_connectionSessionStoreAuthority != authorityBody) {
            uint32_t nextRevision =
                m_connectionSessionStoreAuthorityRevision + 1U;
            if (nextRevision == 0U) nextRevision = 1U;
            if (m_connectionSession.PublishState(
                    ConnectionSession::LITCHCRAFT_STORE_AUTHORITY_STATE_CODE,
                    nextRevision, authorityBody.data(),
                    authorityBody.size())) {
                m_connectionSessionStoreAuthority = authorityBody;
                m_connectionSessionStoreAuthorityValid = true;
                m_connectionSessionStoreAuthorityRevision = nextRevision;
            }
        }
        LitchcraftSession::PhysicalStoreEdit edit{};
        if (litchcraft.CopyPhysicalStoreEdit(edit)) {
            std::array<uint8_t, 10U> body{};
            fxe::WriteU32LE(edit.generation, body.data());
            body[4U] = edit.frame;
            body[5U] = edit.square;
            body[6U] = static_cast<uint8_t>(
                edit.mask | (edit.clearEffects ? 0x10U : 0U));
            body[7U] = edit.color.r;
            body[8U] = edit.color.g;
            body[9U] = edit.color.b;
            (void)m_connectionSession.PublishState(
                ConnectionSession::LITCHCRAFT_STORE_EDIT_STATE_CODE,
                edit.revision, body.data(), body.size());
        }
        std::array<uint8_t, ConnectionSession::OBJECT_PACKET_BYTES -
                                connection_session_codec::HEADER_BYTES>
            physicalBatch{};
        size_t physicalBatchSize = 0U;
        if (m_connectionSession.ReverseObjectStreamToken() != 0U &&
            litchcraft.PeekPhysicalStoreBatch(
                physicalBatch.data(), std::min<size_t>(physicalBatch.size(),
                    m_connectionSession.ReverseObjectMaximumBody()),
                physicalBatchSize) &&
            m_connectionSession.PublishReverseObject(
                physicalBatch[2U],
                fxe::ReadU16LE(physicalBatch.data() + 11U),
                physicalBatch.data(), physicalBatchSize)) {
            litchcraft.AcceptPhysicalStoreBatch();
        }
    }
    std::array<uint8_t, ConnectionSession::REVERSE_OBJECT_PACKET_BYTES> packet{};
    UpdateConnectionOtaStatusOutput();
#if defined(LITCH_BLE_OTA)
    UpdateObjectBenchmarkOutput(packet.data(), packet.size());
#endif
    // Fill the already bounded reverse window in this service pass. A single
    // packet per Update left the phone waiting for the board loop on every
    // snapshot chunk, even when the radio and receiver had free credits.
    for (uint8_t count = 0U; !m_recoveryOnly &&
         count < ConnectionSession::REVERSE_OBJECT_CAPACITY; ++count) {
        const uint32_t prior = m_connectionLitchcraftOutputRevision;
#if defined(LITCH_BLE_OTA)
        if (UpdateConnectionFrameStoreOutput()) {
            if (m_connectionLitchcraftOutputRevision == prior) break;
            continue;
        }
#endif
        UpdateConnectionLitchcraftOutput();
        if (m_connectionLitchcraftOutputRevision == prior) break;
    }
    UpdateConnectionSupportOutput();
    if (!m_recoveryOnly) UpdateConnectionGameHistoryOutput();
    if (!m_connectionSessionSubscribed.load(std::memory_order_acquire) ||
        m_connectionSessionEgressCharacteristic == nullptr ||
        !m_bleSettings.OwnsConnectionEpoch(connectionEpoch)) {
        return;
    }
    for (uint8_t count = 0U; count < 4U; ++count) {
        size_t size = 0U;
        if (!m_connectionSession.PeekOutput(
                connectionEpoch, packet.data(), packet.size(), size)) {
            break;
        }
        ++m_sessionDiagnostics.notifyAttempts;
        connection_session_codec::PacketView accessPacket;
        const bool decodedAccess = connection_session_codec::Codec::Decode(
            {packet.data(), size}, accessPacket, packet.size()) == connection_session_codec::Error::None;
        if (decodedAccess && (m_recoveryOnly || !OwnerProductAccess()) &&
            accessPacket.header.kind == connection_session_codec::Kind::Object &&
            accessPacket.header.code == ConnectionSession::SUPPORT_OBJECT_OPCODE) return;
        if (!m_recoveryOnly && !OwnerProductAccess() && decodedAccess &&
            ((accessPacket.header.kind == connection_session_codec::Kind::State &&
              (accessPacket.header.code != ConnectionSession::RUNTIME_HEALTH_STATE_CODE &&
               (accessPacket.header.code < 40U || accessPacket.header.code > 42U))) ||
             (accessPacket.header.kind == connection_session_codec::Kind::Object &&
              accessPacket.header.code != ConnectionSession::PUBLIC_SUPPORT_OBJECT_OPCODE) ||
             (accessPacket.header.kind == connection_session_codec::Kind::State &&
              accessPacket.header.code == owner_pairing_session_wire::STATUS_CODE &&
              accessPacket.body.Size() >= 4U &&
              (accessPacket.body[3U] & owner_pairing_session_wire::ConnectionOwnerReady) != 0U))) return;
        if (m_recoveryOnly && decodedAccess &&
            ((accessPacket.header.kind == connection_session_codec::Kind::Object &&
              accessPacket.header.code != ConnectionSession::PUBLIC_SUPPORT_OBJECT_OPCODE &&
              accessPacket.header.code != ConnectionSession::OTA_STATUS_RECEIPT_CODE) ||
             (accessPacket.header.kind == connection_session_codec::Kind::State &&
              accessPacket.header.code != ConnectionSession::RUNTIME_HEALTH_STATE_CODE))) return;
        if (!m_connectionSessionEgressCharacteristic->notify(
                packet.data(), size, m_connectionHandle)) {
            ++m_sessionDiagnostics.notifyFailures;
            m_sessionDiagnostics.lastFailureMs = fxe::Millis();
            m_sessionDiagnostics.lastFailureCode = packet[1U];
            m_sessionDiagnostics.lastFailureKind = packet[0U] & 0x0fU;
            break;
        }
        connection_session_codec::PacketView diagnosticPacket;
#if defined(LITCH_BLE_OTA)
        if (m_otaPendingReboot && !m_otaRebootGraceArmed &&
            m_connectionOtaRebootSequence != 0U &&
            connection_session_codec::Codec::Decode(
                {packet.data(), size}, diagnosticPacket, packet.size()) ==
                connection_session_codec::Error::None &&
            diagnosticPacket.header.kind == connection_session_codec::Kind::Object &&
            diagnosticPacket.header.code == ConnectionSession::OTA_STATUS_RECEIPT_CODE &&
            diagnosticPacket.header.sequence == m_connectionOtaRebootSequence) {
            // Submission to NimBLE is the existing reboot boundary; retaining
            // a fragment in the session window is not notification submission.
            m_otaRebootArmedAtMs = fxe::Millis();
            m_otaRebootGraceArmed = true;
        }
#endif
        if (connection_session_codec::Codec::Decode(
                {packet.data(), size}, diagnosticPacket, packet.size()) ==
                connection_session_codec::Error::None &&
            diagnosticPacket.header.kind == connection_session_codec::Kind::State &&
            diagnosticPacket.header.code < 64U) {
            m_sessionDiagnostics.sentStateCodes |= uint64_t{1U} << diagnosticPacket.header.code;
        }
        ++m_sessionNotifyCount;
#if defined(LITCH_BLE_OTA) && (defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE))
        if (decodedAccess && accessPacket.header.kind == connection_session_codec::Kind::Control &&
            accessPacket.header.code == uint8_t(connection_session_codec::Control::Progress) &&
            accessPacket.header.token == m_connectionSession.ObjectStreamToken()) {
            m_liveObjectProgressSubmitted = accessPacket.header.sequence;
            m_liveObjectProgressToken = accessPacket.header.token;
        }
#endif
        m_connectionSession.AcceptOutput(connectionEpoch);
    }
}

void BleService::UpdateConnectionGameHistoryOutput() {
    if (m_connectionSession.ReverseObjectStreamToken() == 0U ||
        m_connectionGameHistoryPeekCallback == nullptr ||
        m_connectionGameHistoryAcceptCallback == nullptr) {
        return;
    }
    constexpr size_t MAX_FRAME_BYTES = 20U;
    constexpr size_t MAX_BODY = MAX_FRAME_BYTES + GAME_HISTORY_FRAGMENT_BEGIN_HEADER;
    std::array<uint8_t, MAX_FRAME_BYTES> frame{};
    size_t frameSize = 0U;
    uint32_t token = 0U;
    if (!m_connectionGameHistoryPeekCallback(
            m_connectionGameHistoryOutputContext, frame.data(), frame.size(),
            frameSize, token,
            m_bleSettings.CurrentConnectionEpoch()) ||
        token == 0U || frameSize < 8U ||
        frameSize > frame.size()) {
        return;
    }
    if (m_connectionGameHistoryOutputToken == 0U) {
        m_connectionGameHistoryOutputToken = token;
        m_connectionGameHistoryOutputSize = static_cast<uint16_t>(frameSize);
        m_connectionGameHistoryOutputOffset = 0U;
    } else if (token != m_connectionGameHistoryOutputToken ||
               frameSize != m_connectionGameHistoryOutputSize ||
               m_connectionGameHistoryOutputOffset >= frameSize) {
        m_connectionGameHistoryOutputToken = 0U;
        m_connectionGameHistoryOutputSize = 0U;
        m_connectionGameHistoryOutputOffset = 0U;
        return;
    }
    const bool begins = m_connectionGameHistoryOutputOffset == 0U;
    const size_t headerSize = begins ? GAME_HISTORY_FRAGMENT_BEGIN_HEADER
                                     : GAME_HISTORY_FRAGMENT_HEADER;
    const size_t maximumBody = std::min<size_t>(
        m_connectionSession.ReverseObjectMaximumBody(),
        std::min<size_t>(MAX_BODY, m_connectionSession.PacketMaximum() -
            connection_session_codec::HEADER_BYTES));
    if (maximumBody <= headerSize) return;
    const size_t remaining = frameSize - m_connectionGameHistoryOutputOffset;
    const size_t count = std::min(maximumBody - headerSize, remaining);
    const bool ends = count == remaining;
    std::array<uint8_t, MAX_BODY> fragment{};
    fragment[0U] = GAME_HISTORY_FRAGMENT_VERSION;
    fragment[1U] = (begins ? GAME_HISTORY_FRAGMENT_BEGIN : 0U) |
        (ends ? GAME_HISTORY_FRAGMENT_END : 0U);
    if (begins) {
        fxe::WriteU16LE(static_cast<uint16_t>(frameSize),
                        fragment.data() + 2U);
    }
    std::copy_n(frame.data() + m_connectionGameHistoryOutputOffset, count,
                fragment.begin() + headerSize);
    if (!m_connectionSession.PublishReverseObject(
            ConnectionSession::GAME_HISTORY_RECEIPT_CODE, token,
            fragment.data(), headerSize + count)) {
        return;
    }
    m_connectionGameHistoryOutputOffset = static_cast<uint16_t>(
        m_connectionGameHistoryOutputOffset + count);
    if (ends) {
        m_connectionGameHistoryAcceptCallback(
            m_connectionGameHistoryOutputContext, token,
            m_bleSettings.CurrentConnectionEpoch());
        m_connectionGameHistoryOutputToken = 0U;
        m_connectionGameHistoryOutputSize = 0U;
        m_connectionGameHistoryOutputOffset = 0U;
    }
}

void BleService::DrainConnectionSessionCommands(
    const uint32_t connectionEpoch) {
    if (!m_bleSettings.OwnsConnectionEpoch(connectionEpoch)) {
        return;
    }
    using Window = fxe::OrderedReceiveWindow<
        ConnectionSession::ORDERED_CAPACITY,
        ConnectionSession::INTERACTIVE_PACKET_BYTES>;
    if (m_pendingConnectionControlAction.IsActive() &&
        (m_pendingConnectionControlAction.opcode == 6U ||
         m_pendingConnectionControlAction.opcode == 7U ||
         m_pendingConnectionControlAction.opcode == 10U)) {
        const uint8_t opcode = m_pendingConnectionControlAction.opcode;
        const bool runtimeEligible = m_litchcraftEligible && !IsOtaInProgress();
        if (m_litchcraftGame != nullptr) {
            const bool admitted = runtimeEligible
                ? m_litchcraftGame->TryBeginLitchcraftOrdered(
                    opcode, opcode == 10U ? m_pendingConnectionControlAction.body.data() : nullptr,
                    opcode == 10U ? m_pendingConnectionControlAction.body.size() : 0U)
                : m_litchcraftGame->TryBeginLitchcraftPresentation(
                    LitchcraftSession::PresentationAccess::Control);
            if (!admitted) return;
        }
        GamePersistenceCoordinator::LitchcraftPresentationGuard admission(m_litchcraftGame);
        if (!runtimeEligible && m_litchcraftGame != nullptr)
            m_litchcraftGame->Litchcraft().CancelPendingStoreHistory();
        const auto status = m_litchcraftGame == nullptr
            ? LitchcraftSession::Status::Unsupported
            : m_litchcraftGame->HandleLitchcraftConnectionOrdered(
                opcode, opcode == 10U ? m_pendingConnectionControlAction.body.data() : nullptr,
                opcode == 10U ? m_pendingConnectionControlAction.body.size() : 0U,
                runtimeEligible, connectionEpoch);
        if (status == LitchcraftSession::Status::Busy &&
            m_litchcraftGame != nullptr &&
            m_litchcraftGame->Litchcraft().IsStoreHistoryPending()) return;
        Window::Outcome outcome{};
        if (status != LitchcraftSession::Status::Ok) {
            outcome.disposition = Window::Disposition::Rejected;
            outcome.reason = static_cast<uint16_t>(status);
        }
        const uint32_t revision = m_litchcraftGame == nullptr ? 0U :
            status == LitchcraftSession::Status::Ok
                ? m_litchcraftGame->Litchcraft().StoreGuardRevision()
                : m_litchcraftGame->Litchcraft().StoreLiveAuthority().revision;
        if (!CompleteConnectionSessionCommand(
                m_pendingConnectionControlAction.streamToken,
                m_pendingConnectionControlAction.sequence, opcode,
                opcode == 10U ? m_pendingConnectionControlAction.body.data() : nullptr,
                opcode == 10U ? m_pendingConnectionControlAction.body.size() : 0U,
                outcome, revision)) return;
        m_pendingConnectionControlAction = PendingConnectionControlAction{};
    }
    if (m_pendingConnectionControlAction.IsActive()) {
        ControlMutationOutcome completion;
        if (m_controlMutationService == nullptr ||
            !m_controlMutationService->TryPeekCompletion(
                m_pendingConnectionControlAction.requestId, completion)) {
            return;
        }
        Window::Outcome outcome{};
        if (completion.status == ControlMutationStatus::Rejected) {
            outcome.disposition = Window::Disposition::Rejected;
            outcome.reason = static_cast<uint16_t>(completion.reason);
            if (outcome.reason == 0U) outcome.reason = 1U;
        }
        if (!CompleteConnectionSessionCommand(
                m_pendingConnectionControlAction.streamToken,
                m_pendingConnectionControlAction.sequence,
                ble_control_wire::ACTION_OPCODE,
                m_pendingConnectionControlAction.body.data(),
                m_pendingConnectionControlAction.body.size(), outcome,
                completion.settingsRevision)) {
            return;
        }
        (void)m_controlMutationService->AcceptCompletion(
            m_pendingConnectionControlAction.requestId);
        m_pendingConnectionControlAction = PendingConnectionControlAction{};
    }
    // A small bounded burst makes contiguous interactive work visible in the
    // same cooperative update without monopolizing Hall/render service.
    for (uint8_t count = 0U; count < 4U; ++count) {
        ConnectionSession::OrderedCommand command;
        if (!m_connectionSession.ReadApplying(command)) {
            const auto begun = m_connectionSession.BeginNext(command);
            if (begun != ConnectionSession::Result::Ok) break;
        }
        LitchcraftSession::Status status = LitchcraftSession::Status::Ok;
        uint16_t rejectionReason = 0U;
        uint32_t revision = 0U;
        if (m_recoveryOnly) {
            status = LitchcraftSession::Status::Unsupported;
        } else if (command.opcode == owner_pairing_session_wire::ORDERED_OPCODE) {
            owner_pairing_session_wire::Command request{};
            if (!OwnerPairingEnabled() || m_ownerRuntime == nullptr ||
                !owner_pairing_session_wire::DecodeCommand(command.body.Data(), command.body.Size(), request)) {
                status = LitchcraftSession::Status::Malformed;
            } else {
                const auto connection = OwnerConnection();
                BoardOwnerAddBaseline baseline{};
                const bool adding = m_ownerRuntime->Custody().GetOwnership() ==
                    BoardOwnerPairingController::Ownership::Owned;
                const bool baselineReady = !adding ||
                    (m_ownerPeerIdentity.connectionEpoch == connection.epoch &&
                     m_ownerPeerIdentity.connHandle == connection.handle &&
                     m_ownerPeerIdentity.peerIdentityAvailable && !m_ownerPeerIdentity.peerMatches &&
                     ReadOwnerAddBaseline(baseline));
                const bool accepted = request.action == owner_pairing_session_wire::Action::Cancel
                    ? m_ownerRuntime->Cancel(request, connection)
                    : baselineReady && m_ownerRuntime->Begin(request, connection, baseline);
                if (!accepted) status = LitchcraftSession::Status::Busy;
                UpdateOwnerPairing();
            }
        } else if (command.opcode == ble_identify::ORDERED_OPCODE) {
            ble_identify::Command request{};
            if (!ble_identify::Decode(command.body.Data(), command.body.Size(), request)) {
                status = LitchcraftSession::Status::Malformed;
            } else if (request.action == ble_identify::Action::Stop) {
                m_identifyStartPending = false;
                m_identifyStopPending = true;
            } else if (request.action == ble_identify::Action::Start) {
                ble_identify::PeerIdentity peer{};
                (void)ReadIdentifyNativePeer(peer);
                const auto result = m_identifyRateLimit.Start(peer, fxe::Millis());
                using Result = ble_identify::RateLimit::Result;
                if (result != Result::Allowed) {
                    status = LitchcraftSession::Status::Busy;
                    const auto reason = result == Result::Cooldown ? ble_identify::Rejection::Cooldown :
                        result == Result::Capacity ? ble_identify::Rejection::Capacity :
                        ble_identify::Rejection::PeerUnavailable;
                    rejectionReason = static_cast<uint16_t>(reason);
                } else {
                    m_identifyNumber = request.number;
                    m_identifyRed = request.red; m_identifyGreen = request.green; m_identifyBlue = request.blue;
                    m_identifyPadTwoDigits = false;
                    m_identifyDurationMs = request.durationMs;
                    m_identifyStopPending = false;
                    m_identifyStartPending = true;
                }
            }
            // Probe is an applied no-op. Old peers reject this opcode, so the
            // domain Result announces support without new envelope feature bits.
        } else if (!OwnerProductAccess()) {
            status = LitchcraftSession::Status::Unsupported;
        } else if (command.opcode == settings_state_wire::MUTATION_OPCODE) {
            if (command.body.Size() != settings_state_wire::MUTATION_BODY_BYTES ||
                command.body[0U] != settings_state_wire::VERSION ||
                m_controlMutationService == nullptr) {
                status = LitchcraftSession::Status::Malformed;
            } else {
                StaticJsonDocument<32> valueDocument;
                const char* key = nullptr;
                JsonVariantConst value;
                if (!settings_state_wire::DecodeMutation(
                        command.body[1U],
                        fxe::ReadU16LE(command.body.Data() + 2U), key,
                        valueDocument, value)) {
                    status = LitchcraftSession::Status::Malformed;
                } else {
                    const ControlMutationOutcome mutation =
                        m_controlMutationService->MutateSetting(key, value);
                    revision = mutation.settingsRevision;
                    if (mutation.status == ControlMutationStatus::Rejected) {
                        rejectionReason = static_cast<uint16_t>(mutation.reason);
                        status = LitchcraftSession::Status::Malformed;
                    }
                }
            }
        } else if (command.opcode == ble_control_wire::ACTION_OPCODE) {
            ParsedBleControlActionRequest request;
            if (m_controlMutationService == nullptr ||
                !TryDecodeConnectionControlAction(
                    command.body.Data(), command.body.Size(), request)) {
                status = LitchcraftSession::Status::Malformed;
            } else {
                const ControlMutationOutcome mutation =
                    m_controlMutationService->MutateAction(
                        request.action, 0U, request.owner,
                        request.coachingMutation, request.undoMoveCount);
                revision = mutation.settingsRevision;
                if (mutation.status == ControlMutationStatus::Accepted) {
                    m_pendingConnectionControlAction.streamToken =
                        command.streamToken;
                    m_pendingConnectionControlAction.sequence = command.sequence;
                    m_pendingConnectionControlAction.requestId = mutation.requestId;
                    m_pendingConnectionControlAction.opcode = ble_control_wire::ACTION_OPCODE;
                    std::copy(
                        command.body.Data(),
                        command.body.Data() + command.body.Size(),
                        m_pendingConnectionControlAction.body.begin());
                    break;
                }
                if (mutation.status == ControlMutationStatus::Rejected) {
                    rejectionReason = static_cast<uint16_t>(mutation.reason);
                    status = LitchcraftSession::Status::Malformed;
                }
            }
        } else if (command.opcode == ble_control_wire::CONSOLE_OPCODE) {
            const auto bodySize = command.body.Size();
            const uint8_t flags = bodySize >= 2U ? command.body[1U] : 0U;
            const bool begins = (flags & ble_control_wire::CONSOLE_BEGIN) != 0U;
            const bool ends = (flags & ble_control_wire::CONSOLE_END) != 0U;
            const size_t headerSize = begins
                ? ble_control_wire::CONSOLE_BEGIN_HEADER_BYTES
                : ble_control_wire::CONSOLE_HEADER_BYTES;
            bool valid = bodySize >= headerSize &&
                command.body[0U] == ble_control_wire::VERSION &&
                (flags & ~(ble_control_wire::CONSOLE_BEGIN |
                           ble_control_wire::CONSOLE_END)) == 0U;
            if (valid && begins) {
                valid = m_consoleInput.BeginStagedRecord(command.body[2U]);
            } else if (valid) {
                valid = m_consoleInput.HasStagedRecord();
            }
            const uint8_t* const payload = valid
                ? command.body.Data() + headerSize : nullptr;
            const size_t payloadSize = valid ? bodySize - headerSize : 0U;
            if (valid) {
                valid = payloadSize != 0U &&
                    std::memchr(payload, '\0', payloadSize) == nullptr &&
                    m_consoleInput.AppendStaged(payload, payloadSize);
            }
            if (valid && ends) {
                valid = m_consoleInput.CommitStagedRecord();
            }
            if (!valid) {
                m_consoleInput.DiscardStagedRecord();
                status = LitchcraftSession::Status::Malformed;
            }
        } else if (m_litchcraftGame == nullptr) {
            status = LitchcraftSession::Status::Unsupported;
        } else {
        if (!m_litchcraftGame->TryBeginLitchcraftOrdered(
                command.opcode, command.body.Data(), command.body.Size(),
                m_litchcraftEligible && !IsOtaInProgress())) break;
        GamePersistenceCoordinator::LitchcraftPresentationGuard admission(*m_litchcraftGame);
        if (command.opcode == ConnectionSession::LITCHCRAFT_MODE_ORDERED_OPCODE) {
            status = m_litchcraftGame->HandleLitchcraftConnectionMode(
                command.body.Data(), command.body.Size(),
                m_litchcraftEligible && !IsOtaInProgress());
        } else
#if defined(LITCH_BLE_OTA)
        if (command.opcode == 22U) {
            const bool valid = command.body.Size() == 9U &&
                command.body[0U] == 0x4dU && command.body[1U] == 2U &&
                command.body[2U] == 22U;
            if (!valid) {
                status = LitchcraftSession::Status::Malformed;
            } else if (m_litchcraftFramePriorityCallback == nullptr) {
                status = LitchcraftSession::Status::Unsupported;
            } else {
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
                m_liveStallArmed = false;
#endif
                m_litchcraftFramePriorityCallback(
                    m_litchcraftFrameIngressContext, command.body.Data(),
                    command.body.Size(), connectionEpoch);
                WakeLitchcraftFrameIngress();
            }
        } else
#endif
        {
            status = m_litchcraftGame->HandleLitchcraftConnectionOrdered(
                command.opcode, command.body.Data(), command.body.Size(),
                m_litchcraftEligible && !IsOtaInProgress(),
                connectionEpoch);
            if ((command.opcode == 6U || command.opcode == 7U || command.opcode == 10U) &&
                status == LitchcraftSession::Status::Busy &&
                m_litchcraftGame->Litchcraft().IsStoreHistoryPending()) {
                // Retain the existing Ordered position until the journal's
                // frame is presented. No new queue or successful early ACK.
                m_pendingConnectionControlAction.streamToken = command.streamToken;
                m_pendingConnectionControlAction.sequence = command.sequence;
                m_pendingConnectionControlAction.opcode = command.opcode;
                if (command.opcode == 10U) std::copy_n(command.body.Data(), 5U,
                    m_pendingConnectionControlAction.body.begin());
                break;
            }
        }
        revision = command.opcode == ConnectionSession::LITCHCRAFT_MODE_ORDERED_OPCODE
            ? m_litchcraftGame->Litchcraft().Revision()
            : m_litchcraftGame->Litchcraft().ConnectionRevision();
        if (command.opcode == 2U || command.opcode == 3U || command.opcode == 4U ||
            command.opcode == 6U || command.opcode == 7U ||
            command.opcode == 8U || command.opcode == 9U || command.opcode == 10U ||
            command.opcode == 11U) {
            const auto authority =
                m_litchcraftGame->Litchcraft().StoreLiveAuthority();
            if (authority.generation != 0U) {
                if (command.opcode == 2U) {
                    // Timeline retains its saved-root generation contract.
                    revision = authority.generation;
                } else {
                    revision = status == LitchcraftSession::Status::Ok
                        ? m_litchcraftGame->Litchcraft().StoreGuardRevision()
                        : authority.revision;
                }
            }
        }
        }
        Window::Outcome outcome{};
        if (status != LitchcraftSession::Status::Ok) {
            outcome.disposition = Window::Disposition::Rejected;
            outcome.reason = rejectionReason != 0U
                ? rejectionReason : static_cast<uint16_t>(status);
            if (outcome.reason == 0U) outcome.reason = 1U;
        }
        (void)CompleteConnectionSessionCommand(
            command.streamToken, command.sequence, command.opcode,
            command.body.Data(), command.body.Size(), outcome, revision);
    }
}

void BleService::DispatchConnectionObject(const ConnectionSession::ObjectCommand& object,
                                           const uint32_t connectionEpoch) {
    ObjectAdmission admission = ObjectAdmission::Invalid;
    if (object.opcode == ConnectionSession::PUBLIC_SUPPORT_OBJECT_OPCODE ||
        (!m_recoveryOnly && OwnerProductAccess() && object.opcode == ConnectionSession::SUPPORT_OBJECT_OPCODE)) {
        admission = HandleConnectionSupportObject(object, connectionEpoch);
    } else if (OwnerProductAccess() || m_recoveryOnly) {
        if (!m_recoveryOnly && object.opcode == ConnectionSession::GAME_HISTORY_OBJECT_OPCODE) {
            admission = HandleConnectionGameHistoryObject(object, connectionEpoch);
        }
        else if (!m_recoveryOnly && object.opcode == ConnectionSession::LITCHCRAFT_OBJECT_OPCODE) {
            admission = HandleConnectionLitchcraftObject(object, object.ingressOrdinal, connectionEpoch);
        } else if (object.opcode == ConnectionSession::OTA_CONTROL_OBJECT_OPCODE) {
            admission = HandleConnectionOtaControlObject(object, object.ingressOrdinal, connectionEpoch);
#if defined(LITCH_BLE_OTA)
        } else if (object.opcode == ConnectionSession::OTA_DATA_OBJECT_OPCODE) {
            admission = HandleConnectionOtaDataObject(object, object.ingressOrdinal);
#endif
        }
    }
    if (admission == ObjectAdmission::Accepted || admission == ObjectAdmission::RetainedCredit) {
        const auto accepted = m_connectionSession.AcceptObject(object.streamToken, object.sequence,
                                              admission == ObjectAdmission::RetainedCredit);
#if defined(LITCH_BLE_OTA) && (defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE))
        if (accepted == ConnectionSession::Result::Ok) m_liveStallProgressMs = fxe::Millis();
#else
        (void)accepted;
#endif
    } else if (admission == ObjectAdmission::Invalid) {
        m_connectionSession.RejectObject(object.streamToken, object.sequence);
    }
}

BleService::ObjectAdmission BleService::HandleConnectionSupportObject(
    const ConnectionSession::ObjectCommand& object, const uint32_t epoch) {
    const bool publicOnly = object.opcode == ConnectionSession::PUBLIC_SUPPORT_OBJECT_OPCODE;
    if ((!publicOnly && !OwnerProductAccess()) || m_supportCapture == nullptr ||
        !m_bleSettings.OwnsConnectionEpoch(epoch) || object.body.Size() != 6U ||
        object.body[0U] != 1U || object.body[1U] > (publicOnly ? 0U : 1U)) return ObjectAdmission::Invalid;
    if (m_connectionSupportSize != 0U) return ObjectAdmission::DeferredCapacity;
    const uint32_t request = fxe::ReadU32LE(object.body.Data() + 2U);
    if (request == 0U) return ObjectAdmission::Invalid;
    const size_t size = m_supportCapture(m_supportCaptureContext, publicOnly,
        object.body[1U], request, m_connectionSupportOutput.data(), m_connectionSupportOutput.size());
    if (size == 0U || size > m_connectionSupportOutput.size()) return ObjectAdmission::Invalid;
    m_connectionSupportSize = static_cast<uint16_t>(size);
    m_connectionSupportOffset = 0U;
    m_connectionSupportEpoch = epoch;
    m_connectionSupportCode = object.opcode;
    return ObjectAdmission::Accepted;
}

void BleService::UpdateConnectionSupportOutput() {
    if (m_connectionSupportSize == 0U) return;
    const bool publicOnly = m_connectionSupportCode == ConnectionSession::PUBLIC_SUPPORT_OBJECT_OPCODE;
    if ((!publicOnly && !OwnerProductAccess()) ||
        !m_bleSettings.OwnsConnectionEpoch(m_connectionSupportEpoch)) {
        m_connectionSupportSize = 0U;
        m_connectionSupportOffset = 0U;
        return;
    }
    const size_t maximum = std::min<size_t>(
        m_connectionSession.ReverseObjectMaximumBody(),
        m_connectionSession.PacketMaximum() - connection_session_codec::HEADER_BYTES);
    const bool begins = m_connectionSupportOffset == 0U;
    // Same v1 Begin/End + uint16 length framing as the existing GameHistory
    // and Litchcraft adapters; repair and credits remain in ConnectionSession.
    const size_t header = begins ? GAME_HISTORY_FRAGMENT_BEGIN_HEADER : GAME_HISTORY_FRAGMENT_HEADER;
    if (maximum <= header) return;
    const size_t count = std::min(maximum - header,
        size_t(m_connectionSupportSize - m_connectionSupportOffset));
    std::array<uint8_t, ConnectionSession::REVERSE_OBJECT_PACKET_BYTES - connection_session_codec::HEADER_BYTES> fragment{};
    fragment[0U] = GAME_HISTORY_FRAGMENT_VERSION;
    fragment[1U] = (begins ? GAME_HISTORY_FRAGMENT_BEGIN : 0U) |
        (count == m_connectionSupportSize - m_connectionSupportOffset ? GAME_HISTORY_FRAGMENT_END : 0U);
    if (begins) fxe::WriteU16LE(m_connectionSupportSize, fragment.data() + 2U);
    std::copy_n(m_connectionSupportOutput.data() + m_connectionSupportOffset, count, fragment.data() + header);
    uint32_t revision = m_connectionSupportRevision + 1U;
    if (revision == 0U) revision = 1U;
    if (!m_connectionSession.PublishReverseObject(m_connectionSupportCode, revision, fragment.data(), header + count)) return;
    m_connectionSupportRevision = revision;
    m_connectionSupportOffset = static_cast<uint16_t>(m_connectionSupportOffset + count);
    if (m_connectionSupportOffset == m_connectionSupportSize) {
        m_connectionSupportSize = 0U;
        m_connectionSupportOffset = 0U;
    }
}

BleService::ObjectAdmission BleService::HandleConnectionGameHistoryObject(
    const ConnectionSession::ObjectCommand& object,
    const uint32_t connectionEpoch) {
    const auto& body = object.body;
    if (connectionEpoch == 0U || object.opcode != ConnectionSession::GAME_HISTORY_OBJECT_OPCODE ||
        body.Size() < GAME_HISTORY_FRAGMENT_HEADER + 1U ||
        body[0U] != GAME_HISTORY_FRAGMENT_VERSION) {
        return ObjectAdmission::Invalid;
    }
    const uint8_t flags = body[1U];
    const bool begins = (flags & GAME_HISTORY_FRAGMENT_BEGIN) != 0U;
    const bool ends = (flags & GAME_HISTORY_FRAGMENT_END) != 0U;
    const size_t headerSize = begins ? GAME_HISTORY_FRAGMENT_BEGIN_HEADER
                                     : GAME_HISTORY_FRAGMENT_HEADER;
    bool valid = (flags & ~GAME_HISTORY_FRAGMENT_KNOWN) == 0U &&
        body.Size() > headerSize &&
        begins == (m_connectionGameHistoryExpected == 0U);
    if (valid && begins) {
        const uint16_t expected = fxe::ReadU16LE(body.Data() + 2U);
        valid = expected >= 8U &&
            expected <= m_connectionGameHistoryInput.size();
        if (valid) {
            m_connectionGameHistoryExpected = static_cast<uint8_t>(expected);
            m_connectionGameHistoryWritten = 0U;
        }
    }
    const size_t payloadSize = valid ? body.Size() - headerSize : 0U;
    const size_t completedSize =
        static_cast<size_t>(m_connectionGameHistoryWritten) + payloadSize;
    valid = valid && completedSize <= m_connectionGameHistoryExpected &&
        ends == (completedSize == m_connectionGameHistoryExpected);
    if (!valid) {
        m_connectionGameHistoryExpected = 0U;
        m_connectionGameHistoryWritten = 0U;
        return ObjectAdmission::Invalid;
    }
    if (ends) {
        std::array<uint8_t, 20U> completed = m_connectionGameHistoryInput;
        std::copy(body.Data() + headerSize, body.Data() + body.Size(),
                  completed.begin() + m_connectionGameHistoryWritten);
        if (m_connectionGameHistoryIngressCallback == nullptr) {
            m_connectionGameHistoryExpected = 0U;
            m_connectionGameHistoryWritten = 0U;
            return ObjectAdmission::Invalid;
        }
        if (!m_connectionGameHistoryIngressCallback(
                m_connectionGameHistoryIngressContext, completed.data(),
                completedSize, connectionEpoch, IsOtaInProgress())) {
            // An unadmitted Begin+End has no prefix to retain. Its exact
            // high-MTU retry must reach the owner again, just like a final
            // continuation fragment does with its retained prefix.
            if (begins) {
                m_connectionGameHistoryExpected = 0U;
                m_connectionGameHistoryWritten = 0U;
            }
            return ObjectAdmission::DeferredCapacity;
        }
        m_connectionGameHistoryExpected = 0U;
        m_connectionGameHistoryWritten = 0U;
        return ObjectAdmission::Accepted;
    }
    std::copy(body.Data() + headerSize, body.Data() + body.Size(),
              m_connectionGameHistoryInput.begin() +
                  m_connectionGameHistoryWritten);
    m_connectionGameHistoryWritten = static_cast<uint8_t>(completedSize);
    return ObjectAdmission::Accepted;
}

#if defined(LITCH_BLE_OTA)
BleService::ObjectAdmission BleService::HandleConnectionOtaDataObject(
    const ConnectionSession::ObjectCommand& object,
    const uint32_t ingressOrdinal) {
    if (object.opcode != ConnectionSession::OTA_DATA_OBJECT_OPCODE ||
        object.body.Size() == 0U || object.body.Size() > BleOtaChunkQueue::MAX_CHUNK_BYTES)
        return ObjectAdmission::Invalid;
    return m_otaChunkQueue.EnqueueReserved(object.body.Data(), object.body.Size(), ingressOrdinal, object.streamToken) ==
        BleOtaChunkQueue::Admission::Accepted ? ObjectAdmission::RetainedCredit : ObjectAdmission::DeferredCapacity;
}
#endif

BleService::ObjectAdmission BleService::HandleConnectionOtaControlObject(
    const ConnectionSession::ObjectCommand& object,
    const uint32_t ingressOrdinal,
    const uint32_t connectionEpoch) {
    const auto& body = object.body;
    if (object.opcode != ConnectionSession::OTA_CONTROL_OBJECT_OPCODE ||
        body.Size() < OTA_CONTROL_FRAGMENT_HEADER + 1U ||
        body[0U] != OTA_CONTROL_FRAGMENT_VERSION) {
        return ObjectAdmission::Invalid;
    }
    const uint8_t flags = body[1U];
    const bool begins = (flags & OTA_CONTROL_FRAGMENT_BEGIN) != 0U;
    const bool ends = (flags & OTA_CONTROL_FRAGMENT_END) != 0U;
    const size_t headerSize = begins ? OTA_CONTROL_FRAGMENT_BEGIN_HEADER
                                     : OTA_CONTROL_FRAGMENT_HEADER;
    bool valid = (flags & ~OTA_CONTROL_FRAGMENT_KNOWN) == 0U &&
        body.Size() > headerSize &&
        begins == (m_connectionOtaControlExpected == 0U);
    if (valid && begins) {
        const uint16_t expected = fxe::ReadU16LE(body.Data() + 2U);
        valid = expected != 0U &&
            expected <= ble_internal::MAX_OTA_CONTROL_EVENT_BYTES;
        if (valid) {
            m_connectionOtaControlExpected = static_cast<uint8_t>(expected);
            m_connectionOtaControlWritten = 0U;
        }
    }
    const size_t payloadSize = valid ? body.Size() - headerSize : 0U;
    const size_t completedSize =
        static_cast<size_t>(m_connectionOtaControlWritten) + payloadSize;
    valid = valid && completedSize <= m_connectionOtaControlExpected &&
        ends == (completedSize == m_connectionOtaControlExpected);
    if (!valid) {
        m_connectionOtaControlExpected = 0U;
        m_connectionOtaControlWritten = 0U;
        return ObjectAdmission::Invalid;
    }
    if (!ends) {
        std::copy(body.Data() + headerSize, body.Data() + body.Size(),
                  m_connectionOtaControlInput.begin() +
                      m_connectionOtaControlWritten);
        m_connectionOtaControlWritten = static_cast<uint8_t>(completedSize);
        return ObjectAdmission::Accepted;
    }

    std::array<char, 192U> completed = m_connectionOtaControlInput;
    std::copy(body.Data() + headerSize, body.Data() + body.Size(),
              completed.begin() + m_connectionOtaControlWritten);
    completed[completedSize] = '\0';
    bool enqueued = false;
    bool retainCredit = false;
    {
#if defined(LITCH_BLE_OTA)
        ConnectionOtaIngressLockGuard guard(m_otaIngressLock);
        const BleOtaControlIngressClassification classification =
            ClassifyBleOtaControlIngress(
                completed.data(), completedSize,
                ble_internal::MAX_OTA_CONTROL_EVENT_BYTES);
        if (!classification.acceptForQueue) {
            if (begins) {
                m_connectionOtaControlExpected = 0U;
                m_connectionOtaControlWritten = 0U;
            }
            return ObjectAdmission::Invalid;
        }
        uint32_t sessionEpoch = m_otaChunkQueue.CurrentEpoch();
        if (classification.beginCandidate && ++sessionEpoch == 0U) sessionEpoch = 1U;
#else
        constexpr uint32_t sessionEpoch = 0U;
#endif
        BleEvent event;
        event.type = BleEventType::OtaControl;
        std::copy_n(completed.data(), completedSize,
                    event.payload.otaControl.input);
        event.payload.otaControl.input[completedSize] = '\0';
        event.payload.otaControl.len = static_cast<uint8_t>(completedSize);
        event.payload.otaControl.sessionEpoch = sessionEpoch;
        event.payload.otaControl.connectionEpoch = connectionEpoch;
        event.ingressOrdinal = ingressOrdinal;
        retainCredit = BleEventQueue::UsesOrdinaryCapacity(event);
        event.payload.otaControl.objectCreditToken = retainCredit ? object.streamToken : 0U;
        enqueued = m_eventQueue.EnqueueNext(event);
#if defined(LITCH_BLE_OTA)
        if (enqueued && classification.beginCandidate) (void)m_otaChunkQueue.AdvanceEpoch();
#endif
    }
    if (!enqueued) {
        if (begins) {
            m_connectionOtaControlExpected = 0U;
            m_connectionOtaControlWritten = 0U;
        }
        return ObjectAdmission::DeferredCapacity;
    }
    m_connectionOtaControlExpected = 0U;
    m_connectionOtaControlWritten = 0U;
    return retainCredit ? ObjectAdmission::RetainedCredit : ObjectAdmission::Accepted;
}

BleService::ObjectAdmission BleService::HandleConnectionLitchcraftObject(
    const ConnectionSession::ObjectCommand& object,
    const uint32_t ingressOrdinal,
    const uint32_t connectionEpoch) {
    const auto& body = object.body;
    if (object.opcode != ConnectionSession::LITCHCRAFT_OBJECT_OPCODE ||
        body.Size() < LITCHCRAFT_FRAGMENT_HEADER + 1U ||
        body[0U] != LITCHCRAFT_FRAGMENT_VERSION) {
        return ObjectAdmission::Invalid;
    }
    const uint8_t flags = body[1U];
    const bool begins = (flags & LITCHCRAFT_FRAGMENT_BEGIN) != 0U;
    const bool ends = (flags & LITCHCRAFT_FRAGMENT_END) != 0U;
    const size_t headerSize = begins ? LITCHCRAFT_FRAGMENT_BEGIN_HEADER
                                     : LITCHCRAFT_FRAGMENT_HEADER;
    bool valid = (flags & ~LITCHCRAFT_FRAGMENT_KNOWN) == 0U &&
        body.Size() > headerSize &&
        begins == (m_connectionLitchcraftExpected == 0U);
    if (valid && begins) {
        const uint16_t expected = fxe::ReadU16LE(body.Data() + 2U);
        valid = expected != 0U &&
            expected <= m_connectionLitchcraftInput.size();
        if (valid) {
            m_connectionLitchcraftExpected = expected;
            m_connectionLitchcraftWritten = 0U;
        }
    }
    const size_t payloadSize = valid ? body.Size() - headerSize : 0U;
    const size_t completedSize =
        static_cast<size_t>(m_connectionLitchcraftWritten) + payloadSize;
    valid = valid && completedSize <= m_connectionLitchcraftExpected &&
        ends == (completedSize == m_connectionLitchcraftExpected);
    if (!valid) {
        m_connectionLitchcraftExpected = 0U;
        m_connectionLitchcraftWritten = 0U;
        return ObjectAdmission::Invalid;
    }
    if (!ends) {
        std::copy(body.Data() + headerSize, body.Data() + body.Size(),
                  m_connectionLitchcraftInput.begin() +
                      m_connectionLitchcraftWritten);
        m_connectionLitchcraftWritten =
            static_cast<uint16_t>(completedSize);
        return ObjectAdmission::Accepted;
    }
    std::copy(body.Data() + headerSize, body.Data() + body.Size(),
              m_connectionLitchcraftInput.begin() + m_connectionLitchcraftWritten);
    const auto& completed = m_connectionLitchcraftInput;
    if (completedSize >= 2U && completed[0U] == LitchcraftSession::MAGIC &&
        (completed[1U] == uint8_t(LitchcraftSession::Op::Enter) ||
         completed[1U] == uint8_t(LitchcraftSession::Op::Exit))) {
        m_connectionLitchcraftExpected = m_connectionLitchcraftWritten = 0U;
        return ObjectAdmission::Invalid;
    }
    const bool schema4 = completedSize >= 9U &&
        completed[0U] == 0x4dU && completed[1U] == 2U;
    const uint8_t operation = schema4 ? completed[2U] : 0U;
    const bool frameStore = schema4 &&
        ((operation == 1U && completedSize == 10U &&
          completed[9U] >= 4U && completed[9U] <= 7U) ||
         (operation >= 18U && operation <= 21U) ||
         (operation >= 24U && operation <= 26U));
    if (schema4 && operation >= 18U && operation <= 26U &&
        !frameStore) {
        m_connectionLitchcraftExpected = 0U;
        m_connectionLitchcraftWritten = 0U;
        return ObjectAdmission::Invalid;
    }
    const bool bulk = completedSize >= 2U &&
        ((completed[0U] == LitchcraftSession::MAGIC &&
          completed[1U] == LitchcraftSession::BULK_REPLACE) ||
         (completedSize >= 3U && completed[0U] == 0x4dU &&
          completed[1U] == 2U && completed[2U] >= 15U &&
          completed[2U] <= 17U)) || frameStore;
#if defined(LITCH_BLE_OTA)
    if (bulk && (IsOtaInProgress() || m_otaPendingReboot)) {
        // Bulk artwork and firmware image bytes share the bounded ingress
        // queue. Keep new Store work out through the reboot grace as well.
        m_connectionLitchcraftExpected = 0U;
        m_connectionLitchcraftWritten = 0U;
        return ObjectAdmission::Invalid;
    }
    bool enqueued = false;
    bool retainCredit = false;
    if (bulk) {
        // Reassembly can complete a 512-byte logical packet even at minimum
        // MTU. Each negotiated lease reserves that maximum byte cost.
        static_assert(ConnectionSession::OBJECT_PACKET_BYTES == BleOtaChunkQueue::MAX_CHUNK_BYTES);
        if (completedSize > ConnectionSession::OBJECT_PACKET_BYTES) {
            m_connectionLitchcraftExpected = m_connectionLitchcraftWritten = 0U;
            return ObjectAdmission::Invalid;
        }
        enqueued = m_otaChunkQueue.EnqueueReserved(
            completed.data(), completedSize, ingressOrdinal, object.streamToken) == BleOtaChunkQueue::Admission::Accepted;
    } else {
#else
    // Static Craft has no schema-4 animation/program or bulk queue owner.
    if (bulk || schema4) {
        m_connectionLitchcraftExpected = m_connectionLitchcraftWritten = 0U;
        return ObjectAdmission::Invalid;
    }
    bool enqueued = false;
    bool retainCredit = false;
    {
#endif
        BleEvent event;
        enqueued = MakeLitchcraftIngressEvent(
            completed.data(), completedSize, connectionEpoch, event);
        if (!enqueued) {
            m_connectionLitchcraftExpected = 0U;
            m_connectionLitchcraftWritten = 0U;
            return ObjectAdmission::Invalid;
        }
        retainCredit = BleEventQueue::UsesOrdinaryCapacity(event);
        if (retainCredit) {
            event.type = BleEventType::ConnectionLitchcraft;
            // Both alternatives share bytes/length; the token replaces epoch.
            // Priority copies retain their separate lane and connection epoch.
            event.payload.connectionLitchcraft.objectCreditToken = object.streamToken;
        }
        event.ingressOrdinal = ingressOrdinal;
        enqueued = m_eventQueue.EnqueueNext(event);
    }
    if (!enqueued) {
        // A refused Begin+End owns no admitted prefix. Keep an exact retry
        // eligible immediately, while a refused continuation retains its prefix.
        if (begins) {
            m_connectionLitchcraftExpected = 0U;
            m_connectionLitchcraftWritten = 0U;
        }
        return ObjectAdmission::DeferredCapacity;
    }
    m_connectionLitchcraftExpected = 0U;
    m_connectionLitchcraftWritten = 0U;
#if defined(LITCH_BLE_OTA)
    if (bulk) {
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
        if (frameStore && operation == 18U) {
            m_liveStallArmed = true;
            m_liveStallObjectToken = m_connectionSession.ObjectStreamToken();
            m_liveStallSnapshots = 0U;
            m_liveStallLastSampleMs = 0U;
        } else if (frameStore && operation == 21U) {
            m_liveStallArmed = false;
        }
#endif
        WakeLitchcraftFrameIngress();
        return ObjectAdmission::RetainedCredit;
    }
#endif
    return retainCredit ? ObjectAdmission::RetainedCredit : ObjectAdmission::Accepted;
}

bool BleService::CompleteConnectionSessionCommand(
    const uint32_t streamToken, const uint32_t sequence,
    const uint8_t opcode, const uint8_t* const body,
    const size_t bodySize,
    const fxe::OrderedReceiveWindow<
        ConnectionSession::ORDERED_CAPACITY,
        ConnectionSession::INTERACTIVE_PACKET_BYTES>::Outcome outcome,
    const uint32_t revision) {
    uint32_t digest = m_connectionSessionDomainDigest;
    using Window = fxe::OrderedReceiveWindow<
        ConnectionSession::ORDERED_CAPACITY,
        ConnectionSession::INTERACTIVE_PACKET_BYTES>;
    if (outcome.disposition == Window::Disposition::Applied) {
        std::array<uint8_t, 19U> identity{};
        fxe::WriteU32LE(m_connectionSessionDomainDigest, identity.data());
        fxe::WriteU32LE(revision, identity.data() + 4U);
        identity[8U] = opcode;
        if (bodySize != 0U) {
            std::copy(body, body + bodySize, identity.begin() + 9U);
        }
        digest = fxe::Crc32(identity.data(), 9U + bodySize);
    }
    const bool completed = m_connectionSession.Complete(
        streamToken, sequence, outcome, revision, digest) ==
        ConnectionSession::Result::Ok;
    if (completed && outcome.disposition == Window::Disposition::Applied) {
        m_connectionSessionDomainDigest = digest;
    }
    return completed;
}

void BleService::ClearPendingConnectionControlAction() {
    m_connectionSession.End(m_connectionSession.Generation());
    m_connectionSessionActivated.store(false, std::memory_order_release);
    if (m_pendingConnectionControlAction.requestId != 0U &&
        m_controlMutationService != nullptr) {
        (void)m_controlMutationService->AbandonCompletion(
            m_pendingConnectionControlAction.requestId);
    }
    if (m_litchcraftGame != nullptr) {
        m_litchcraftGame->Litchcraft().CancelPendingStoreHistory();
    }
    m_pendingConnectionControlAction = PendingConnectionControlAction{};
    m_connectionSupportSize = 0U;
    m_connectionSupportOffset = 0U;
    m_connectionSupportEpoch = 0U;
    m_connectionGameHistoryExpected = 0U;
    m_connectionGameHistoryWritten = 0U;
    m_connectionGameHistoryOutputToken = 0U;
    m_connectionGameHistoryOutputSize = 0U;
    m_connectionGameHistoryOutputOffset = 0U;
    ResetConnectionOtaStatusOutput();
#if defined(LITCH_BLE_OTA)
    m_objectBenchmark.EndLifetime();
#endif
    m_connectionOtaControlExpected = 0U;
    m_connectionOtaControlWritten = 0U;
    m_connectionLitchcraftExpected = 0U;
    m_connectionLitchcraftWritten = 0U;
    m_connectionLitchcraftOutputOffset = 0U;
#if defined(LITCH_BLE_OTA)
    m_connectionFrameStoreOutputOffset = 0U;
    m_connectionFrameStoreOutputCrc = 0U;
#endif
}

#endif

} // namespace modules
