#include "firmware/settings_persistence_worker.hpp"

#ifdef ARDUINO

#include "runtime/progress_watchdog.hpp"

namespace firmware {

bool SettingsPersistenceWorker::RequestStorageReformat(
    const JsonDocument& safeSnapshot) {
    if (!IsStarted() || m_source == nullptr || m_task == nullptr ||
        m_mutex == nullptr ||
        safeSnapshot.overflowed() || !safeSnapshot.is<JsonObjectConst>() ||
        measureJson(safeSnapshot) >= SNAPSHOT_BUFFER_CAPACITY ||
        xSemaphoreTake(m_mutex, 0U) != pdTRUE) return false;
    bool idle = GetStorageReformatState() == StorageReformatState::Idle &&
        m_receiptGeneration.load(std::memory_order_acquire) == 0U;
    for (const auto state : m_bufferState) {
        idle = idle && state == BufferState::Free;
    }
#if defined(LITCH_ESP32S3)
    idle = idle && m_litchcraftArtworkState == BufferState::Free &&
        !m_litchcraftFrameStorageActive && !m_litchcraftFrameIngressPending &&
        !m_litchcraftFramePlaybackPending && !m_litchcraftFramePlaybackResident &&
        !m_litchcraftFrameCommitPending &&
        !m_litchcraftFrameModeSavePending &&
        m_litchcraftFrameClearState.load(std::memory_order_acquire) ==
            FrameClearState::Idle &&
        m_storageReadState.load(std::memory_order_acquire) != 1U &&
        m_storageReadState.load(std::memory_order_acquire) != 2U;
#endif
    if (!idle) {
        xSemaphoreGive(m_mutex);
        return false;
    }
    const size_t written = serializeJson(
        safeSnapshot, SettingsBuffer(0U), SNAPSHOT_BUFFER_CAPACITY);
    if (written == 0U || written >= SNAPSHOT_BUFFER_CAPACITY) {
        xSemaphoreGive(m_mutex);
        return false;
    }
    m_storageReformatBufferIndex = 0U;
    // Reserve the existing buffer without exposing it as an ordinary save.
    m_bufferState[0U] = BufferState::Writing;
    m_storageReformatState.store(StorageReformatState::Pending,
                                 std::memory_order_release);
    xSemaphoreGive(m_mutex);
    xTaskNotifyGive(m_task);
    return true;
}

bool SettingsPersistenceWorker::ReleaseStorageReformatResult() {
    auto state = GetStorageReformatState();
    return (state == StorageReformatState::Succeeded ||
            state == StorageReformatState::Failed) &&
        m_storageReformatState.compare_exchange_strong(
            state, StorageReformatState::Idle, std::memory_order_acq_rel);
}

bool SettingsPersistenceWorker::ServiceStorageReformat() {
    auto pending = StorageReformatState::Pending;
    if (!m_storageReformatState.compare_exchange_strong(
            pending, StorageReformatState::Running,
            std::memory_order_acq_rel)) return false;
    runtime::WatchCurrentTask(2U, runtime::ProgressStage::Storage);
    auto& document = m_writer.Doc();
    document.clear();
    const auto parsed = deserializeJson(document, static_cast<const char*>(
        SettingsBuffer(m_storageReformatBufferIndex)));
    const bool saved = !parsed && document.is<JsonObject>() &&
        m_writer.ReformatFilesystemAndSaveNowBlocking();
    if (!saved) {
        PublishFailure(parsed ? fxe::SettingsStoreError::SerializeFailed
                              : m_writer.GetLastError());
        m_source->ReportAsyncSaveFailure(GetLastError(),
            !parsed && m_writer.HasConfirmedStorageFailure()
                ? fxe::SettingsStore::StorageFailureEvidence::Confirmed
                : fxe::SettingsStore::StorageFailureEvidence::Unconfirmed);
    } else {
        // A safe reset snapshot is durable, not the currently playing RAM game.
        // Do not publish normal-save success or re-enable the live source.
        m_lastError.store(static_cast<uint8_t>(fxe::SettingsStoreError::None),
                          std::memory_order_release);
    }
    ReleaseBuffer(m_storageReformatBufferIndex);
    m_storageReformatState.store(saved ? StorageReformatState::Succeeded
                                       : StorageReformatState::Failed,
                                 std::memory_order_release);
    runtime::UnwatchCurrentTask(2U);
    return true;
}

void SettingsPersistenceWorker::CancelDisabledPending() {
    // Called with the worker mutex held. An already-running transaction keeps
    // its result callback and terminal receipt; only queued snapshots cancel.
    if (m_source == nullptr || !m_source->IsDisabled()) return;
#if defined(LITCH_ESP32S3)
    if (m_litchcraftArtworkState == BufferState::Pending) {
        const uint32_t generation = m_litchcraftArtworkGeneration;
        m_litchcraftArtworkState = BufferState::Free;
        m_litchcraftArtworkBytes = nullptr;
        m_litchcraftArtworkSize = 0U;
        m_litchcraftArtworkDocument = nullptr;
        m_litchcraftArtworkEnvelope = nullptr;
        if (m_litchcraftArtworkOwnsDocument) {
            m_pendingStorage.artwork.~ArtworkDocument();
            new (&m_pendingStorage.settings) SettingsBuffers{};
            m_litchcraftArtworkOwnsDocument = false;
        }
        m_lastCompletedGeneration.store(generation, std::memory_order_release);
        if (m_receiptGeneration.load(std::memory_order_acquire) == generation) {
            m_receiptOutcome.store(SaveOutcome::Failed, std::memory_order_release);
        }
    }
    uint8_t benchmark = 1U;
    (void)m_storageReadState.compare_exchange_strong(
        benchmark, 4U, std::memory_order_acq_rel);
#endif
    for (uint8_t index = 0U; index < BUFFER_COUNT; ++index) {
        if (m_bufferState[index] != BufferState::Pending) continue;
        const uint32_t generation = m_bufferGeneration[index];
        m_bufferState[index] = BufferState::Free;
        const uint32_t completed =
            m_lastCompletedGeneration.load(std::memory_order_acquire);
        if (static_cast<int32_t>(generation - completed) > 0) {
            m_lastCompletedGeneration.store(generation,
                                            std::memory_order_release);
        }
        if (m_receiptGeneration.load(std::memory_order_acquire) == generation) {
            m_receiptOutcome.store(SaveOutcome::Failed,
                                    std::memory_order_release);
        }
    }
}

#if defined(LITCH_ESP32S3)
bool SettingsPersistenceWorker::ServiceDisabledLitchcraftFramePlayback() {
    m_litchcraftFrameStream.CompleteInitialRecovery();
    using Stream = modules::litchcraft_frame_stream::FrameStream;
    if (m_litchcraftFrameModeSavePending.exchange(false, std::memory_order_acq_rel)) {
        m_litchcraftFrameModeSaveFailed.store(true, std::memory_order_release);
    }
    m_litchcraftFrameNextSaveRetryMs = 0U;
    // Keep edits and the displayed frame in RAM. Only an already durable,
    // playing resident may continue read-only paging after storage degradation.
    if (!m_litchcraftFramePlaybackResident.load(std::memory_order_acquire) ||
        m_litchcraftFrameStream.HasPendingBlankDocument()) {
        m_litchcraftFramePlaybackPending.store(false, std::memory_order_release);
        if (m_litchcraftFrameStorageActive) {
            m_pendingStorage.frames.receiver.EndOwner(m_litchcraftFrameActiveOwnerEpoch);
            ReleaseLitchcraftFrameStorage();
        }
        return false;
    }
    if (!m_litchcraftFrameRuntimeReady.load(std::memory_order_acquire) ||
        !m_litchcraftFramePlaybackPending.load(std::memory_order_acquire) ||
        (!m_litchcraftFrameStorageActive && !StartLitchcraftFrameStorage())) return false;
    auto& storage = m_pendingStorage.frames;
    if (storage.receiver.CommitPending()) return false;
    const auto result = m_litchcraftFrameStream.Service(
        storage.store, false, Stream::ServiceMode::PagingOnly,
        m_litchcraftFramePlaybackSuspended.load(std::memory_order_acquire)
            ? Stream::PrefetchPolicy::None
            : m_litchcraftFramePlaybackResident.load(std::memory_order_acquire)
            ? Stream::PrefetchPolicy::TwoAhead : Stream::PrefetchPolicy::Next);
    const bool more = result == Stream::ServiceResult::Loading ||
        result == Stream::ServiceResult::LoadedRequested ||
        result == Stream::ServiceResult::PrefetchedNext;
    m_litchcraftFramePlaybackPending.store(more, std::memory_order_release);
    return more;
}
#endif

} // namespace firmware

#endif
