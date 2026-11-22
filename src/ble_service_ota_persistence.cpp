#include "modules/ble_service.hpp"

#if defined(LITCH_BLE) && defined(LITCH_BLE_OTA)
#include <cstring>

#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/game_persistence_coordinator.hpp"

namespace modules {
namespace {
// Give the same storage owners the ordinary sleep/reboot save window.
constexpr uint32_t OTA_PERSISTENCE_TIMEOUT_MS = 10000U;
constexpr uint32_t OTA_PERSISTENCE_PREPARE_RETRY_MS = 100U;
}
void BleService::UpdateOtaInactivityTimeout() {
    if (!m_otaTransaction.IsActive() || m_pendingOtaEnd.len != 0U) return;
    const uint32_t epoch = m_otaTransaction.SessionEpoch();
    if (m_otaTransaction.CheckTimeout(fxe::Millis(), GetOtaTransactionOps()) == BleOtaTransactionResult::TimedOut) {
        logger::Error("ble: OTA inactivity timeout; aborting");
        LogOtaWriteMetrics("timeout");
        AbortOtaTransfer(true, true, epoch);
        NotifyOtaStatus("error", "timeout");
    }
}

bool BleService::DeferOtaEndForPersistence(const char* data, const size_t len,
                                         const uint32_t sessionEpoch,
                                         const uint32_t connectionEpoch) {
    if (m_otaSimulationMode || m_otaEndPersistenceReady || m_litchcraftGame == nullptr ||
        !m_litchcraftGame->ArtPersistence().IsConfigured()) return false;
    if (m_pendingOtaEnd.len != 0U) {
        const bool same = m_pendingOtaEnd.len == len &&
            m_pendingOtaEnd.sessionEpoch == sessionEpoch &&
            m_pendingOtaEnd.connectionEpoch == connectionEpoch &&
            std::memcmp(m_pendingOtaEnd.input, data, len) == 0;
        if (!same) NotifyOtaStatus("error", "busy_ota");
        return true;
    }
    if (len >= sizeof(m_pendingOtaEnd.input)) {
        AbortOtaTransfer(true, true, sessionEpoch);
        NotifyOtaStatus("error", "persist_failed");
        return true;
    }
    std::memcpy(m_pendingOtaEnd.input, data, len);
    m_pendingOtaEnd.input[len] = '\0';
    m_pendingOtaEnd.len = static_cast<uint8_t>(len);
    m_pendingOtaEnd.sessionEpoch = sessionEpoch;
    m_pendingOtaEnd.connectionEpoch = connectionEpoch;
    m_pendingOtaEndAtMs = fxe::Millis();
    m_nextOtaPersistencePrepareAtMs = m_pendingOtaEndAtMs;
    m_otaEndFlushRequested = false;
    if (m_otaStoreSavePending != nullptr) {
        // Freeze Store edits before asking its worker to make the latest frame
        // durable. Abort/reset releases this pause if the update cannot finish.
        m_litchcraftGame->Litchcraft().SetMutationsPaused(true);
        if (m_otaStoreSaveRequest != nullptr) {
            m_otaStoreSaveRequest(m_otaStorePersistenceContext);
        }
    }
    return true;
}

void BleService::PollOtaEndPersistence() {
    if (m_pendingOtaEnd.len == 0U) return;
    const uint32_t epoch = m_pendingOtaEnd.sessionEpoch;
    if (!IsConnected() || !m_bleSettings.OwnsConnectionEpoch(m_pendingOtaEnd.connectionEpoch) ||
        !m_otaTransaction.IsActive() || m_otaTransaction.SessionEpoch() != epoch ||
        m_otaChunkQueue.CurrentEpoch() != epoch) {
        AbortOtaTransfer(true, false, epoch);
        return;
    }
    auto& persistence = m_litchcraftGame->ArtPersistence();
    if (m_otaEndFlushRequested ||
        persistence.FlushState() == LitchcraftPersistence::FlushStatus::Pending) persistence.Update();
    const uint32_t nowMs = fxe::Millis();
    using FlushStatus = LitchcraftPersistence::FlushStatus;
    const auto flush = persistence.FlushState();
    if (m_otaEndFlushRequested && flush == FlushStatus::Failed) {
        // An accepted settings receipt still belongs to its persistence owner;
        // aborting the image never fabricates cancellation of that write.
        logger::Warn("BLE OTA finalization aborted: persist_failed");
        AbortOtaTransfer(true, true, epoch);
        NotifyOtaStatus("error", "persist_failed");
        return;
    }

    // A retained explicit flush belongs to its original caller. OTA needs
    // its own latest whole-document snapshot before the image can activate.
    const bool needsSnapshot = !m_otaEndFlushRequested ||
        (flush == FlushStatus::Saved && persistence.IsDirty());
    if (!needsSnapshot && flush == FlushStatus::Saved &&
        (m_otaStoreSavePending == nullptr || !m_otaStoreSavePending(m_otaStorePersistenceContext)) &&
        CanRetainConnectionOtaStatus()) {
        // A queued abort, replacement begin, or extra image data must run
        // first, including when this drain exhausted its time slice.
        BleEvent queued;
        uint32_t ordinal = 0U;
        if (!m_eventQueue.TryPeek(queued) && !m_otaChunkQueue.TryPeekOrdinal(ordinal)) {
            const auto end = m_pendingOtaEnd;
            m_pendingOtaEnd.len = 0U;
            m_otaEndPersistenceReady = true;
            HandleOtaControlPacket(end.input, end.len, end.sessionEpoch, end.connectionEpoch);
            m_otaEndPersistenceReady = false;
            return;
        }
    }

    const uint32_t elapsedMs = nowMs - m_pendingOtaEndAtMs;
    if (elapsedMs >= OTA_PERSISTENCE_TIMEOUT_MS) {
        const char* detail = m_otaEndFlushRequested
            ? "persist_timeout"
            : "persist_not_ready";
        logger::Warn("BLE OTA finalization aborted: %s after %lu ms", detail,
                     static_cast<unsigned long>(elapsedMs));
        AbortOtaTransfer(true, true, epoch);
        NotifyOtaStatus("error", detail);
        return;
    }
    if (!needsSnapshot || flush == FlushStatus::Pending ||
        static_cast<int32_t>(nowMs - m_nextOtaPersistencePrepareAtMs) < 0) return;
    if (!m_litchcraftGame->PrepareSettingsSnapshotForLifecycle()) {
        // Promotion, undo and Duck relocation can temporarily block a snapshot.
        m_nextOtaPersistencePrepareAtMs = nowMs + OTA_PERSISTENCE_PREPARE_RETRY_MS;
        return;
    }
    persistence.RequestFlush();
    m_otaEndFlushRequested = true;
    persistence.Update();
}
} // namespace modules
#endif
