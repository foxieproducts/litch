#pragma once

#include "firmware/settings_physical_access.hpp"
#include "modules/support_runtime_stats.hpp"

#ifdef ARDUINO

#include <array>
#include <atomic>
#include <cstdint>
#include <new>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "firmware/settings_factory_task_storage.hpp"
#include "foxie_embedded/settings_store.hpp"
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
#include "foxie_embedded/millis.hpp"
#endif
#if defined(LITCH_ESP32S3)
#include "firmware/litchcraft_artwork_storage.hpp"
#include "firmware/litchcraft_frame_file.hpp"
#include "modules/ble_ota_chunk_queue.hpp"
#include "modules/litchcraft_frame_store_receiver.hpp"
#include "modules/litchcraft_frame_stream.hpp"
#include "modules/litchcraft_persistence.hpp"
#endif

namespace firmware {

class SettingsPersistenceWorker final {
  public:
    void SetSupportRuntimeStats(modules::SupportRuntimeStats* stats) {
        if (!IsStarted()) m_supportRuntimeStats = stats;
    }
    using SaveReceipt = uint32_t;
    // One prerequisite belongs to the retained tracked request. Runs once on
    // settings_io before any snapshot write; failure prevents that write.
    using TrackedPrerequisite = bool (*)(void*);
    using DurableSnapshotCallback = void (*)(void*, const JsonDocument&);
    using PrepareSnapshotCallback = bool (*)(void*, JsonDocument&);
    enum class SnapshotTransactionOutcome : uint8_t { Saved, Failed };
    using SnapshotTransactionResultCallback =
        void (*)(void*, SnapshotTransactionOutcome, const JsonDocument&);
    using CurrentGameSyncServiceCallback = void (*)(void*);
    // Install the sole non-null observer before Begin. The first successful
    // assignment is retained, including across a failed Begin; replacement and
    // removal are rejected. Invoked on the worker after settings promotion.
    // Auxiliary history failure must not turn a saved current game into failure.
    bool SetDurableSnapshotCallback(DurableSnapshotCallback callback, void* context);
    // Install the sole log-before-settings transaction before Begin. Prepare is
    // invoked once per accepted snapshot on the settings worker, even when the
    // settings write retries. Result is invoked once after terminal success or
    // failure with that same prepared document. A consumer refusal fails only
    // its request and retries latest RAM; it cannot disable shared saving.
    bool SetSnapshotTransactionCallbacks(
        PrepareSnapshotCallback prepare,
        SnapshotTransactionResultCallback result,
        void* context);
    // The current-game transfer shares settings_io because its source reads
    // the same flash files. BLE only wakes this owner; it never performs I/O.
    bool SetCurrentGameSyncServiceCallback(
        CurrentGameSyncServiceCallback callback, void* context);
    void NotifyCurrentGameSync();
    // Borrow the just-deserialized input buffer during the synchronous
    // log-before-settings callback. Decoded strings are owned by the writer's
    // JSON pool, so the complete input buffer is available for scratch.
    void* ActiveSnapshotTransactionScratch(size_t bytes, size_t alignment);
    enum class SaveOutcome : uint8_t { Unknown = 0U, Pending, Saved, Failed };
    struct PersistenceTiming {
        bool available = false;
        bool succeeded = false;
        uint32_t sequence = 0U;
        uint32_t generation = 0U;
        uint32_t elapsedMs = 0U;
        uint32_t fileCalls = 0U;
        uint32_t fileMs = 0U;
        uint32_t pagingCalls = 0U;
        uint32_t pagingMs = 0U;
        SettingsTransactionFlashSample flash{};
    };
    struct Diagnostics {
        uint32_t requested = 0U;
        uint32_t completed = 0U;
        uint32_t receipt = 0U;
        SaveOutcome outcome = SaveOutcome::Unknown;
        uint32_t settingsMs = 0U;
        uint32_t artworkMs = 0U;
        uint32_t flashAccesses = 0U;
        uint32_t stackFreeBytes = 0U;
        uint8_t storageReadState = 0U;
        uint32_t storageReadBytes = 0U;
        uint32_t storageReadWorkerMs = 0U;
        uint32_t storageReadFileMs = 0U;
        uint32_t storageReadHash = 0U;
        uint32_t storageReadCalls = 0U;
        uint32_t frameStoreTransferMs = 0U;
        uint32_t frameStoreBodyWriteMs = 0U;
        uint32_t frameStoreBodyPrepareMs = 0U;
        uint32_t frameStoreRootPublishMs = 0U;
        uint32_t frameStoreBodyBytes = 0U;
        uint32_t frameStoreIngressPackets = 0U;
        uint8_t frameStoreRecovery = 0U;
        uint32_t frameStoreStartupReadyMs = UINT32_MAX;
        uint32_t frameStoreStartupBeginMs = UINT32_MAX;
        uint32_t frameStoreStartupModeResolvedMs = UINT32_MAX;
        bool frameStoreRuntimeReady = false;
        bool frameStoreStorageActive = false;
        bool frameStorePlaybackPending = false;
        bool frameStoreModeSavePending = false;
        bool frameStoreModeSaveFailed = false;
        // One coherent pair for the most recently executing settings request.
        // A busy mutex yields unavailable, never a torn record or loop wait.
        bool settingsOperationsAvailable = false;
        uint32_t settingsOperationWorkerGeneration = 0U;
        fxe::SettingsSaveOperation settingsLastOperation{};
        fxe::SettingsSaveOperation settingsWorstOperation{};
        SettingsPhysicalAccessSample settingsLastPhysicalAccess{};
        SettingsPhysicalAccessSample settingsWorstPhysicalAccess{};
        // Last completed settings request, copied under the same mutex.
        PersistenceTiming settingsTiming{};
    };
    Diagnostics GetDiagnostics() const {
        Diagnostics result{m_lastRequestedGeneration.load(), m_lastCompletedGeneration.load(),
                m_receiptGeneration.load(), m_receiptOutcome.load(),
                m_lastSettingsMs.load(), m_lastArtworkMs.load(), m_flashAccesses.load()};
#if defined(LITCH_ESP32S3)

        result.frameStoreTransferMs = m_frameStoreTransferMs.load();
        result.frameStoreBodyWriteMs = m_frameStoreBodyWriteMs.load();
        result.frameStoreBodyPrepareMs = m_frameStoreBodyPrepareMs.load();
        result.frameStoreRootPublishMs = m_frameStoreRootPublishMs.load();
        result.frameStoreBodyBytes = m_frameStoreBodyBytes.load();
        result.frameStoreIngressPackets = m_frameStoreIngressPackets.load();
        result.frameStoreRecovery = m_litchcraftFrameLastRecovery.load();
        result.frameStoreStartupReadyMs = m_litchcraftStartupReadyMs.load();
        result.frameStoreStartupBeginMs = m_litchcraftStartupBeginMs.load();
        result.frameStoreStartupModeResolvedMs = m_litchcraftStartupModeResolvedMs.load();
        result.frameStoreRuntimeReady = m_litchcraftFrameRuntimeReady.load();
        result.frameStoreStorageActive = m_litchcraftFrameStorageActive.load();
        result.frameStorePlaybackPending = m_litchcraftFramePlaybackPending.load();
        result.frameStoreModeSavePending = m_litchcraftFrameModeSavePending.load();
        result.frameStoreModeSaveFailed = m_litchcraftFrameModeSaveFailed.load();
#endif
#ifndef LITCH_NATIVE
        if (m_task != nullptr) result.stackFreeBytes = uxTaskGetStackHighWaterMark(m_task);
#endif
        if (m_mutex != nullptr && xSemaphoreTake(m_mutex, 0U) == pdTRUE) {
            result.settingsOperationsAvailable = true;
            result.settingsOperationWorkerGeneration = m_settingsOperationWorkerGeneration;
            result.settingsLastOperation = m_settingsLastOperation;
            result.settingsWorstOperation = m_settingsWorstOperation;
            result.settingsLastPhysicalAccess = m_settingsLastPhysicalAccess;
            result.settingsWorstPhysicalAccess = m_settingsWorstPhysicalAccess;
            result.settingsTiming = m_settingsTiming;
#if defined(LITCH_ESP32S3)
            result.storageReadState = m_storageReadState.load();
            if (result.storageReadState == 3U || result.storageReadState == 4U ||
                result.storageReadState == 7U || result.storageReadState == 8U ||
                result.storageReadState == 9U || result.storageReadState == 10U) {
                result.storageReadBytes = m_storageReadBytes.load();
                result.storageReadWorkerMs = m_storageReadWorkerMs.load();
                result.storageReadFileMs = m_storageReadFileMs.load();
                result.storageReadHash = m_storageReadHash.load();
                result.storageReadCalls = m_storageReadCalls.load();
            }
#endif
            xSemaphoreGive(m_mutex);
        }
        return result;
    }

#if defined(LITCH_ESP32S3) && (defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE))
    struct IngressPerformanceDiagnostics {
        struct SlotTiming { uint32_t calls = 0U, totalMs = 0U, maximumMs = 0U; };
        uint32_t ownerEpoch = 0U, request = 0U, skipped = 0U;
        // Chunk/ContinueBody, then Commit (including a synchronous retry).
        std::array<SettingsFlashTiming, 2U> flash{};
        std::array<uint32_t, 2U> calls{};
        // Slot zero, later sixteen-slot part boundaries, all other slots.
        std::array<SlotTiming, 3U> slots{};
        bool physicalAvailable = false, available = false;
    };
    static_assert(sizeof(IngressPerformanceDiagnostics) <= 192U);
    IngressPerformanceDiagnostics GetIngressPerformanceDiagnostics() const {
        IngressPerformanceDiagnostics result;
        if (m_mutex != nullptr && xSemaphoreTake(m_mutex, 0U) == pdTRUE) {
            result = m_ingressPerformance;
            result.available = true;
            xSemaphoreGive(m_mutex);
        }
        return result;
    }
    struct LiveIngressDiagnostics {
        uint32_t ownerEpoch = 0U;
        size_t responseSize = 0U;
        uint8_t prefetchCount = 0U;
        bool ingressPending = false, modeSavePending = false, storageActive = false;
        bool available = false;
        uint32_t handleAgeMs = 0U;
        uint8_t handleOperation = 0U, handlePhase = 0U;
    };
    LiveIngressDiagnostics GetLiveIngressDiagnostics() const {
        LiveIngressDiagnostics result;
        // One coherent atomic word: low 5 bits opcode, next 2 phase, high
        // 24 low-millisecond bits. Ages wrap after 4.66 h; a Handle is bounded
        // to seconds in this temporary probe. No receiver cross-thread reads.
        const uint32_t handle = m_liveIngressHandle.load(std::memory_order_acquire);
        result.handlePhase = static_cast<uint8_t>((handle >> 5U) & 3U);
        if (handle != 0U) {
            result.handleOperation = static_cast<uint8_t>(handle & 31U);
            result.handleAgeMs = (fxe::Millis() - (handle >> 8U)) & 0x00ffffffU;
        }
        if (m_mutex != nullptr && xSemaphoreTake(m_mutex, 0U) == pdTRUE) {
            result.ownerEpoch = m_litchcraftFrameOwnerEpoch;
            result.responseSize = m_litchcraftFrameResponseSize.load();
            result.prefetchCount = m_litchcraftFramePrefetchCount;
            result.ingressPending = m_litchcraftFrameIngressPending.load();
            result.modeSavePending = m_litchcraftFrameModeSavePending.load();
            result.storageActive = m_litchcraftFrameStorageActive.load();
            result.available = true;
            xSemaphoreGive(m_mutex);
        }
        return result;
    }
#endif

    explicit SettingsPersistenceWorker(const fxe::SettingsStoreConfig& config)
        : m_writer(config) {}

    bool BindFlashArbiter(hardware::RmtFlashArbiter& arbiter) {
        if (IsStarted()) return m_settingsPhysicalAccess.HasFlashArbiter(arbiter);
        m_settingsPhysicalAccess.BindFlashArbiter(arbiter);
        return true;
    }
    SettingsFactoryTaskStorage& FactoryTaskStorage() { return m_taskStorage; }
    bool Begin(fxe::SettingsStore& source);
    bool Enqueue(const JsonDocument& document);
#if defined(LITCH_NATIVE)
    void SetStorageAccessTestHook(void (*hook)(void*, bool), void* context) {
        m_storageAccessTestHook = hook;
        m_storageAccessTestContext = context;
    }
    void SetSettingsStorageAccessTestHook(void (*hook)(void*), void* context) {
        m_settingsStorageAccessTestHook = hook;
        m_settingsStorageAccessTestContext = context;
    }
#endif
    enum class StorageReformatState : uint8_t {
        Idle, Pending, Running, Succeeded, Failed,
    };
    // Explicitly confirmed recovery only. Copies a bounded safe document into
    // existing worker scratch and refuses competing storage owners. No live
    // source values are replaced or re-enabled; success requires a reboot.
    bool RequestStorageReformat(const JsonDocument& safeSnapshot);
    StorageReformatState GetStorageReformatState() const {
        return m_storageReformatState.load(std::memory_order_acquire);
    }
    bool ReleaseStorageReformatResult();
    // One retained, exact-generation receipt. While retained, other requests
    // are not admitted; callers retain their dirty state and retry normally.
    // Acceptance is irrevocable here, even before the worker starts writing.
    bool EnqueueTracked(
        const JsonDocument& document, SaveReceipt& outReceipt,
        fxe::SettingsStore::AsyncSaveAdmission* admission = nullptr,
        std::atomic<SaveReceipt>* publishedReceipt = nullptr,
        TrackedPrerequisite prerequisite = nullptr, void* prerequisiteContext = nullptr,
        const char* omittedSetting = nullptr, const char* replacement = nullptr);
    // The exact retained receipt for the current durable callback, or zero for
    // an ordinary untracked snapshot. Valid only within that callback.
    SaveReceipt CurrentDurableSnapshotReceipt() const {
        return m_callbackReceipt.load(std::memory_order_acquire);
    }
#if defined(LITCH_ESP32S3)
    bool SetLitchcraftArtworkStorageOps(LitchcraftArtworkStorage::Ops ops);
    bool SetLitchcraftArtworkCurrentKnowledge(
        LitchcraftArtworkStorage::CurrentKnowledge knowledge);
    // Synchronous boot/recovery read. Runtime completion uses the verified
    // SaveOutcome receipt, never this method on the input/render caller.
    LitchcraftArtworkStorage::Status ReadLitchcraftArtwork(
        LitchcraftArtworkStorage::Artifact artifact, uint8_t* out,
        size_t capacity, size_t& outSize);
    // Caller freezes bytes until the exact receipt is released. Saved means
    // both Pending and promoted Current passed byte-for-byte worker readback.
    bool EnqueueLitchcraftArtworkTracked(
        const uint8_t* bytes, size_t size, SaveReceipt& outReceipt,
        fxe::SettingsStore::AsyncSaveAdmission* admission = nullptr);
    bool EnqueueLitchcraftArtworkDocumentTracked(
        const modules::litchcraft_animation::Document& document, bool active,
        modules::LitchcraftPersistence::AdvancedEnvelope& envelope,
        SaveReceipt& outReceipt,
        fxe::SettingsStore::AsyncSaveAdmission* admission = nullptr);
    bool RequestSettingsReadDiagnostic();
    enum class FrameClearState : uint8_t {
        Idle, Pending, Running, Succeeded, Failed, PrelaunchLC5Pending
    };
    bool RequestLitchcraftFrameClear();
    bool LitchcraftFrameClearPending() const {
        const auto state = LitchcraftFrameClearState();
        return state == FrameClearState::Pending || state == FrameClearState::Running ||
            state == FrameClearState::PrelaunchLC5Pending;
    }
    FrameClearState TakeLitchcraftFrameClearResult() {
        auto state = LitchcraftFrameClearState();
        if ((state == FrameClearState::Succeeded || state == FrameClearState::Failed) &&
            m_litchcraftFrameClearState.compare_exchange_strong(
                state, FrameClearState::Idle, std::memory_order_acq_rel)) return state;
        return FrameClearState::Idle;
    }
    FrameClearState LitchcraftFrameClearState() const {
        return m_litchcraftFrameClearState.load(std::memory_order_acquire);
    }
    void NotifyLitchcraftFrameIngress(
        modules::BleOtaChunkQueue& queue,
        modules::litchcraft_frame_store::StoreProjection projection,
        uint32_t ownerEpoch, uint16_t packetMaximum = 512U);
    void NotifyLitchcraftFramePriority(
        const uint8_t* bytes, size_t size, uint32_t ownerEpoch);
    bool PeekLitchcraftFrameResponse(uint8_t* out, size_t capacity,
                                    size_t& outSize);
    void AcceptLitchcraftFrameResponse();
    bool IsLitchcraftBulkTransferActive();
    void SetLitchcraftPlaybackSuspended(bool suspended) {
        if (m_litchcraftFramePlaybackSuspended.exchange(suspended,
                std::memory_order_acq_rel) != suspended) {
            m_litchcraftFramePlaybackPending.store(true, std::memory_order_release);
            if (m_task != nullptr) xTaskNotifyGive(m_task);
        }
    }
    bool ReplayLitchcraftFrameResponse(uint16_t request, uint32_t ownerEpoch);
    modules::litchcraft_frame_stream::FrameStream& LitchcraftFrameStream() {
        return m_litchcraftFrameStream;
    }
    const modules::litchcraft_frame_stream::FrameStream&
    LitchcraftFrameStream() const {
        return m_litchcraftFrameStream;
    }
    void NotifyLitchcraftFrameRuntimeReady();
    void SetLitchcraftFrameSummaryMaintenanceReady(bool ready);
    void NotifyLitchcraftFramePlayback(bool playing = false);
    void NotifyLitchcraftFrameSession(bool playing, uint32_t session,
                                     bool active);
    bool LitchcraftFrameModeSavePending() const {
        return m_litchcraftFrameModeSavePending.load(
            std::memory_order_acquire);
    }
    bool LitchcraftFrameCommitPending() const {
        return m_litchcraftFrameCommitPending.load(
            std::memory_order_acquire);
    }
#if defined(LITCH_NATIVE)
    bool BlockLitchcraftFrameRootWritesForTest(bool blocked);
    void SetLitchcraftFrameTimingTestHook(
        modules::litchcraft_frame_store::FrameStore::TimingObserver hook, void* context) {
        m_litchcraftFrameTimingTestHook = hook;
        m_litchcraftFrameTimingTestContext = context;
    }
    void SetLitchcraftFrameBeforeConfigureTestHook(void (*hook)(void*),
                                                   void* context) {
        m_litchcraftFrameBeforeConfigureTestHook = hook;
        m_litchcraftFrameBeforeConfigureTestContext = context;
    }
    void SetLitchcraftFrameBeforeSyncTestHook(void (*hook)(void*),
                                              void* context) {
        m_litchcraftFrameBeforeSyncTestHook = hook;
        m_litchcraftFrameBeforeSyncTestContext = context;
    }
#endif
#endif
    SaveOutcome GetSaveOutcome(SaveReceipt receipt) const;
    bool ReleaseSaveReceipt(SaveReceipt receipt);
    bool Flush(const JsonDocument& document, uint32_t timeoutMs);
    bool WaitUntilIdle(uint32_t timeoutMs) const;

    bool IsStarted() const {
        return m_started.load(std::memory_order_acquire);
    }
    bool IsIdle() const;
    uint32_t GetCompletedSaveCount() const {
        return m_completedSaveCount.load(std::memory_order_acquire);
    }
    uint32_t GetFailureCount() const {
        return m_failureCount.load(std::memory_order_acquire);
    }
#if defined(LITCH_ESP32S3)
    uint32_t GetLitchcraftArtworkFailureCount() const {
        return m_litchcraftArtworkFailureCount.load(std::memory_order_acquire);
    }
    uint32_t GetLitchcraftFrameSaveFailureCount() const {
        return m_litchcraftFrameSaveFailureCount.load(
            std::memory_order_acquire);
    }
#endif
    fxe::SettingsStoreError GetLastError() const {
        return static_cast<fxe::SettingsStoreError>(
            m_lastError.load(std::memory_order_acquire));
    }

  private:
    friend class GameHistoryJournalStorage;
    friend class CurrentGameLogStorage;
    friend class CurrentGameLogService;

    // Worker-owned evidence from the real log adapter/readback, never a
    // generic callback refusal. It belongs only to the current preparation.
    void NoteSnapshotStorageFailure(fxe::SettingsStoreError error) {
        if (m_snapshotTransactionActive) m_snapshotStorageError = error;
    }

    enum class BufferState : uint8_t {
        Free = 0,
        Pending,
        Writing,
    };

    static void TaskEntry(void* context);
    void Run();
    bool Enqueue(const JsonDocument& document, SaveReceipt* outReceipt,
                 fxe::SettingsStore::AsyncSaveAdmission* admission = nullptr,
                 std::atomic<SaveReceipt>* publishedReceipt = nullptr,
        TrackedPrerequisite prerequisite = nullptr, void* prerequisiteContext = nullptr,
        const char* omittedSetting = nullptr, const char* replacement = nullptr);
    bool TryTakePending(uint8_t& outIndex);
#if defined(LITCH_ESP32S3)
    bool TryTakeLitchcraftArtwork();
    void ReleaseLitchcraftArtwork();
    void RunSettingsReadDiagnostic();
    void BeginFrameReadDiagnostic(bool reset, bool singleFrame = false);
    void CompleteFrameReadDiagnostic(
        const modules::litchcraft_frame_store::StoreIngressResult& result,
        uint32_t elapsedMs);
    void RecordFrameReadAccess(LitchcraftFrameFile::AccessOperation operation,
                               size_t bytes, uint32_t elapsedMs);
    bool ServiceLitchcraftFrameIngress();
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
    IngressPerformanceDiagnostics m_ingressPerformance{};
    bool m_ingressPerformanceSampling = false;
    bool m_ingressPerformanceSlotWritten = false;
    uint32_t m_ingressPerformanceSlotMs = 0U;
    bool BeginIngressPerformanceSample(uint32_t ownerEpoch);
    void EndIngressPerformanceSample(uint32_t ownerEpoch, bool commit);
    // Phases: 0 idle, 1 Handle, 3 SDK access observation (2 unused).
    std::atomic<uint32_t> m_liveIngressHandle{0U};
    void SetLiveIngressHandlePhase(uint8_t phase) {
        const uint32_t handle = m_liveIngressHandle.load(std::memory_order_relaxed);
        if (handle != 0U) m_liveIngressHandle.store(
            (handle & ~0x60U) | (uint32_t(phase) << 5U), std::memory_order_release);
    }
#endif
    modules::litchcraft_frame_store::StoreIngressResult*
        AcquireLitchcraftFrameResponse();
    bool FillLitchcraftFrameReadResponses(
        modules::litchcraft_frame_store::StoreReceiver& receiver,
        modules::BleOtaChunkQueue& queue, uint32_t ownerEpoch,
        uint32_t projectionRevision);
    static modules::litchcraft_frame_store::PresentationAdmission AdmitFrameStoreCommit(
        void* context, uint32_t ownerEpoch, uint32_t expectedGeneration,
        const modules::litchcraft_frame_store::Manifest& manifest,
        uint32_t generation,
        const modules::litchcraft_frame_store::Frame& firstFrame);
    static bool PublishFrameStoreCommit(
        void* context, uint32_t ownerEpoch, uint32_t expectedGeneration,
        const modules::litchcraft_frame_store::Manifest& manifest,
        uint32_t generation,
        const modules::litchcraft_frame_store::Frame& firstFrame);
    static void RecordFrameStoreTiming(
        void* context, modules::litchcraft_frame_store::FrameStore::TimingPhase phase,
        uint32_t elapsedMs);
    void ResetFrameStoreTransferTiming();
    void CompleteFrameStoreTransfer(
        modules::litchcraft_frame_store::FrameStore& store);
    bool RequestLitchcraftFrameClear(FrameClearState pending);
    bool ServiceLitchcraftFrameClear();
    bool ServiceLitchcraftFramePlayback();
    bool ServiceDisabledLitchcraftFramePlayback();
    void ServiceLitchcraftFramePlaybackSlice();
    void ServiceLitchcraftFramePagingDuringSettings();
    void SynchronizeLitchcraftFramePlayback(
        modules::litchcraft_frame_store::FrameStore& store);
    bool StartLitchcraftFrameStorage();
    void NoteStartupModeResolved();
    void ReleaseLitchcraftFrameStorage();
    modules::litchcraft_frame_store::Status RestoreLitchcraftFrameResident(
        modules::litchcraft_frame_store::FrameStore& store);
#endif
    bool ServiceStorageReformat();
    void CancelDisabledPending();
    bool PrepareBuffer(uint8_t index, bool& outTransactionInvoked);
    bool SavePreparedBuffer(uint8_t index);
    void WaitForSettingsRetry();
    void PublishSuccess();
    void AcquireFlashAccessWindow();
    void ReleaseFlashAccessWindow();
    void BeginPersistenceTiming();
    void CompletePersistenceTiming(PersistenceTiming& destination,
                                   uint32_t generation, bool succeeded,
                                   uint32_t elapsedMs);
    bool DrainForDirectStorageAccess();
    bool HasNewerPending(uint32_t generation);
    void ReleaseBuffer(uint8_t index);
    void PublishFailure(
        fxe::SettingsStoreError error = fxe::SettingsStoreError::None);

    static constexpr uint8_t SHARED_SETTINGS_BUFFER_COUNT = 2U;
#if defined(LITCH_ESP32S3)
    static constexpr uint8_t BUFFER_COUNT = SHARED_SETTINGS_BUFFER_COUNT + 1U;
#else
    static constexpr uint8_t BUFFER_COUNT = SHARED_SETTINGS_BUFFER_COUNT;
#endif
    // The maximum-schema fixture includes artwork and per-player preferences;
    // the shared bound includes its terminator. Keep the larger streamed file
    // limit for recovery without reserving it in both worker buffers.
    static constexpr size_t SNAPSHOT_BUFFER_CAPACITY =
        fxe::SETTINGS_STORE_SNAPSHOT_BUFFER_CAPACITY;
    static constexpr uint8_t MAX_SAVE_ATTEMPTS = 3U;
    static constexpr uint32_t RETRY_DELAY_MS = 250U;

    TrackedPrerequisite m_trackedPrerequisite = nullptr;
    void* m_trackedPrerequisiteContext = nullptr;
    modules::SupportRuntimeStats* m_supportRuntimeStats = nullptr;
    DurableSnapshotCallback m_durableSnapshotCallback = nullptr;
    void* m_durableSnapshotContext = nullptr;
    PrepareSnapshotCallback m_prepareSnapshotCallback = nullptr;
    SnapshotTransactionResultCallback m_snapshotTransactionResultCallback =
        nullptr;
    void* m_snapshotTransactionContext = nullptr;
    bool m_snapshotTransactionActive = false;
    fxe::SettingsStoreError m_snapshotStorageError = fxe::SettingsStoreError::None;
    CurrentGameSyncServiceCallback m_currentGameSyncServiceCallback = nullptr;
    void* m_currentGameSyncServiceContext = nullptr;
    fxe::SettingsStore m_writer;
    fxe::SettingsStore* m_source = nullptr;
    using SettingsBuffers = std::array<
        std::array<char, SNAPSHOT_BUFFER_CAPACITY>, SHARED_SETTINGS_BUFFER_COUNT>;
#if defined(LITCH_ESP32S3)
    using ArtworkDocument = modules::litchcraft_animation::Document;
    void RefreshLitchcraftBulkActivity();
    struct FrameActivityPublication final {
        SettingsPersistenceWorker& owner;
        ~FrameActivityPublication() { owner.RefreshLitchcraftBulkActivity(); }
    };
    struct FrameTransferStorage final {
        explicit FrameTransferStorage(SettingsPersistenceWorker& owner)
            : store(file.StoreOps()), receiver(store) {
            receiver.SetActivityStartedCallback([](void* context) {
                static_cast<SettingsPersistenceWorker*>(context)->m_litchcraftFrameBulkActive.store(
                    true, std::memory_order_release);
            }, &owner);
            store.SetTimingObserver(
                &SettingsPersistenceWorker::RecordFrameStoreTiming, &owner);
            (void)file.SetAccessOps(
                {&owner,
                 [](void* context) {
                     static_cast<SettingsPersistenceWorker*>(context)
                         ->AcquireFlashAccessWindow();
                 },
                 [](void* context, const LitchcraftFrameFile::AccessOperation operation,
                    const size_t bytes, const uint32_t elapsedMs) {
                     auto& owner = *static_cast<SettingsPersistenceWorker*>(context);
                     owner.ReleaseFlashAccessWindow();
                     owner.RecordFrameReadAccess(operation, bytes, elapsedMs);
                 }});
        }
        LitchcraftFrameFile file{};
        modules::litchcraft_frame_store::FrameStore store;
        modules::litchcraft_frame_store::StoreReceiver receiver;
        modules::litchcraft_frame_store::Status recovery =
            modules::litchcraft_frame_store::Status::NotConfigured;
    };
    union PendingStorage {
        SettingsBuffers settings;
        ArtworkDocument artwork;
        FrameTransferStorage frames;
        PendingStorage() : settings{} {}
        ~PendingStorage() {}
    };
    alignas(std::max_align_t) PendingStorage m_pendingStorage{};
    // Frame paging/transfer owns the union for its whole session. Keep one
    // bounded settings snapshot independent so healthy preferences and safety
    // policy can persist without stopping playback or cancelling a transfer.
    alignas(std::max_align_t)
        std::array<char, SNAPSHOT_BUFFER_CAPACITY> m_residentSettingsSnapshot{};
    static_assert(sizeof(FrameTransferStorage) <= 16U * 1024U,
                  "frame transfer and playback scratch stay within the fixed 16 KiB worker budget");
    static_assert(sizeof(FrameTransferStorage) + SNAPSHOT_BUFFER_CAPACITY +
                      sizeof(modules::litchcraft_frame_stream::FrameStream) +
                      sizeof(modules::LitchcraftSession) +
                      sizeof(modules::LitchcraftPersistence) +
                      sizeof(modules::litchcraft_animation::ComposedFrame) +
                      sizeof(modules::BleOtaChunkQueue) <= 70U * 1024U,
                  "Store scratch plus an independent settings snapshot fit 70 KiB");
#else
    alignas(std::max_align_t) SettingsBuffers m_pendingSettings{};
#endif
    char* SettingsBuffer(const uint8_t index) {
#if defined(LITCH_ESP32S3)
        return index == SHARED_SETTINGS_BUFFER_COUNT
            ? m_residentSettingsSnapshot.data()
            : m_pendingStorage.settings[index].data();
#else
        return m_pendingSettings[index].data();
#endif
    }
    BufferState m_bufferState[BUFFER_COUNT] = {};
    uint32_t m_bufferGeneration[BUFFER_COUNT] = {};
    uint32_t m_nextGeneration = 0U;
    uint8_t m_activeBufferIndex = 0U;

#if defined(LITCH_ESP32S3)
    LitchcraftArtworkStorage m_litchcraftArtworkStorage{};
    BufferState m_litchcraftArtworkState = BufferState::Free;
    const uint8_t* m_litchcraftArtworkBytes = nullptr;
    size_t m_litchcraftArtworkSize = 0U;
    const modules::litchcraft_animation::Document*
        m_litchcraftArtworkDocument = nullptr;
    modules::LitchcraftPersistence::AdvancedEnvelope*
        m_litchcraftArtworkEnvelope = nullptr;
    bool m_litchcraftArtworkActive = false;
    bool m_litchcraftArtworkOwnsDocument = false;
    uint32_t m_litchcraftArtworkGeneration = 0U;
    modules::BleOtaChunkQueue* m_litchcraftFrameQueue = nullptr;
    modules::litchcraft_frame_store::StoreProjection
        m_litchcraftFrameProjection{};
    modules::litchcraft_frame_store::StoreProjection
        m_litchcraftFrameBootstrapProjection{0U, 0U, 3U, false, false};
    std::atomic<bool> m_litchcraftFrameModeSavePending{false};
    std::atomic<bool> m_litchcraftFrameModeSaveFailed{false};
    std::atomic<bool> m_litchcraftFrameCommitPending{false};
    std::atomic<FrameClearState> m_litchcraftFrameClearState{FrameClearState::Idle};
    modules::BleOtaChunkQueue* m_litchcraftFrameClearQueue = nullptr;
    uint32_t m_litchcraftFrameClearQueueEpoch = 0U;
    uint32_t m_litchcraftFrameModeRevision = 0U;
#if defined(LITCH_NATIVE)
    modules::litchcraft_frame_store::FrameStore::TimingObserver m_litchcraftFrameTimingTestHook = nullptr;
    void* m_litchcraftFrameTimingTestContext = nullptr;
    void (*m_litchcraftFrameBeforeConfigureTestHook)(void*) = nullptr;
    void* m_litchcraftFrameBeforeConfigureTestContext = nullptr;
    void (*m_litchcraftFrameBeforeSyncTestHook)(void*) = nullptr;
    void* m_litchcraftFrameBeforeSyncTestContext = nullptr;
#endif
    modules::litchcraft_frame_store::StoreProjection
        m_litchcraftFramePreviewPrevious{};
    modules::litchcraft_frame_store::StoreProjection
        m_litchcraftFramePreviewExpected{};
    // Runtime selection outlives the transient storage union. Generation zero
    // means boot/retirement has no selected resident to restore.
    struct FrameResidentIdentity {
        uint32_t generation = 0U;
        uint32_t bodyCrc32 = 0U;
        uint8_t rootSlot = 0U;
    };
    FrameResidentIdentity m_litchcraftFrameResident{};
    modules::litchcraft_frame_store::StoreReceipt m_litchcraftFrameReceipt{};
    modules::litchcraft_frame_stream::FrameStream m_litchcraftFrameStream{};
    std::array<uint8_t,
               modules::litchcraft_frame_store::STORE_RESPONSE_MAX_BYTES>
        m_litchcraftFrameResponse{};
    static constexpr uint8_t FRAME_RESPONSE_PREFETCH_CAPACITY = 3U;
    // The sole worker encodes into an uncounted tail slot during I/O. BLE
    // reads only counted slots under m_mutex; replay writes the separate
    // current/last buffers. No packet-sized scratch is needed on task stack.
    std::array<modules::litchcraft_frame_store::StoreIngressResult,
               FRAME_RESPONSE_PREFETCH_CAPACITY> m_litchcraftFramePrefetch{};
    std::array<uint8_t,
               modules::litchcraft_frame_store::STORE_RESPONSE_MAX_BYTES>
        m_litchcraftFrameLastResponse{};
    uint8_t m_litchcraftFramePrefetchHead = 0U;
    uint8_t m_litchcraftFramePrefetchCount = 0U;
    std::array<uint8_t, 9U> m_litchcraftFramePriority{};
    std::atomic<size_t> m_litchcraftFrameResponseSize{0U};
    uint16_t m_litchcraftFrameLastResponseSize = 0U;
    size_t m_litchcraftFramePrioritySize = 0U;
    uint32_t m_litchcraftFrameOwnerEpoch = 0U;
    uint16_t m_litchcraftFramePacketMaximum = 512U;
    uint32_t m_litchcraftFrameActiveOwnerEpoch = 0U;
    uint32_t m_litchcraftFrameDiscardSession = 0U;
    uint32_t m_litchcraftFrameDiscardOwnerEpoch = 0U;
    uint16_t m_litchcraftFrameDiscardTransfer = 0U;
    // Worker-only identity of a FIFO head waiting for physical Show.
    uint32_t m_litchcraftFrameDeferredOrdinal = 0U;
    uint32_t m_litchcraftFrameDeferredOwnerEpoch = 0U;
    uint32_t m_litchcraftFrameDeferredModeRevision = 0U;
    uint32_t m_litchcraftFrameLastActivityMs = 0U;
    uint32_t m_litchcraftFrameTransferStartedMs = 0U;
    uint32_t m_litchcraftFrameIngressPackets = 0U;
    bool m_litchcraftFrameTransferTimingActive = false;
    uint32_t m_litchcraftFrameResponseOwnerEpoch = 0U;
    uint32_t m_litchcraftFrameResponseProjectionRevision = 0U;
    std::atomic<bool> m_litchcraftFrameIngressPending{false};
    std::atomic<bool> m_litchcraftFramePlaybackPending{false};
    std::atomic<uint32_t> m_litchcraftFrameSaveFailureCount{0U};
    std::atomic<bool> m_litchcraftFramePlaybackResident{false};
    std::atomic<bool> m_litchcraftFrameStorageActive{false};
    std::atomic<bool> m_litchcraftFrameBulkActive{false};
    std::atomic<bool> m_litchcraftFramePlaybackSuspended{false};
    bool m_litchcraftFramePreviewTransitionPending = false;
    uint32_t m_litchcraftFrameNextCommitRetryMs = 0U;
    uint32_t m_litchcraftFrameCommitRetryDelayMs = 100U;
    uint32_t m_litchcraftFrameNextSaveRetryMs = 0U;
    uint32_t m_litchcraftFrameSaveRetryDelayMs = 100U;
    std::atomic<bool> m_litchcraftFrameRuntimeReady{false};
    // First boot-only boundaries; later Store residency and optional summaries
    // never overwrite the essential saved-mode recovery observations.
    std::atomic<uint32_t> m_litchcraftStartupReadyMs{UINT32_MAX};
    std::atomic<uint32_t> m_litchcraftStartupBeginMs{UINT32_MAX};
    std::atomic<uint32_t> m_litchcraftStartupModeResolvedMs{UINT32_MAX};
    std::atomic<bool> m_litchcraftFrameSummaryMaintenanceReady{true};
    bool m_litchcraftFrameSummaryMaintenancePending = false;
    std::atomic<uint8_t> m_litchcraftFrameLastRecovery{
        static_cast<uint8_t>(modules::litchcraft_frame_store::Status::NotConfigured)};
    std::atomic<uint32_t> m_litchcraftFrameProjectionRevision{0U};
#endif

    StaticSemaphore_t m_mutexStorage{};
    SemaphoreHandle_t m_mutex = nullptr;
    SettingsFactoryTaskStorage m_taskStorage{};
    TaskHandle_t m_task = nullptr;

    std::atomic<StorageReformatState> m_storageReformatState{
        StorageReformatState::Idle};
    uint8_t m_storageReformatBufferIndex = 0U;
    std::atomic<bool> m_started{false};
    std::atomic<uint32_t> m_lastRequestedGeneration{0U};
    std::atomic<uint32_t> m_lastSavedGeneration{0U};
    std::atomic<uint32_t> m_lastCompletedGeneration{0U};
    std::atomic<SaveReceipt> m_receiptGeneration{0U};
    std::atomic<SaveReceipt> m_callbackReceipt{0U};
    std::atomic<SaveOutcome> m_receiptOutcome{SaveOutcome::Unknown};
    std::atomic<uint32_t> m_completedSaveCount{0U};
    std::atomic<uint32_t> m_failureCount{0U};
    // Pair protected by the existing short worker mutex; only settings_io writes.
    // Active generation is worker-only, zero for explicit reformat/boot work.
    uint32_t m_settingsOperationActiveGeneration = 0U;
    uint32_t m_settingsOperationWorkerGeneration = 0U;
    fxe::SettingsSaveOperation m_settingsLastOperation{};
    fxe::SettingsSaveOperation m_settingsWorstOperation{};
    SettingsPhysicalAccess m_settingsPhysicalAccess{};
    SettingsPhysicalAccessSample m_settingsLastPhysicalAccess{};
    SettingsPhysicalAccessSample m_settingsWorstPhysicalAccess{};
#if defined(LITCH_NATIVE)
    void (*m_storageAccessTestHook)(void*, bool) = nullptr;
    void* m_storageAccessTestContext = nullptr;
    void (*m_settingsStorageAccessTestHook)(void*) = nullptr;
    void* m_settingsStorageAccessTestContext = nullptr;
#endif
    // Worker-only active counters; publication never exposes a partial request.
    PersistenceTiming m_activePersistenceTiming{};
    PersistenceTiming m_settingsTiming{};
    uint32_t m_persistenceTimingSequence = 0U;
    std::atomic<uint32_t> m_lastSettingsMs{0U};
    std::atomic<uint32_t> m_lastArtworkMs{0U};
    std::atomic<uint32_t> m_flashAccesses{0U};
#if defined(LITCH_ESP32S3)
    std::atomic<uint32_t> m_litchcraftArtworkFailureCount{0U};
    // Existing benchmark storage, reused without another queue or buffer.
    // 1/2/3/4 settings pending/running/passed/failed;
    // 5/6 saved-read between-slices/in-slice; 7/8 full passed/failed;
    // 9/10 single-frame passed/failed. Bytes are actual file data, not padding.
    std::atomic<uint8_t> m_storageReadState{0U};
    std::atomic<uint32_t> m_storageReadBytes{0U};
    std::atomic<uint32_t> m_storageReadWorkerMs{0U};
    std::atomic<uint32_t> m_storageReadFileMs{0U};
    std::atomic<uint32_t> m_storageReadHash{0U};
    std::atomic<uint32_t> m_storageReadCalls{0U};
    // Last completed wire replacement timing. During an active replacement,
    // body write/prepare values are cumulative/current; successful publication
    // freezes the complete transfer values for support diagnostics.
    // Storage-worker-only edit diagnostics; keep phase counters off the
    // filesystem/256-frame verification stack and separate from transfers.
    bool m_litchcraftFrameEditTimingActive = false;
    uint32_t m_litchcraftFrameEditBeginMs = UINT32_MAX;
    uint32_t m_litchcraftFrameEditPrepareMs = 0U;
    uint32_t m_litchcraftFrameEditPublishMs = 0U;
    std::atomic<uint32_t> m_frameStoreTransferMs{0U};
    std::atomic<uint32_t> m_frameStoreBodyWriteMs{0U};
    std::atomic<uint32_t> m_frameStoreBodyPrepareMs{0U};
    std::atomic<uint32_t> m_frameStoreRootPublishMs{0U};
    std::atomic<uint32_t> m_frameStoreBodyBytes{0U};
    std::atomic<uint32_t> m_frameStoreIngressPackets{0U};
#endif
    std::atomic<uint32_t> m_persistedSaveTotal{0U};
    std::atomic<uint8_t> m_lastError{
        static_cast<uint8_t>(fxe::SettingsStoreError::None)};
};

} // namespace firmware

#endif
