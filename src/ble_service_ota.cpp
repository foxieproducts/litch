#include "modules/ble_service.hpp"

#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#endif
#include <cstdio>
#include <cstring>

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/ble_ota_transaction.hpp"
#include "modules/ble_service_internal.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litchcraft_session.hpp"

namespace modules {
namespace {

#if defined(LITCH_BLE_OTA)
constexpr uint32_t kSlowOtaWriteThresholdMs = 10U;

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

const char* ChunkFailureDetail(const BleOtaTransactionResult result) {
    switch (result) {
        case BleOtaTransactionResult::EmptyChunk:
            return "empty_chunk";
        case BleOtaTransactionResult::ChunkTooLarge:
            return "chunk_too_large";
        case BleOtaTransactionResult::ChunkOverNegotiated:
            return "chunk_too_large";
        case BleOtaTransactionResult::ChunkOvershoot:
            return "size_overshoot";
        case BleOtaTransactionResult::WriteFailed:
            return "write_failed";
        default:
            return "invalid_chunk";
    }
}
#endif

} // namespace

#if defined(LITCH_BLE_OTA)
BleOtaTransactionOps BleService::GetOtaTransactionOps() {
    return BleOtaTransactionOps{
        this,
        &BleService::BeginOtaBackend,
        &BleService::WriteOtaBackend,
        &BleService::FlushOtaBackend,
        &BleService::EndOtaBackend,
        &BleService::AbortOtaBackend,
    };
}

bool BleService::BeginOtaBackend(void* const context, const uint32_t size) {
    BleService& self = *static_cast<BleService*>(context);
    self.m_otaWriteStageLen = 0U;
    if (self.m_otaSimulationMode) {
        return true;
    }
    return self.m_otaPlatformAdapter != nullptr &&
           self.m_otaPlatformAdapter->Begin(size);
}

bool BleService::WriteOtaBackend(void* const context,
                                 const uint8_t* const data,
                                 const size_t len) {
    BleService& self = *static_cast<BleService*>(context);
    if (self.m_otaSimulationMode) {
        return !self.m_objectBenchmark.IsForward() ||
            self.m_objectBenchmark.ConsumeForward(self.BenchmarkLifetime(), data, len);
    }
    return self.AppendOtaWriteStage(data, len);
}

bool BleService::FlushOtaBackend(void* const context) {
    BleService& self = *static_cast<BleService*>(context);
    return self.m_otaSimulationMode || self.FlushOtaWriteStageBuffer();
}

bool BleService::EndOtaBackend(void* const context,
                               const bool evenIfRemaining) {
    BleService& self = *static_cast<BleService*>(context);
    if (self.m_otaSimulationMode) {
        return true;
    }
    return !evenIfRemaining && self.m_otaPlatformAdapter != nullptr &&
           self.m_otaPlatformAdapter->End();
}

void BleService::AbortOtaBackend(void* const context) {
    BleService& self = *static_cast<BleService*>(context);
    if (!self.m_otaSimulationMode) {
        if (self.m_otaPlatformAdapter != nullptr) {
            self.m_otaPlatformAdapter->Abort();
        }
    }
}

void BleService::RejectOtaBeginCandidate(const uint32_t sessionEpoch) {
    if (m_otaPendingReboot) {
        (void)m_otaChunkQueue.AdvanceEpochIfCurrent(sessionEpoch);
        return;
    }
    AbortOtaTransfer(true, true, sessionEpoch);
}

void BleService::AbortOtaTransfer(const bool stopIdentify,
                                  const bool invalidateQueuedData,
                                  const uint32_t ownedSessionEpoch) {
    uint32_t cleanupEpoch = ownedSessionEpoch;
    if (cleanupEpoch == 0U) {
        cleanupEpoch = m_otaTransaction.SessionEpoch();
    }
    if (cleanupEpoch == 0U) {
        cleanupEpoch = m_otaChunkQueue.CurrentEpoch();
    }
    if (m_objectBenchmark.IsForward()) m_objectBenchmark.Cancel();
    m_otaTransaction.Abort(GetOtaTransactionOps());
    if (invalidateQueuedData) {
        (void)m_otaChunkQueue.AdvanceEpochIfCurrent(cleanupEpoch);
    }
    ResetOtaTransferState(stopIdentify);
}

bool BleService::HandleRejectedOtaBeginCandidateIngressFailure() {
    // Keep the rejection intent in its existing ingress owner. Reserve the
    // terminal continuation for an active transfer's immediate safety abort.
    if (!CanRetainConnectionOtaStatus()) return false;
    uint32_t sessionEpoch = 0U;
    BleOtaRejectedBeginReason reason =
        BleOtaRejectedBeginReason::None;
    {
        OtaIngressLockGuard guard(m_otaIngressLock);
        sessionEpoch = m_otaRejectedBeginCandidateEpoch;
        reason = m_otaRejectedBeginCandidateReason;
        m_otaRejectedBeginCandidateEpoch = 0U;
        m_otaRejectedBeginCandidateReason =
            BleOtaRejectedBeginReason::None;
    }
    if (sessionEpoch == 0U) {
        return false;
    }

    const char* const detail =
        m_otaPendingReboot ? "reboot_pending"
                           : BleOtaRejectedBeginErrorDetail(reason);
    logger::Warn("BLE OTA begin candidate dropped before dispatch: reason=%s epoch=%lu",
                 detail,
                 static_cast<unsigned long>(sessionEpoch));
    RejectOtaBeginCandidate(sessionEpoch);
    NotifyOtaStatus("error", detail);
    return true;
}

bool BleService::FlushOtaWriteStageBuffer() {
    if (m_otaSimulationMode || m_otaWriteStageLen == 0U) {
        return true;
    }

    const size_t pendingLen = m_otaWriteStageLen;
    const fxe::ElapsedTime elapsed = fxe::ElapsedTime::StartNow();
    const bool accepted = m_otaPlatformAdapter != nullptr &&
                          m_otaPlatformAdapter->Write(
                              m_otaWriteStageBuffer, pendingLen);
    const size_t written = accepted ? pendingLen : 0U;
    const uint32_t flushMs = elapsed.ElapsedMs();

    ++m_otaWriteCallCount;
    m_otaWriteBytesFlushed += static_cast<uint32_t>(written);
    m_otaWriteTotalMs += flushMs;
    if (flushMs > m_otaWriteMaxMs) {
        m_otaWriteMaxMs = flushMs;
    }
    if (flushMs >= kSlowOtaWriteThresholdMs) {
        ++m_otaWriteSlowCount;
    }

    if (written != pendingLen) {
        logger::Error("BLE OTA staged write failed: wrote=%lu expected=%lu flush_ms=%lu rx=%lu total=%lu",
                      static_cast<unsigned long>(written),
                      static_cast<unsigned long>(pendingLen),
                      static_cast<unsigned long>(flushMs),
                      static_cast<unsigned long>(m_otaBytesReceived),
                      static_cast<unsigned long>(m_otaExpectedSize));
        return false;
    }

    m_otaWriteStageLen = 0U;
    return true;
}

bool BleService::AppendOtaWriteStage(const uint8_t* const data, const size_t len) {
    if (data == nullptr || len == 0U) {
        return true;
    }

    size_t offset = 0U;
    while (offset < len) {
        if (m_otaWriteStageLen == OTA_WRITE_STAGE_CAPACITY && !FlushOtaWriteStageBuffer()) {
            return false;
        }

        const size_t remainingStage = OTA_WRITE_STAGE_CAPACITY - m_otaWriteStageLen;
        const size_t remainingData = len - offset;
        const size_t copyLen = (remainingData < remainingStage) ? remainingData : remainingStage;
        std::memcpy(m_otaWriteStageBuffer + m_otaWriteStageLen, data + offset, copyLen);
        m_otaWriteStageLen += copyLen;
        offset += copyLen;

        if (m_otaWriteStageLen == OTA_WRITE_STAGE_CAPACITY && !FlushOtaWriteStageBuffer()) {
            return false;
        }
    }
    return true;
}

void BleService::LogOtaWriteMetrics(const char* const status) const {
    if (m_otaSimulationMode || m_otaWriteCallCount == 0U) {
        return;
    }

    const uint32_t avgWriteMs = m_otaWriteTotalMs / m_otaWriteCallCount;
    logger::Trace("BLE OTA write metrics: status=%s calls=%lu bytes=%lu total_ms=%lu avg_ms=%lu max_ms=%lu slow_count=%lu stage=%lu",
                 (status != nullptr) ? status : "unknown",
                 static_cast<unsigned long>(m_otaWriteCallCount),
                 static_cast<unsigned long>(m_otaWriteBytesFlushed),
                 static_cast<unsigned long>(m_otaWriteTotalMs),
                 static_cast<unsigned long>(avgWriteMs),
                 static_cast<unsigned long>(m_otaWriteMaxMs),
                 static_cast<unsigned long>(m_otaWriteSlowCount),
                 static_cast<unsigned long>(OTA_WRITE_STAGE_CAPACITY));

    const uint32_t avgNotifyMs =
        (m_otaNotifyCount == 0U) ? 0U : (m_otaNotifyTotalMs / m_otaNotifyCount);
    logger::Trace("BLE OTA path metrics: status=%s enqueue_count=%lu enqueue_drop=%lu queue_highwater=%u drain_cycles=%lu drain_count=%lu drain_max_chunks=%lu notify_count=%lu notify_avg_ms=%lu notify_max_ms=%lu",
                 (status != nullptr) ? status : "unknown",
                 static_cast<unsigned long>(m_otaChunkEnqueueCount),
                 static_cast<unsigned long>(m_otaChunkEnqueueDrops),
                 static_cast<unsigned>(m_otaChunkQueue.HighWaterMark()),
                 static_cast<unsigned long>(m_otaChunkDrainCycles),
                 static_cast<unsigned long>(m_otaChunkDrainCount),
                 static_cast<unsigned long>(m_otaChunkDrainMaxPerCycle),
                 static_cast<unsigned long>(m_otaNotifyCount),
                 static_cast<unsigned long>(avgNotifyMs),
                 static_cast<unsigned long>(m_otaNotifyMaxMs));
}

void BleService::HandleOtaChunkPacket(const uint8_t* const data,
                                      const size_t len,
                                      const uint32_t sessionEpoch) {
    if (!m_otaTransaction.IsActive()) {
        return;
    }

    const BleOtaTransactionResult result = m_otaTransaction.WriteChunk(
        sessionEpoch, data, len, fxe::Millis(), GetOtaTransactionOps());
    if (result == BleOtaTransactionResult::StaleSession) {
        logger::Warn("BLE OTA stale queued chunk rejected: epoch=%lu active=%lu len=%lu",
                     static_cast<unsigned long>(sessionEpoch),
                     static_cast<unsigned long>(m_otaTransaction.SessionEpoch()),
                     static_cast<unsigned long>(len));
        return;
    }
    if (result != BleOtaTransactionResult::Accepted) {
        const char* const detail = ChunkFailureDetail(result);
        LogOtaWriteMetrics(detail);
        AbortOtaTransfer(true, true, sessionEpoch);
        NotifyOtaStatus("error", detail);
        return;
    }

    m_otaBytesReceived = m_otaTransaction.ReceivedSize();
    uint8_t progressPercent = 0U;
    if (m_otaExpectedSize > 0U) {
        const uint32_t rawPercent = (m_otaBytesReceived * 100U) / m_otaExpectedSize;
        progressPercent =
            (rawPercent > 100U) ? 100U : static_cast<uint8_t>(rawPercent);
    }
    m_otaProgressPercent = progressPercent;
    if (m_otaSimulationMode) {
        return;
    }

    const uint8_t progressStep = static_cast<uint8_t>(progressPercent / 10U);
    if (progressStep != m_otaLastLoggedStep) {
        m_otaLastLoggedStep = progressStep;
        logger::Trace("BLE OTA progress: %u%% (%lu/%lu)",
                     static_cast<unsigned>(progressPercent),
                     static_cast<unsigned long>(m_otaBytesReceived),
                     static_cast<unsigned long>(m_otaExpectedSize));
    }
    m_otaLastDisplayedPercent = progressPercent;

    const uint32_t nowMs = fxe::Millis();
    const bool statusDue = m_lastOtaStatusMs == 0U ||
                           (nowMs - m_lastOtaStatusMs) >=
                               ble_internal::MIN_OTA_STATUS_INTERVAL_MS;
    const bool complete =
        m_otaExpectedSize > 0U && m_otaBytesReceived >= m_otaExpectedSize;
    if (statusDue || complete) {
        m_lastOtaStatusMs = nowMs;
        NotifyOtaStatus("progress");
    }
}

bool BleService::HandleOtaQueueFailure() {
    if (!CanRetainConnectionOtaTerminal()) return false;
    if (!m_otaTransaction.IsActive()) {
        return false;
    }

    BleOtaChunkQueueFailure failure = BleOtaChunkQueueFailure::None;
    if (!m_otaChunkQueue.TryConsumeFailure(
            m_otaTransaction.SessionEpoch(), failure)) {
        return false;
    }

    const char* detail = "invalid_chunk";
    if (failure == BleOtaChunkQueueFailure::Empty) {
        detail = "empty_chunk";
    } else if (failure == BleOtaChunkQueueFailure::TooLarge) {
        detail = "chunk_too_large";
    } else if (failure == BleOtaChunkQueueFailure::Full) {
        detail = "queue_full";
    }
    logger::Error("BLE OTA queued ingress rejected: %s", detail);
    LogOtaWriteMetrics(detail);
    const uint32_t sessionEpoch = m_otaTransaction.SessionEpoch();
    AbortOtaTransfer(true, true, sessionEpoch);
    NotifyOtaStatus("error", detail);
    return true;
}
#endif

#if defined(LITCH_BLE)
bool BleService::NotifyOtaStatus(const char* const status, const char* const detail) {
    if (!IsConnected() || status == nullptr ||
        status[0] == '\0') {
        return false;
    }

    uint16_t mtu = GetMtu();
    if (mtu < 23U) {
        mtu = 23U;
    }
    size_t maxNotifyPayload = static_cast<size_t>(mtu - 3U);
    if (maxNotifyPayload < 20U) {
        maxNotifyPayload = 20U;
    }
    if (maxNotifyPayload > ble_internal::BLE_MAX_ATT_PAYLOAD_BYTES) {
        maxNotifyPayload = ble_internal::BLE_MAX_ATT_PAYLOAD_BYTES;
    }
    const bool sharedStatus = m_connectionSession.IsActivated();
    if (sharedStatus) {
        maxNotifyPayload = m_connectionOtaStatusPending.size();
    }

    char payload[160] = {};
    size_t payloadLen = 0U;
    if (detail != nullptr && detail[0] != '\0' && std::strcmp(status, "version") == 0) {
        std::snprintf(payload, sizeof(payload), "{\"v\":\"%s\"}", detail);
    } else if (detail != nullptr && detail[0] != '\0' && std::strcmp(status, "error") == 0) {
        payloadLen = FormatBleOtaErrorPayload(
            detail, maxNotifyPayload, payload, sizeof(payload));
    } else if (detail != nullptr && detail[0] != '\0') {
        std::snprintf(payload,
                      sizeof(payload),
                      "{\"st\":\"%s\",\"detail\":\"%s\",\"pct\":%u,\"rx\":%lu,\"total\":%lu}",
                      status,
                      detail,
                      static_cast<unsigned>(m_otaProgressPercent),
                      static_cast<unsigned long>(m_otaBytesReceived),
                      static_cast<unsigned long>(m_otaExpectedSize));
#if defined(LITCH_BLE_OTA)
    } else if (std::strcmp(status, "done") == 0 && m_otaWriteCallCount != 0U) {
        std::snprintf(payload,
                      sizeof(payload),
                      "{\"st\":\"done\",\"pct\":%u,\"rx\":%lu,\"total\":%lu,\"wb\":%lu,\"wm\":%lu}",
                      static_cast<unsigned>(m_otaProgressPercent),
                      static_cast<unsigned long>(m_otaBytesReceived),
                      static_cast<unsigned long>(m_otaExpectedSize),
                      static_cast<unsigned long>(m_otaWriteBytesFlushed),
                      static_cast<unsigned long>(m_otaWriteTotalMs));
#endif
    } else {
        std::snprintf(payload,
                      sizeof(payload),
                      "{\"st\":\"%s\",\"pct\":%u,\"rx\":%lu,\"total\":%lu}",
                      status,
                      static_cast<unsigned>(m_otaProgressPercent),
                      static_cast<unsigned long>(m_otaBytesReceived),
                      static_cast<unsigned long>(m_otaExpectedSize));
    }

    if (payloadLen == 0U) {
        payloadLen = std::strlen(payload);
    }
    if (payloadLen > maxNotifyPayload) {
        if (std::strcmp(status, "version") == 0) {
            const char* const version = (detail != nullptr) ? detail : "";
            const size_t maxVersionLen =
                (maxNotifyPayload > 8U) ? ((maxNotifyPayload - 8U < 32U)
                                             ? maxNotifyPayload - 8U
                                             : 32U)
                                        : 1U;
            std::snprintf(payload,
                          sizeof(payload),
                          "{\"v\":\"%.*s\"}",
                          static_cast<int>(maxVersionLen),
                          version);
        } else if (std::strcmp(status, "progress") == 0) {
            std::snprintf(payload,
                          sizeof(payload),
                          "{\"p\":%u,\"r\":%lu}",
                          static_cast<unsigned>(m_otaProgressPercent),
                          static_cast<unsigned long>(m_otaBytesReceived));
            if (std::strlen(payload) > maxNotifyPayload) {
                // Receive acknowledgement is the safety signal for bounded
                // senders. Prefer it once both fields no longer fit (for
                // example, after 999,999 bytes on a 20-byte ATT payload).
                std::snprintf(payload,
                              sizeof(payload),
                              "{\"r\":%lu}",
                              static_cast<unsigned long>(m_otaBytesReceived));
            }
        } else {
            const char statusToken[2] = {status[0], '\0'};
            std::snprintf(payload, sizeof(payload), "{\"s\":\"%s\"}", statusToken);
        }

        payloadLen = std::strlen(payload);
        if (payloadLen > maxNotifyPayload) {
            payload[maxNotifyPayload] = '\0';
            payloadLen = maxNotifyPayload;
        }
    }

    if (sharedStatus) {
#if defined(LITCH_BLE_OTA)
        const bool progress = std::strcmp(status, "progress") == 0;
        const bool done = std::strcmp(status, "done") == 0;
        const OtaStatusDelivery delivery = progress
            ? OtaStatusDelivery::Progress
            : (done && m_otaPendingReboot ? OtaStatusDelivery::Reboot
                                         : OtaStatusDelivery::Reliable);
        if (QueueConnectionOtaStatus(
                reinterpret_cast<const uint8_t*>(payload), payloadLen,
                delivery)) {
            if (progress) m_connectionOtaProgressDirty = false;
            return true;
        }
        if (progress) {
            m_connectionOtaProgressDirty = true;
            return true;
        }
        const char* terminal = nullptr;
        if (done) terminal = "done";
        else if (std::strcmp(status, "error") == 0) terminal = "error";
        else if (std::strcmp(status, "aborted") == 0) terminal = "aborted";
        if (terminal != nullptr && CanRetainConnectionOtaTerminal() &&
            m_connectionOtaDeferredTerminalDetail.Assign(
                detail != nullptr ? std::string_view(detail) : std::string_view{})) {
            m_connectionOtaDeferredTerminalStatus = terminal;
            return true;
        }
        return false;
#else
        return QueueConnectionOtaStatus(
            reinterpret_cast<const uint8_t*>(payload), payloadLen);
#endif
    }
    if (m_otaControlCharacteristic == nullptr) return false;
    const fxe::ElapsedTime notifyElapsed = fxe::ElapsedTime::StartNow();
    m_otaControlCharacteristic->setValue(
        reinterpret_cast<const uint8_t*>(payload), payloadLen);
    const bool submitted = m_otaControlCharacteristic->notify();
    ++m_otaNotifyCount;
    const uint32_t notifyMs = notifyElapsed.ElapsedMs();
    m_otaNotifyTotalMs += notifyMs;
    if (notifyMs > m_otaNotifyMaxMs) {
        m_otaNotifyMaxMs = notifyMs;
    }
    return submitted;
}
#endif

#if defined(LITCH_BLE_OTA)
void BleService::WakeLitchcraftFrameIngress() {
    if (m_litchcraftFrameIngressCallback == nullptr ||
        m_litchcraftGame == nullptr) {
        return;
    }
    const auto& art = m_litchcraftGame->Litchcraft();
    m_litchcraftFrameIngressCallback(
        m_litchcraftFrameIngressContext, m_otaChunkQueue,
        {art.SessionId(), art.SelectedFrame(), art.AnimationTopology(),
         art.IsActive(), art.IsAnimationPlaying()},
        m_bleSettings.CurrentConnectionEpoch(),
        m_connectionSession.PacketMaximum());
}

bool BleService::DrainOneOtaChunk() {
    if (!m_otaTransaction.IsActive() &&
        m_otaChunkQueue.TryPeekLitchcraftFrameStore()) {
        if (IsConnected() &&
            m_bleSettings.OwnsConnectionEpoch(
                m_bleSettings.CurrentConnectionEpoch()) &&
            m_litchcraftFrameIngressCallback != nullptr) {
            m_lastClientActivityMs = fxe::Millis();
            WakeLitchcraftFrameIngress();
            return false;
        }
        const uint8_t* dropped = nullptr;
        size_t droppedSize = 0U;
        uint32_t droppedOrdinal = 0U;
        uint32_t droppedEpoch = 0U;
        if (m_otaChunkQueue.TryAcquireReadSlot(
                dropped, droppedSize, droppedOrdinal, droppedEpoch)) {
            m_otaChunkQueue.ReleaseReadSlot();
            ++m_otaChunkDrainCount;
            return true;
        }
        return false;
    }
    const uint8_t* data = nullptr;
    size_t len = 0U;
    uint32_t ingressOrdinal = 0U;
    uint32_t sessionEpoch = 0U;
    if (!m_otaChunkQueue.TryAcquireReadSlot(
            data, len, ingressOrdinal, sessionEpoch)) {
        return false;
    }
    (void)ingressOrdinal;
    const bool litchcraftBulk = !m_otaTransaction.IsActive() && len >= 3U &&
        ((data[0] == LitchcraftSession::MAGIC &&
          data[1] == LitchcraftSession::BULK_REPLACE) ||
         (data[0] == 0x4dU && data[1] == 2U &&
          (data[2] == 15U || data[2] == 16U || data[2] == 17U)));
    if (litchcraftBulk) {
        // Queue epochs retire accepted data immediately in the callback;
        // BLE ownership separately requires the latest connection to be active.
        if (sessionEpoch == m_otaChunkQueue.CurrentEpoch() &&
            m_bleSettings.OwnsConnectionEpoch(m_bleSettings.CurrentConnectionEpoch()) &&
            IsConnected() && m_litchcraftGame != nullptr) {
            m_lastClientActivityMs = fxe::Millis();
            m_litchcraftGame->HandleLitchcraftBulkPacket(
                data, len, m_litchcraftEligible);
        }
    } else {
        HandleOtaChunkPacket(data, len, sessionEpoch);
    }
    m_otaChunkQueue.ReleaseReadSlot();
    ++m_otaChunkDrainCount;
    return true;
}
#endif

} // namespace modules
