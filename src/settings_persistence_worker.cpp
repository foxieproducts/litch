#include "firmware/settings_persistence_worker.hpp"
#include "runtime/progress_watchdog.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/millis.hpp"

#if defined(LITCH_ESP32S3)
#include "modules/litchcraft_frame_store.hpp"
#if !defined(LITCH_NATIVE)
#include "firmware/litchcraft_frame_file.hpp"
#endif
#endif

#ifdef ARDUINO

#if defined(LITCH_ESP32S3)
#include <esp_partition.h>
#endif

#include "foxie_embedded/elapsed_time.hpp"
#include "logger.hpp"

namespace firmware {
namespace {

#if defined(LITCH_ESP32S3)
// Share Core 0 fairly with CPU think (priority 2); BLE remains higher priority.
constexpr UBaseType_t SETTINGS_TASK_PRIORITY = 2U;
#else
constexpr UBaseType_t SETTINGS_TASK_PRIORITY = 1U;
#endif
constexpr BaseType_t SETTINGS_TASK_CORE = 0;
constexpr uint32_t DIRECT_STORAGE_WAIT_TIMEOUT_MS = 3000U;
#if defined(LITCH_ESP32S3)
constexpr uint32_t LITCHCRAFT_FRAME_TRANSFER_TIMEOUT_MS = 10000U;
constexpr uint32_t LITCHCRAFT_FRAME_TRANSFER_TICK_MS = 100U;

bool SameFrameProjection(
    const modules::litchcraft_frame_store::StoreProjection& left,
    const modules::litchcraft_frame_store::StoreProjection& right) {
    return left.session == right.session &&
        left.selectedFrame == right.selectedFrame &&
        left.topology == right.topology && left.active == right.active &&
        left.playing == right.playing;
}

bool SameFrameStorageAuthority(
    const modules::litchcraft_frame_store::StoreProjection& left,
    const modules::litchcraft_frame_store::StoreProjection& right) {
    // Selection and play/pause are live presentation state. They may change
    // while a snapshot flash read is in flight, but they do not change which
    // session owns the Store or whether its contents remain readable.
    return left.session == right.session &&
        left.topology == right.topology && left.active == right.active;
}
#endif

// Serialize an immutable object with one field removed or replaced. Factory
// records must be validated before queue admission without a second JSON pool.
size_t SerializeFactorySnapshot(const JsonDocument& document, const char* key,
                                const char* replacement, char* const output,
                                const size_t capacity) {
    const JsonObjectConst object = document.as<JsonObjectConst>();
    if (object.isNull() || key == nullptr || capacity < 3U) return 0U;
    size_t cursor = 0U;
    const auto put = [&](const char value) {
        if (cursor + 1U >= capacity) return false;
        if (output != nullptr) output[cursor] = value;
        ++cursor;
        return true;
    };
    const auto append = [&](JsonVariantConst value) {
        const size_t size = measureJson(value);
        if (size >= capacity - cursor ||
            (output != nullptr && serializeJson(value, output + cursor, capacity - cursor) != size)) {
            return false;
        }
        cursor += size;
        return true;
    };
    if (!put('{')) return 0U;
    bool first = true;
    StaticJsonDocument<0> scalar;
    for (const JsonPairConst pair : object) {
        const char* const name = pair.key().c_str();
        if (name == nullptr) return 0U;
        if (std::strcmp(name, key) == 0) continue;
        if ((!first && !put(',')) || !scalar.set(name) ||
            !append(scalar.as<JsonVariantConst>()) || !put(':') || !append(pair.value())) return 0U;
        first = false;
    }
    if (replacement != nullptr) {
        if ((!first && !put(',')) || !scalar.set(key) ||
            !append(scalar.as<JsonVariantConst>()) || !put(':') || !scalar.set(replacement) ||
            !append(scalar.as<JsonVariantConst>())) return 0U;
    }
    if (!put('}')) return 0U;
    if (output != nullptr) output[cursor] = '\0';
    return cursor;
}

bool GenerationReached(const uint32_t completed, const uint32_t target) {
    return static_cast<int32_t>(completed - target) >= 0;
}


} // namespace

bool SettingsPersistenceWorker::SetDurableSnapshotCallback(
    const DurableSnapshotCallback callback, void* const context) {
    if (IsStarted() || callback == nullptr ||
        m_durableSnapshotCallback != nullptr) {
        return false;
    }
    m_durableSnapshotCallback = callback;
    m_durableSnapshotContext = context;
    return true;
}

bool SettingsPersistenceWorker::SetSnapshotTransactionCallbacks(
    const PrepareSnapshotCallback prepare,
    const SnapshotTransactionResultCallback result,
    void* const context) {
    if (IsStarted() || prepare == nullptr || result == nullptr ||
        m_prepareSnapshotCallback != nullptr ||
        m_snapshotTransactionResultCallback != nullptr) {
        return false;
    }
    m_prepareSnapshotCallback = prepare;
    m_snapshotTransactionResultCallback = result;
    m_snapshotTransactionContext = context;
    return true;
}

bool SettingsPersistenceWorker::SetCurrentGameSyncServiceCallback(
    const CurrentGameSyncServiceCallback callback, void* const context) {
    if (IsStarted() || callback == nullptr || context == nullptr ||
        m_currentGameSyncServiceCallback != nullptr) {
        return false;
    }
    m_currentGameSyncServiceCallback = callback;
    m_currentGameSyncServiceContext = context;
    return true;
}

void SettingsPersistenceWorker::NotifyCurrentGameSync() {
    if (IsStarted() && m_task != nullptr) {
        xTaskNotifyGive(m_task);
    }
}

void* SettingsPersistenceWorker::ActiveSnapshotTransactionScratch(
    const size_t bytes, const size_t alignment) {
    if (!m_snapshotTransactionActive || bytes == 0U ||
        bytes > SNAPSHOT_BUFFER_CAPACITY || alignment == 0U ||
        (alignment & (alignment - 1U)) != 0U) {
        return nullptr;
    }
    char* const buffer = SettingsBuffer(m_activeBufferIndex);
    const uintptr_t base = reinterpret_cast<uintptr_t>(buffer);
    const uintptr_t candidate =
        (base + SNAPSHOT_BUFFER_CAPACITY - bytes) & ~(alignment - 1U);
    return candidate >= base
        ? reinterpret_cast<void*>(candidate)
        : nullptr;
}

bool SettingsPersistenceWorker::Begin(fxe::SettingsStore& source) {
    if (IsStarted()) {
        return true;
    }

    m_mutex = xSemaphoreCreateMutexStatic(&m_mutexStorage);
    if (m_mutex == nullptr) {
        return false;
    }

    const uint32_t persistedTotal =
        source.Doc()[fxe::SETTINGS_SAVE_TOTAL_KEY] | 0U;
    m_persistedSaveTotal.store(persistedTotal, std::memory_order_release);
    m_source = &source;
#if defined(LITCH_ESP32S3)
    if (!m_litchcraftArtworkStorage.SetAccessOps(
            {this,
             [](void* context) {
                 static_cast<SettingsPersistenceWorker*>(context)
                     ->AcquireFlashAccessWindow();
             },
             [](void* context) {
                 static_cast<SettingsPersistenceWorker*>(context)
                     ->ReleaseFlashAccessWindow();
             }})) {
        m_source = nullptr;
        return false;
    }
#endif
    m_writer.SetStorageAccessCallbacks(
        [this]() {
            AcquireFlashAccessWindow();
            m_settingsPhysicalAccess.BeginOperation();
        },
        [this]() {
            m_settingsPhysicalAccess.EndOperation();
            ReleaseFlashAccessWindow();
#if defined(LITCH_NATIVE)
            if (m_settingsStorageAccessTestHook != nullptr) {
                m_settingsStorageAccessTestHook(m_settingsStorageAccessTestContext);
            }
#endif
#if defined(LITCH_ESP32S3)
            // Only the settings writer invokes this hook. FrameFile ends its
            // observation scope without recursively servicing another page.
            ServiceLitchcraftFramePagingDuringSettings();
#endif
        });
    m_writer.SetSaveOperationCallback([this](const fxe::SettingsSaveOperation& operation) {
        if (m_settingsPhysicalAccess.IsRecordingTransaction()) {
            SettingsPhysicalAccessMicroCounter::Add(m_activePersistenceTiming.fileCalls, 1U);
            SettingsPhysicalAccessMicroCounter::Add(m_activePersistenceTiming.fileMs, operation.durationMs);
        }
        // The SettingsStore calls only after releasing storage access and paging.
        // Copying two scalars-only records never extends a flash lease.
        if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return;
        const uint32_t generation = m_settingsOperationActiveGeneration;
        const bool newSave = m_settingsLastOperation.kind == fxe::SettingsSaveOperationKind::None ||
            m_settingsOperationWorkerGeneration != generation ||
            (generation == 0U && m_settingsLastOperation.saveGeneration != operation.saveGeneration);
        m_settingsOperationWorkerGeneration = generation;
        m_settingsLastOperation = operation;
        m_settingsLastPhysicalAccess = m_settingsPhysicalAccess.Sample();
        if (newSave || operation.durationMs > m_settingsWorstOperation.durationMs) {
            m_settingsWorstOperation = operation;
            m_settingsWorstPhysicalAccess = m_settingsLastPhysicalAccess;
        }
        xSemaphoreGive(m_mutex);
    });
    if (!m_taskStorage.TryClaim()) {
        m_source = nullptr;
        return false;
    }
    m_task = xTaskCreateStaticPinnedToCore(
        &SettingsPersistenceWorker::TaskEntry, "settings_io",
        SettingsFactoryTaskStorage::SETTINGS_STACK_BYTES, this, SETTINGS_TASK_PRIORITY,
        m_taskStorage.stack, &m_taskStorage.control, SETTINGS_TASK_CORE);
    if (m_task == nullptr) {
        m_taskStorage.Release();
        m_source = nullptr;
        return false;
    }

#if defined(LITCH_ESP32S3)
    // Resolve once outside wrapped SDK accesses. The worker and this immutable
    // binding live for the firmware lifetime; other tasks only compare identity.
    m_settingsPhysicalAccess.Bind(m_task, esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs"));
    RegisterSettingsPhysicalAccess(&m_settingsPhysicalAccess);
#endif
#if defined(LITCH_ESP32S3)
    if (!source.IsDisabled()) m_litchcraftFrameStream.BeginInitialRecovery();
#endif
    m_started.store(true, std::memory_order_release);
    source.SetAsyncSaveAdmissionCallback(
        [this](const JsonDocument& document) {
            auto admission = fxe::SettingsStore::AsyncSaveAdmission::Rejected;
            Enqueue(document, nullptr, &admission);
            return admission;
        });
    source.SetBeforeDirectStorageAccessCallback(
        [this]() {
            return DrainForDirectStorageAccess();
        });
#if defined(LITCH_ESP32S3)
    // Boot recovery waits until the live render/control surfaces are ready.
    m_litchcraftFramePlaybackPending.store(false, std::memory_order_release);
#endif
    return true;
}

bool SettingsPersistenceWorker::Enqueue(const JsonDocument& document) {
    return Enqueue(document, nullptr);
}

bool SettingsPersistenceWorker::EnqueueTracked(
    const JsonDocument& document, SaveReceipt& outReceipt,
    fxe::SettingsStore::AsyncSaveAdmission* const admission,
    std::atomic<SaveReceipt>* const publishedReceipt,
    const TrackedPrerequisite prerequisite, void* const prerequisiteContext,
    const char* const omittedSetting, const char* const replacement) {
    outReceipt = 0U;
    if (publishedReceipt != nullptr) {
        publishedReceipt->store(0U, std::memory_order_release);
    }
    return Enqueue(document, &outReceipt, admission, publishedReceipt,
                   prerequisite, prerequisiteContext, omittedSetting, replacement);
}

#if defined(LITCH_ESP32S3)
bool SettingsPersistenceWorker::SetLitchcraftArtworkStorageOps(
    const LitchcraftArtworkStorage::Ops ops) {
    if (IsStarted() || !LitchcraftArtworkStorage(ops).IsConfigured()) {
        return false;
    }
    m_litchcraftArtworkStorage = LitchcraftArtworkStorage(ops);
    return true;
}

bool SettingsPersistenceWorker::SetLitchcraftArtworkCurrentKnowledge(
    const LitchcraftArtworkStorage::CurrentKnowledge knowledge) {
    if (!IsStarted() || m_mutex == nullptr ||
        xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool idle = m_litchcraftArtworkState == BufferState::Free &&
        !m_litchcraftFrameStorageActive && !m_litchcraftFrameIngressPending;
    const bool accepted = idle &&
        m_litchcraftArtworkStorage.SetCurrentKnowledge(knowledge);
    xSemaphoreGive(m_mutex);
    return accepted;
}

LitchcraftArtworkStorage::Status
SettingsPersistenceWorker::ReadLitchcraftArtwork(
    const LitchcraftArtworkStorage::Artifact artifact, uint8_t* const out,
    const size_t capacity, size_t& outSize) {
    outSize = 0U;
    if (!IsStarted() || m_mutex == nullptr) {
        return LitchcraftArtworkStorage::Status::IoFailure;
    }
    const fxe::ElapsedTime elapsed = fxe::ElapsedTime::StartNow();
    while (!elapsed.HasElapsed(DIRECT_STORAGE_WAIT_TIMEOUT_MS)) {
        if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) {
            return LitchcraftArtworkStorage::Status::IoFailure;
        }
        bool idle = m_litchcraftArtworkState == BufferState::Free &&
            !m_litchcraftFrameStorageActive &&
            !m_litchcraftFrameIngressPending;
        for (uint8_t index = 0U; index < BUFFER_COUNT && idle; ++index) {
            idle = m_bufferState[index] == BufferState::Free;
        }
        if (idle) {

            const auto status = m_litchcraftArtworkStorage.Read(
                artifact, out, capacity, outSize);
            xSemaphoreGive(m_mutex);
            return status;
        }
        xSemaphoreGive(m_mutex);
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    return LitchcraftArtworkStorage::Status::IoFailure;
}

bool SettingsPersistenceWorker::EnqueueLitchcraftArtworkTracked(
    const uint8_t* const bytes, const size_t size, SaveReceipt& outReceipt,
    fxe::SettingsStore::AsyncSaveAdmission* const admission) {
    outReceipt = 0U;
    if (admission != nullptr) {
        *admission = fxe::SettingsStore::AsyncSaveAdmission::Rejected;
    }
    if (!IsStarted() || m_mutex == nullptr || m_task == nullptr ||
        m_source == nullptr || m_source->IsDisabled() ||
        GetStorageReformatState() != StorageReformatState::Idle ||
        bytes == nullptr || size == 0U ||
        size > LitchcraftArtworkStorage::SLOT_CAPACITY) {
        return false;
    }
    if (xSemaphoreTake(m_mutex, 0U) != pdTRUE) {
        if (admission != nullptr) *admission = fxe::SettingsStore::AsyncSaveAdmission::Deferred;
        return false;
    }
    if (m_receiptGeneration.load(std::memory_order_acquire) != 0U ||
        m_litchcraftFrameStorageActive || m_litchcraftFrameIngressPending ||
        m_litchcraftArtworkState != BufferState::Free) {
        if (admission != nullptr) {
            *admission = fxe::SettingsStore::AsyncSaveAdmission::Deferred;
        }
        xSemaphoreGive(m_mutex);
        return false;
    }
    for (const BufferState state : m_bufferState) {
        if (state != BufferState::Free) {
            if (admission != nullptr) {
                *admission = fxe::SettingsStore::AsyncSaveAdmission::Deferred;
            }
            xSemaphoreGive(m_mutex);
            return false;
        }
    }
    ++m_nextGeneration;
    if (m_nextGeneration == 0U) ++m_nextGeneration;
    m_litchcraftArtworkBytes = bytes;
    m_litchcraftArtworkSize = size;
    m_litchcraftArtworkGeneration = m_nextGeneration;

    m_litchcraftArtworkState = BufferState::Pending;
    m_lastRequestedGeneration.store(m_nextGeneration,
                                    std::memory_order_release);
    m_receiptOutcome.store(SaveOutcome::Pending, std::memory_order_release);
    m_receiptGeneration.store(m_nextGeneration, std::memory_order_release);
    outReceipt = m_nextGeneration;
    if (admission != nullptr) {
        *admission = fxe::SettingsStore::AsyncSaveAdmission::Accepted;
    }
    xSemaphoreGive(m_mutex);
    xTaskNotifyGive(m_task);
    return true;
}

bool SettingsPersistenceWorker::EnqueueLitchcraftArtworkDocumentTracked(
    const modules::litchcraft_animation::Document& document,
    const bool active,
    modules::LitchcraftPersistence::AdvancedEnvelope& envelope,
    SaveReceipt& outReceipt,
    fxe::SettingsStore::AsyncSaveAdmission* const admission) {
    outReceipt = 0U;
    if (admission != nullptr) {
        *admission = fxe::SettingsStore::AsyncSaveAdmission::Rejected;
    }
    if (!IsStarted() || m_mutex == nullptr || m_task == nullptr ||
        m_source == nullptr || m_source->IsDisabled() ||
        GetStorageReformatState() != StorageReformatState::Idle) return false;
    if (xSemaphoreTake(m_mutex, 0U) != pdTRUE) {
        if (admission != nullptr) {
            *admission = fxe::SettingsStore::AsyncSaveAdmission::Deferred;
        }
        return false;
    }
    if (m_receiptGeneration.load(std::memory_order_acquire) != 0U ||
        m_litchcraftFrameStorageActive || m_litchcraftFrameIngressPending ||
        m_litchcraftArtworkState != BufferState::Free) {
        if (admission != nullptr) {
            *admission = fxe::SettingsStore::AsyncSaveAdmission::Deferred;
        }
        xSemaphoreGive(m_mutex);
        return false;
    }
    for (const BufferState state : m_bufferState) {
        if (state != BufferState::Free) {
            if (admission != nullptr) {
                *admission = fxe::SettingsStore::AsyncSaveAdmission::Deferred;
            }
            xSemaphoreGive(m_mutex);
            return false;
        }
    }
    ++m_nextGeneration;
    if (m_nextGeneration == 0U) ++m_nextGeneration;
    m_pendingStorage.settings.~SettingsBuffers();
    new (&m_pendingStorage.artwork) ArtworkDocument(document);
    m_litchcraftArtworkDocument = &m_pendingStorage.artwork;
    m_litchcraftArtworkEnvelope = &envelope;
    m_litchcraftArtworkActive = active;
    m_litchcraftArtworkOwnsDocument = true;
    m_litchcraftArtworkBytes = nullptr;
    m_litchcraftArtworkSize = 0U;
    m_litchcraftArtworkGeneration = m_nextGeneration;

    m_litchcraftArtworkState = BufferState::Pending;
    m_lastRequestedGeneration.store(m_nextGeneration,
                                    std::memory_order_release);
    m_receiptOutcome.store(SaveOutcome::Pending, std::memory_order_release);
    m_receiptGeneration.store(m_nextGeneration, std::memory_order_release);
    outReceipt = m_nextGeneration;
    if (admission != nullptr) {
        *admission = fxe::SettingsStore::AsyncSaveAdmission::Accepted;
    }
    xSemaphoreGive(m_mutex);
    xTaskNotifyGive(m_task);
    return true;
}

bool SettingsPersistenceWorker::RequestSettingsReadDiagnostic() {
    if (!IsStarted() || m_task == nullptr || m_source == nullptr || m_source->IsDisabled() ||
        GetStorageReformatState() != StorageReformatState::Idle ||
        xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return false;
    const uint8_t state = m_storageReadState.load(std::memory_order_acquire);
    const bool busy = state == 1U || state == 2U || state == 5U || state == 6U ||
                      state == 11U || state == 12U;
    if (!busy) m_storageReadState.store(1U, std::memory_order_release);
    xSemaphoreGive(m_mutex);
    if (!busy) xTaskNotifyGive(m_task);
    return !busy;
}

bool SettingsPersistenceWorker::RequestLitchcraftFrameClear() {
    return RequestLitchcraftFrameClear(FrameClearState::Pending);
}

bool SettingsPersistenceWorker::RequestLitchcraftFrameClear(const FrameClearState pending) {
    if (!IsStarted() || m_task == nullptr || m_mutex == nullptr ||
        m_source == nullptr || m_source->IsDisabled() ||
        GetStorageReformatState() != StorageReformatState::Idle ||
        !m_litchcraftFrameRuntimeReady.load(std::memory_order_acquire) ||
        xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return false;
    const auto state = m_litchcraftFrameClearState.load(std::memory_order_acquire);
    const bool busy = state != FrameClearState::Idle ||
        m_litchcraftFrameCommitPending.load(std::memory_order_acquire) ||
        m_litchcraftFrameStream.ReplacementPending();
    using Scope = modules::litchcraft_frame_stream::FrameStream::RetirementScope;
    const bool prelaunchLC5 = pending == FrameClearState::PrelaunchLC5Pending;
    const bool retired = !busy && m_litchcraftFrameStream.RetireDocument(
        prelaunchLC5 ? Scope::EmptyOnly : Scope::CurrentDocument);
    if (busy || (!retired &&
                 (prelaunchLC5 || !m_litchcraftFrameStream.NeedsDurableRetirement()))) {
        xSemaphoreGive(m_mutex);
        return false;
    }
    m_litchcraftFrameClearQueue = m_litchcraftFrameQueue;
    m_litchcraftFrameClearQueueEpoch = m_litchcraftFrameClearQueue == nullptr
        ? 0U : m_litchcraftFrameClearQueue->CurrentEpoch();
    m_litchcraftFrameClearState.store(pending, std::memory_order_release);
    // Close the existing lifecycle/OTA barrier before flash or stream adoption.
    m_litchcraftFrameModeSavePending.store(true, std::memory_order_release);
    m_litchcraftFramePlaybackPending.store(true, std::memory_order_release);
    xSemaphoreGive(m_mutex);
    xTaskNotifyGive(m_task);
    return true;
}

void SettingsPersistenceWorker::NotifyLitchcraftFrameIngress(
    modules::BleOtaChunkQueue& queue,
    const modules::litchcraft_frame_store::StoreProjection projection,
    const uint32_t ownerEpoch, const uint16_t packetMaximum) {
    const bool hasPacket = queue.TryPeekLitchcraftFrameStore();
    if (!IsStarted() || m_mutex == nullptr || m_task == nullptr ||
        ownerEpoch == 0U || packetMaximum < 20U ||
        packetMaximum > 512U ||
        xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    const bool ownerChanged = m_litchcraftFrameOwnerEpoch != 0U &&
        m_litchcraftFrameOwnerEpoch != ownerEpoch;
    const bool projectionChanged =
        m_litchcraftFrameOwnerEpoch != ownerEpoch ||
        !SameFrameProjection(m_litchcraftFrameProjection, projection);
    const bool storageAuthorityChanged =
        m_litchcraftFrameOwnerEpoch != ownerEpoch ||
        !SameFrameStorageAuthority(m_litchcraftFrameProjection, projection);
    const bool previewLagging =
        m_litchcraftFramePreviewTransitionPending && !ownerChanged &&
        SameFrameProjection(projection, m_litchcraftFramePreviewPrevious);
    const bool previewReached =
        m_litchcraftFramePreviewTransitionPending && !ownerChanged &&
        SameFrameProjection(projection, m_litchcraftFramePreviewExpected);
    const bool ownsStorage = m_litchcraftFrameStorageActive;
    const bool hasResponse = m_litchcraftFrameResponseSize != 0U;
    if (!hasPacket && !((ownsStorage || hasResponse) && projectionChanged)) {
        xSemaphoreGive(m_mutex);
        return;
    }
    m_litchcraftFrameQueue = &queue;
    if (ownerChanged) {
        m_litchcraftFrameResponseSize = 0U;
        m_litchcraftFrameLastResponseSize = 0U;
        m_litchcraftFramePrefetchCount = 0U;
        m_litchcraftFramePrefetchHead = 0U;
        m_litchcraftFrameResponseOwnerEpoch = 0U;
        m_litchcraftFrameResponseProjectionRevision = 0U;
        m_litchcraftFramePrioritySize = 0U;
        m_litchcraftFrameDiscardSession = 0U;
        m_litchcraftFrameDiscardTransfer = 0U;
        m_litchcraftFrameDiscardOwnerEpoch = 0U;
        m_litchcraftFramePreviewTransitionPending = false;
    }
    m_litchcraftFrameOwnerEpoch = ownerEpoch;
    m_litchcraftFramePacketMaximum = packetMaximum;
    m_litchcraftFrameIngressPending = true;
    m_litchcraftFrameProjection = projection;
    if (storageAuthorityChanged && !previewLagging && !previewReached &&
        (!hasResponse || ownerChanged)) {
        m_litchcraftFrameProjectionRevision.fetch_add(
            1U, std::memory_order_acq_rel);
        m_litchcraftFrameResponseSize = 0U;
        m_litchcraftFrameLastResponseSize = 0U;
        m_litchcraftFramePrefetchCount = 0U;
        m_litchcraftFramePrefetchHead = 0U;
        m_litchcraftFrameResponseOwnerEpoch = 0U;
        m_litchcraftFrameResponseProjectionRevision = 0U;
    }
    if (previewReached) {
        m_litchcraftFramePreviewTransitionPending = false;
    }
    // A Store response is the application-level barrier for the packets that
    // produced it. A successful Commit can change the live playback projection
    // before the main loop has notified that receipt. Adopt the new projection,
    // but keep the response revision stable until BLE accepts the notification;
    // otherwise the expected mode change silently erases Commit's receipt.
    const bool wake = m_litchcraftFrameResponseSize == 0U &&
        (hasPacket || storageAuthorityChanged);
    if (wake && hasPacket) {
        // Publish priority before waking the worker so an outer flash
        // admission can finish and service the queued BLE packet. SDK calls
        // inside the acquired lease still yield between bounded slices.

    }
    xSemaphoreGive(m_mutex);
    if (wake) xTaskNotifyGive(m_task);
}

void SettingsPersistenceWorker::NotifyLitchcraftFramePriority(
    const uint8_t* const bytes, const size_t size,
    const uint32_t ownerEpoch) {
    if (!IsStarted() || m_mutex == nullptr || m_task == nullptr ||
        bytes == nullptr || size != m_litchcraftFramePriority.size() ||
        bytes[0] != 0x4dU || bytes[1] != 2U ||
        bytes[2] != modules::litchcraft_frame_store::STORE_CANCEL ||
        ownerEpoch == 0U ||
        xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    const bool ownerChanged = m_litchcraftFrameOwnerEpoch != 0U &&
        m_litchcraftFrameOwnerEpoch != ownerEpoch;
    if (ownerChanged) {
        m_litchcraftFramePrioritySize = 0U;
        m_litchcraftFrameDiscardSession = 0U;
        m_litchcraftFrameDiscardTransfer = 0U;
        m_litchcraftFrameDiscardOwnerEpoch = 0U;
        m_litchcraftFrameProjectionRevision.fetch_add(
            1U, std::memory_order_acq_rel);
    }
    // Cancel is the only priority schema-4 packet. Its terminal receipt
    // supersedes an undelivered Begin/Checkpoint State so notification loss
    // cannot keep the transfer and shared storage union alive for 10 seconds.
    m_litchcraftFrameResponseSize = 0U;
    m_litchcraftFrameLastResponseSize = 0U;
    m_litchcraftFramePrefetchCount = 0U;
    m_litchcraftFramePrefetchHead = 0U;
    m_litchcraftFrameResponseOwnerEpoch = 0U;
    m_litchcraftFrameResponseProjectionRevision = 0U;
    if (m_litchcraftFramePrioritySize == 0U) {
        std::copy_n(bytes, size, m_litchcraftFramePriority.data());
        m_litchcraftFramePrioritySize = size;
    }
    m_litchcraftFrameOwnerEpoch = ownerEpoch;
    m_litchcraftFrameIngressPending = true;

    xSemaphoreGive(m_mutex);
    xTaskNotifyGive(m_task);
}

bool SettingsPersistenceWorker::PeekLitchcraftFrameResponse(
    uint8_t* const out, const size_t capacity, size_t& outSize) {
    outSize = 0U;
    if (out == nullptr || m_mutex == nullptr ||
        xSemaphoreTake(m_mutex, 0U) != pdTRUE) {
        return false;
    }
    const uint32_t projectionRevision =
        m_litchcraftFrameProjectionRevision.load(std::memory_order_acquire);
    const bool current =
        m_litchcraftFrameResponseOwnerEpoch == m_litchcraftFrameOwnerEpoch &&
        m_litchcraftFrameResponseProjectionRevision == projectionRevision;
    if (m_litchcraftFrameResponseSize != 0U && !current) {
        m_litchcraftFrameResponseSize = 0U;
        m_litchcraftFramePrefetchCount = 0U;
        m_litchcraftFramePrefetchHead = 0U;
        m_litchcraftFrameResponseOwnerEpoch = 0U;
        m_litchcraftFrameResponseProjectionRevision = 0U;
    }
    const bool available = current && m_litchcraftFrameResponseSize != 0U &&
        capacity >= m_litchcraftFrameResponseSize;
    if (available) {
        outSize = m_litchcraftFrameResponseSize;
        std::copy_n(m_litchcraftFrameResponse.data(), outSize, out);
    }
    xSemaphoreGive(m_mutex);
    return available;
}

void SettingsPersistenceWorker::AcceptLitchcraftFrameResponse() {
    if (m_mutex == nullptr ||
        xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (m_litchcraftFrameResponseSize != 0U &&
        m_litchcraftFrameResponseOwnerEpoch == m_litchcraftFrameOwnerEpoch) {
        m_litchcraftFrameLastResponseSize = static_cast<uint16_t>(
            m_litchcraftFrameResponseSize.load(std::memory_order_acquire));
        std::copy_n(m_litchcraftFrameResponse.data(),
                    m_litchcraftFrameLastResponseSize,
                    m_litchcraftFrameLastResponse.data());
    }
    if (m_litchcraftFramePrefetchCount != 0U) {
        const auto& next =
            m_litchcraftFramePrefetch[m_litchcraftFramePrefetchHead];
        std::copy_n(next.response.data(), next.responseSize,
                    m_litchcraftFrameResponse.data());
        m_litchcraftFrameResponseSize = next.responseSize;
        m_litchcraftFramePrefetchHead = static_cast<uint8_t>(
            (m_litchcraftFramePrefetchHead + 1U) %
            FRAME_RESPONSE_PREFETCH_CAPACITY);
        --m_litchcraftFramePrefetchCount;
    } else {
        m_litchcraftFrameResponseSize = 0U;
        m_litchcraftFrameResponseOwnerEpoch = 0U;
        m_litchcraftFrameResponseProjectionRevision = 0U;
    }
    const bool wake = m_litchcraftFrameIngressPending &&
        m_litchcraftFrameResponseSize == 0U;
    xSemaphoreGive(m_mutex);
    if ((wake || m_litchcraftFramePlaybackPending.load(
                     std::memory_order_acquire)) &&
        m_task != nullptr) {
        xTaskNotifyGive(m_task);
    }
}

bool SettingsPersistenceWorker::ReplayLitchcraftFrameResponse(
    const uint16_t request, const uint32_t ownerEpoch) {
    if (request == 0U || ownerEpoch == 0U || m_mutex == nullptr ||
        xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const size_t pendingSize = m_litchcraftFrameResponseSize.load(
        std::memory_order_acquire);
    const size_t size = pendingSize != 0U
        ? pendingSize : m_litchcraftFrameLastResponseSize;
    const bool matched = ownerEpoch == m_litchcraftFrameOwnerEpoch &&
        size >= 13U &&
        fxe::ReadU16LE((pendingSize != 0U
                           ? m_litchcraftFrameResponse.data()
                           : m_litchcraftFrameLastResponse.data()) + 11U) ==
            request &&
        (pendingSize == 0U ||
         m_litchcraftFrameResponseOwnerEpoch == ownerEpoch);
    if (matched) {
        m_litchcraftFrameResponseOwnerEpoch = ownerEpoch;
        m_litchcraftFrameResponseProjectionRevision =
            m_litchcraftFrameProjectionRevision.load(std::memory_order_acquire);
        if (pendingSize == 0U) {
            std::copy_n(m_litchcraftFrameLastResponse.data(), size,
                        m_litchcraftFrameResponse.data());
            m_litchcraftFrameResponseSize.store(size, std::memory_order_release);
        }
    }
    xSemaphoreGive(m_mutex);
    return matched;
}

void SettingsPersistenceWorker::NotifyLitchcraftFramePlayback(
    const bool playing) {
    if (!IsStarted() || m_task == nullptr) {
        return;
    }
    m_litchcraftFramePlaybackResident.store(playing,
                                            std::memory_order_release);
    m_litchcraftFramePlaybackPending.store(true, std::memory_order_release);
    if (m_litchcraftFrameRuntimeReady.load(std::memory_order_acquire)) {
        xTaskNotifyGive(m_task);
    }
}

void SettingsPersistenceWorker::NotifyLitchcraftFrameSession(
    const bool playing, const uint32_t session, const bool active) {
    if (!IsStarted() || m_mutex == nullptr ||
        xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return;
    const bool modeChanged =
        m_litchcraftFrameBootstrapProjection.session != session ||
        m_litchcraftFrameBootstrapProjection.active != active;
    m_litchcraftFrameBootstrapProjection = {
        session, 0U, 3U, active, playing};
    if (session != 0U && modeChanged) {
        ++m_litchcraftFrameModeRevision;
        if (m_litchcraftFrameModeRevision == 0U) {
            ++m_litchcraftFrameModeRevision;
        }
        const bool disabled = m_source != nullptr && m_source->IsDisabled();
        m_litchcraftFrameModeSavePending.store(
            !disabled, std::memory_order_release);
        m_litchcraftFrameModeSaveFailed.store(
            disabled, std::memory_order_release);
    }
    xSemaphoreGive(m_mutex);
    NotifyLitchcraftFramePlayback(playing);
}

void SettingsPersistenceWorker::SetLitchcraftFrameSummaryMaintenanceReady(const bool ready) {
    const bool prior = m_litchcraftFrameSummaryMaintenanceReady.exchange(ready, std::memory_order_acq_rel);
    if (ready && !prior && IsStarted() && m_task != nullptr) {
        m_litchcraftFramePlaybackPending.store(true, std::memory_order_release);
        xTaskNotifyGive(m_task);
    }
}

void SettingsPersistenceWorker::NotifyLitchcraftFrameRuntimeReady() {
    if (!IsStarted() || m_task == nullptr) return;
    const uint32_t readyMs = fxe::Millis();
    if (m_litchcraftFrameRuntimeReady.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // A saved Store must resume without a phone request. Wait until the live
    // render loop exists, then recover and publish its first complete frame.
    m_litchcraftStartupReadyMs.store(readyMs, std::memory_order_release);
    m_litchcraftFramePlaybackPending.store(true, std::memory_order_release);
    xTaskNotifyGive(m_task);
}
#endif

SettingsPersistenceWorker::SaveOutcome SettingsPersistenceWorker::GetSaveOutcome(
    const SaveReceipt receipt) const {
    if (receipt == 0U || m_receiptGeneration.load(std::memory_order_acquire) != receipt) {
        return SaveOutcome::Unknown;
    }
    return m_receiptOutcome.load(std::memory_order_acquire);
}

bool SettingsPersistenceWorker::ReleaseSaveReceipt(const SaveReceipt receipt) {
    if (m_mutex == nullptr || xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const auto outcome = GetSaveOutcome(receipt);
    const bool terminal = outcome == SaveOutcome::Saved || outcome == SaveOutcome::Failed;
    if (terminal) {
        m_receiptGeneration.store(0U, std::memory_order_release);
        m_trackedPrerequisite = nullptr;
        m_trackedPrerequisiteContext = nullptr;
    }
    xSemaphoreGive(m_mutex);
    return terminal;
}

bool SettingsPersistenceWorker::Enqueue(
    const JsonDocument& document,
    SaveReceipt* const outReceipt,
    fxe::SettingsStore::AsyncSaveAdmission* const admission,
    std::atomic<SaveReceipt>* const publishedReceipt,
    const TrackedPrerequisite prerequisite, void* const prerequisiteContext,
    const char* const omittedSetting, const char* const replacement) {
    if (admission != nullptr) { *admission = fxe::SettingsStore::AsyncSaveAdmission::Rejected; }
    if (!IsStarted() || m_mutex == nullptr || m_task == nullptr ||
        m_source == nullptr || m_source->IsDisabled() ||
        GetStorageReformatState() != StorageReformatState::Idle) {
        return false;
    }
    if (xSemaphoreTake(m_mutex, 0U) != pdTRUE) {
        if (admission != nullptr) *admission = fxe::SettingsStore::AsyncSaveAdmission::Deferred;
        return false;
    }
    if (m_source->IsDisabled() ||
        GetStorageReformatState() != StorageReformatState::Idle) {
        xSemaphoreGive(m_mutex);
        return false;
    }
    if (m_receiptGeneration.load(std::memory_order_acquire) != 0U) {
        if (admission != nullptr) { *admission = fxe::SettingsStore::AsyncSaveAdmission::Deferred; }
        xSemaphoreGive(m_mutex);
        return false;
    }
    uint8_t firstBuffer = 0U;
#if defined(LITCH_ESP32S3)
    if (m_litchcraftFrameStorageActive || m_litchcraftFrameIngressPending ||
        m_litchcraftFramePlaybackResident.load(std::memory_order_acquire) ||
        m_litchcraftFramePlaybackPending.load(std::memory_order_acquire)) {
        firstBuffer = SHARED_SETTINGS_BUFFER_COUNT;
    }
#endif

    uint8_t selected = BUFFER_COUNT;
    for (uint8_t i = firstBuffer; i < BUFFER_COUNT; ++i) {
        if (m_bufferState[i] == BufferState::Pending) {
            selected = i;
            break;
        }
        if (selected == BUFFER_COUNT &&
            m_bufferState[i] == BufferState::Free) {
            selected = i;
        }
    }

    bool accepted = false;
    if (selected < BUFFER_COUNT) {
        // Measure the whole immutable document before touching a pending slot.
        // A refused newer snapshot must leave the accepted generation intact.
        const size_t required = omittedSetting == nullptr
            ? measureJson(document)
            : SerializeFactorySnapshot(document, omittedSetting, replacement, nullptr, SNAPSHOT_BUFFER_CAPACITY);
        if (required > 0U && required < SNAPSHOT_BUFFER_CAPACITY) {
            char* const buffer = SettingsBuffer(selected);
            const size_t written = omittedSetting == nullptr
                ? serializeJson(document, buffer, SNAPSHOT_BUFFER_CAPACITY)
                : SerializeFactorySnapshot(document, omittedSetting, replacement, buffer, SNAPSHOT_BUFFER_CAPACITY);
            if (written != required) {
                xSemaphoreGive(m_mutex);
                return false;
            }
            ++m_nextGeneration;
            if (m_nextGeneration == 0U) {
                ++m_nextGeneration;
            }
            m_bufferGeneration[selected] = m_nextGeneration;

            m_bufferState[selected] = BufferState::Pending;
            m_lastRequestedGeneration.store(
                m_nextGeneration, std::memory_order_release);
            if (outReceipt != nullptr) {
                m_receiptOutcome.store(SaveOutcome::Pending, std::memory_order_release);
                m_receiptGeneration.store(m_nextGeneration, std::memory_order_release);
                *outReceipt = m_nextGeneration;
                m_trackedPrerequisite = prerequisite;
                m_trackedPrerequisiteContext = prerequisiteContext;
                // Publish before releasing the queue mutex: the worker may
                // already be awake and finish before EnqueueTracked returns.
                if (publishedReceipt != nullptr) {
                    publishedReceipt->store(m_nextGeneration, std::memory_order_release);
                }
            }
            accepted = true;
            if (admission != nullptr) { *admission = fxe::SettingsStore::AsyncSaveAdmission::Accepted; }
        }
    }

    if (selected == BUFFER_COUNT && admission != nullptr) {
        // The resident slot may already be writing. Retain the caller's latest
        // dirty document for its ordinary retry instead of inventing an I/O
        // failure or overwriting an accepted snapshot.
        *admission = fxe::SettingsStore::AsyncSaveAdmission::Deferred;
    }
    xSemaphoreGive(m_mutex);
    if (accepted) {
        xTaskNotifyGive(m_task);
    }
    return accepted;
}

bool SettingsPersistenceWorker::Flush(
    const JsonDocument& document,
    const uint32_t timeoutMs) {
    if (!Enqueue(document, nullptr)) {
        return false;
    }
    const uint32_t target =
        m_lastRequestedGeneration.load(std::memory_order_acquire);
    const fxe::ElapsedTime elapsed = fxe::ElapsedTime::StartNow();
    while (!GenerationReached(
        m_lastSavedGeneration.load(std::memory_order_acquire), target)) {
        if (GenerationReached(
                m_lastCompletedGeneration.load(std::memory_order_acquire),
                target)) {
            break;
        }
        if (elapsed.HasElapsed(timeoutMs)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    const bool saved = GenerationReached(
        m_lastSavedGeneration.load(std::memory_order_acquire), target);
    return saved;
}

bool SettingsPersistenceWorker::IsIdle() const {
    const uint32_t requested =
        m_lastRequestedGeneration.load(std::memory_order_acquire);
    const uint32_t completed =
        m_lastCompletedGeneration.load(std::memory_order_acquire);
    const auto recovery = GetStorageReformatState();
    bool idle = recovery != StorageReformatState::Pending &&
        recovery != StorageReformatState::Running && requested == completed &&
        m_receiptGeneration.load(std::memory_order_acquire) == 0U;
#if defined(LITCH_ESP32S3)
    idle = idle && !m_litchcraftFrameStorageActive &&
        !m_litchcraftFrameIngressPending &&
        !m_litchcraftFramePlaybackPending &&
        m_litchcraftFrameResponseSize == 0U &&
        m_litchcraftFrameClearState.load(std::memory_order_acquire) !=
            FrameClearState::Pending &&
        m_litchcraftFrameClearState.load(std::memory_order_acquire) !=
            FrameClearState::Running &&
        m_litchcraftFrameClearState.load(std::memory_order_acquire) !=
            FrameClearState::PrelaunchLC5Pending;
#endif
#if defined(LITCH_ESP32S3)
    const auto readState = m_storageReadState.load(std::memory_order_acquire);
    idle = idle && readState != 1U && readState != 2U;
#endif
    return idle;
}

bool SettingsPersistenceWorker::WaitUntilIdle(
    const uint32_t timeoutMs) const {
    const fxe::ElapsedTime elapsed = fxe::ElapsedTime::StartNow();
    while (!IsIdle()) {
        if (elapsed.HasElapsed(timeoutMs)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    return true;
}

void SettingsPersistenceWorker::TaskEntry(void* context) {
    auto* self = static_cast<SettingsPersistenceWorker*>(context);
    if (self != nullptr) {
        self->Run();
    }
    vTaskDelete(nullptr);
}

bool SettingsPersistenceWorker::TryTakePending(uint8_t& outIndex) {
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    CancelDisabledPending();
    bool found = false;
    for (uint8_t i = 0U; i < BUFFER_COUNT; ++i) {
        if (m_bufferState[i] != BufferState::Pending) {
            continue;
        }
        m_bufferState[i] = BufferState::Writing;
        outIndex = i;
        found = true;
        break;
    }
    xSemaphoreGive(m_mutex);
    return found;
}

#if defined(LITCH_ESP32S3)
bool SettingsPersistenceWorker::TryTakeLitchcraftArtwork() {
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return false;
    CancelDisabledPending();
    const bool found = m_litchcraftArtworkState == BufferState::Pending;
    if (found) m_litchcraftArtworkState = BufferState::Writing;
    xSemaphoreGive(m_mutex);
    return found;
}

void SettingsPersistenceWorker::ReleaseLitchcraftArtwork() {
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return;
    m_litchcraftArtworkState = BufferState::Free;
    m_litchcraftArtworkBytes = nullptr;
    m_litchcraftArtworkSize = 0U;
    m_litchcraftArtworkDocument = nullptr;
    m_litchcraftArtworkEnvelope = nullptr;
    m_litchcraftArtworkActive = false;
    if (m_litchcraftArtworkOwnsDocument) {
        m_pendingStorage.artwork.~ArtworkDocument();
        new (&m_pendingStorage.settings) SettingsBuffers{};
        m_litchcraftArtworkOwnsDocument = false;
    }
    xSemaphoreGive(m_mutex);
}

bool SettingsPersistenceWorker::StartLitchcraftFrameStorage() {
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return false;
    if (m_litchcraftFrameStorageActive) {
        xSemaphoreGive(m_mutex);
        return true;
    }
    bool idle = m_litchcraftArtworkState == BufferState::Free &&
        m_litchcraftFrameResponseSize == 0U;
    // Only these buffers share the union. An admitted independent snapshot
    // must not prevent pending Play from establishing its paging resident.
    for (uint8_t index = 0U; index < SHARED_SETTINGS_BUFFER_COUNT; ++index) {
        idle = idle && m_bufferState[index] == BufferState::Free;
    }
    if (!idle) {
        xSemaphoreGive(m_mutex);
        return false;
    }
    const auto projection = m_litchcraftFrameBootstrapProjection;
    const uint32_t ownerEpoch = m_litchcraftFrameOwnerEpoch;
    m_pendingStorage.settings.~SettingsBuffers();
    new (&m_pendingStorage.frames) FrameTransferStorage(*this);
    m_litchcraftFrameStorageActive = true;
    m_litchcraftFrameActiveOwnerEpoch = ownerEpoch;
    m_litchcraftFrameLastActivityMs = fxe::Millis();
    xSemaphoreGive(m_mutex);

    auto& storage = m_pendingStorage.frames;
    storage.receiver.BindLiveStream(m_litchcraftFrameStream);
    storage.receiver.RestoreReceipt(m_litchcraftFrameReceipt);
    if (m_litchcraftFrameStream.InitialRecoveryPending()) {
        uint32_t unobserved = UINT32_MAX;
        (void)m_litchcraftStartupBeginMs.compare_exchange_strong(
            unobserved, fxe::Millis(), std::memory_order_acq_rel);
    }
    // A transient filesystem read must not leave saved artwork absent until a
    // phone request. Recovery remains on this worker and retries only I/O
    // failures; blank or corrupt roots have their own terminal meaning.
    for (uint8_t attempt = 0U; attempt < MAX_SAVE_ATTEMPTS; ++attempt) {
        using Purpose = modules::litchcraft_frame_store::FrameStore::RecoveryPurpose;
        // Restore durable mode authority before optional edit-summary work.
        // The first playback synchronization prepares the existing cache afterward.
        const Purpose purpose = Purpose::RootOnly;
        storage.recovery = m_source != nullptr && m_source->IsDisabled()
            ? storage.receiver.Recover(projection, purpose)
            : storage.receiver.RecoverOrCreateBlank(projection, purpose);
        if (storage.recovery !=
                modules::litchcraft_frame_store::Status::IoFailure ||
            attempt + 1U == MAX_SAVE_ATTEMPTS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    if (storage.recovery == modules::litchcraft_frame_store::Status::Ok ||
        storage.recovery == modules::litchcraft_frame_store::Status::NoArtwork) {
        const auto restored = RestoreLitchcraftFrameResident(storage.store);
        if (restored != modules::litchcraft_frame_store::Status::Ok) {
            storage.recovery = restored;
            // An ingress probe can precede mode projection. It still must not
            // enumerate the recovered wrong root as accepted board authority.
            m_litchcraftFrameModeSaveFailed.store(true, std::memory_order_release);
        }
    }
    m_litchcraftFrameLastRecovery.store(
        static_cast<uint8_t>(storage.recovery), std::memory_order_release);
    if (storage.recovery != modules::litchcraft_frame_store::Status::Ok) {
        m_litchcraftFrameStream.CompleteInitialRecovery();
        NoteStartupModeResolved();
    }
    if (projection.session != 0U && projection.active &&
        storage.recovery != modules::litchcraft_frame_store::Status::Ok) {
        m_litchcraftFrameModeSaveFailed.store(
            true, std::memory_order_release);
    }
    storage.receiver.SetCommitCallbacks(
        &SettingsPersistenceWorker::AdmitFrameStoreCommit,
        &SettingsPersistenceWorker::PublishFrameStoreCommit, this);
    if (storage.recovery == modules::litchcraft_frame_store::Status::Ok ||
        storage.recovery == modules::litchcraft_frame_store::Status::NoArtwork) {
        m_litchcraftFrameStream.BindRetirementGeneration(storage.store.Generation());
    }
    return true;
}

modules::litchcraft_frame_store::Status
SettingsPersistenceWorker::RestoreLitchcraftFrameResident(
    modules::litchcraft_frame_store::FrameStore& store) {
    using modules::litchcraft_frame_store::Status;
    const uint32_t liveGeneration = m_litchcraftFrameStream.Generation();
    if (liveGeneration == 0U) {
        // Clear/retirement deliberately ends the previous document authority.
        m_litchcraftFrameResident = {};
        return Status::Ok;
    }
    const auto& expected = m_litchcraftFrameResident;
    if (expected.generation == 0U) return Status::Ok;
    if (expected.generation != liveGeneration) return Status::RevisionConflict;
    modules::litchcraft_frame_store::FrameStore::ResidentDescriptor current{};
    if (store.CurrentResident(current) &&
        current.rootSlot == expected.rootSlot &&
        current.generation == expected.generation &&
        current.bodyCrc32 == expected.bodyCrc32) return Status::Ok;
    // Recover picks the newest durable root, whereas a runtime resident switch
    // may intentionally select an older root. Validate its exact proof before
    // any synchronization can substitute a different complete document.
    const auto selected = store.SelectResident(
        expected.rootSlot, expected.generation, expected.bodyCrc32);
    return selected == Status::NoArtwork ? Status::Corrupt : selected;
}

#if defined(LITCH_NATIVE)
bool SettingsPersistenceWorker::BlockLitchcraftFrameRootWritesForTest(
    const bool blocked) {
    if (m_mutex == nullptr ||
        xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool active = m_litchcraftFrameStorageActive;
    if (active) {
        m_pendingStorage.frames.file.BlockRootWritesForTest(blocked);
    }
    xSemaphoreGive(m_mutex);
    return active;
}
#endif

void SettingsPersistenceWorker::ReleaseLitchcraftFrameStorage() {
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return;
    const bool releasable = m_litchcraftFrameStorageActive &&
        m_litchcraftFrameDeferredOwnerEpoch == 0U &&
        !m_pendingStorage.frames.receiver.CommitPending() &&
        !m_pendingStorage.frames.receiver.ReadActive();
    xSemaphoreGive(m_mutex);
    if (!releasable) return;
    // Closing a cached LittleFS handle can wait for SDK/filesystem work.
    // Never hold the settings mutex through that wait.
    m_pendingStorage.frames.file.CloseCachedHandles();
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return;
    if (!m_litchcraftFrameStorageActive ||
        m_litchcraftFrameDeferredOwnerEpoch != 0U ||
        m_pendingStorage.frames.receiver.CommitPending() ||
        m_pendingStorage.frames.receiver.ReadActive()) {
        xSemaphoreGive(m_mutex);
        return;
    }
    // Capture only the root already owned by the live stream. This includes
    // successful edits and mode saves, but cannot bless an unadopted Commit.
    const uint32_t liveGeneration = m_litchcraftFrameStream.Generation();
    modules::litchcraft_frame_store::FrameStore::ResidentDescriptor resident{};
    if (liveGeneration == 0U) {
        m_litchcraftFrameResident = {};
    } else if (m_pendingStorage.frames.store.CurrentResident(resident) &&
               resident.generation == liveGeneration) {
        m_litchcraftFrameResident = {
            resident.generation, resident.bodyCrc32, resident.rootSlot};
    }
    m_litchcraftFrameReceipt = m_pendingStorage.frames.receiver.Receipt();
    m_pendingStorage.frames.~FrameTransferStorage();
    new (&m_pendingStorage.settings) SettingsBuffers{};
    m_litchcraftFrameStorageActive = false;
    m_litchcraftFrameBulkActive.store(false, std::memory_order_release);
    m_litchcraftFrameCommitPending.store(
        false, std::memory_order_release);
    m_litchcraftFrameActiveOwnerEpoch = 0U;
    m_litchcraftFrameLastActivityMs = 0U;
    m_litchcraftFrameTransferStartedMs = 0U;
    m_litchcraftFrameIngressPackets = 0U;
    m_litchcraftFrameTransferTimingActive = false;


    xSemaphoreGive(m_mutex);
}

modules::litchcraft_frame_store::PresentationAdmission
SettingsPersistenceWorker::AdmitFrameStoreCommit(
    void* const context, const uint32_t ownerEpoch,
    const uint32_t expectedGeneration,
    const modules::litchcraft_frame_store::Manifest& manifest,
    const uint32_t generation,
    const modules::litchcraft_frame_store::Frame&) {
    using Admission = modules::litchcraft_frame_store::PresentationAdmission;
    auto& self = *static_cast<SettingsPersistenceWorker*>(context);
    if (xSemaphoreTake(self.m_mutex, portMAX_DELAY) != pdTRUE) return {};
    const bool current = self.m_litchcraftFrameOwnerEpoch == ownerEpoch &&
        self.m_litchcraftFrameProjection.session != 0U &&
        self.m_litchcraftFramePrioritySize == 0U &&
        (self.m_litchcraftFrameDeferredOwnerEpoch == 0U ||
         (self.m_litchcraftFrameDeferredOwnerEpoch == ownerEpoch &&
          self.m_litchcraftFrameDeferredModeRevision == self.m_litchcraftFrameModeRevision));
    // A retired root retains its generation for durable ordering, while its
    // presentation stream is empty. Reserve that stream before root I/O so
    // concurrent Hall or app edits cannot be lost during finalization.
    const uint32_t presentationGeneration =
        self.m_pendingStorage.frames.store.HasArtwork()
            ? expectedGeneration : 0U;
    // Close the OTA barrier before the root or stream publishes replacement.
    // OTA polls atomics without taking this worker's mutex.
    if (current) {
        self.m_litchcraftFrameCommitPending.store(
            true, std::memory_order_release);
    }
    const Admission admission = current
        ? self.m_litchcraftFrameStream.TryReserveReplacement(
              presentationGeneration, manifest, generation)
        : Admission{};
    if (admission.kind == Admission::Kind::Accepted) {
        self.m_litchcraftFramePreviewPrevious =
            self.m_litchcraftFrameProjection;
        self.m_litchcraftFramePreviewExpected = {
            self.m_litchcraftFrameProjection.session, 0U,
            manifest.topology, manifest.active, false};
        self.m_litchcraftFramePreviewTransitionPending = true;
    } else if (current) {
        self.m_litchcraftFrameCommitPending.store(
            false, std::memory_order_release);
    }
    xSemaphoreGive(self.m_mutex);
    return admission;
}

bool SettingsPersistenceWorker::PublishFrameStoreCommit(
    void* const context, const uint32_t,
    const uint32_t,
    const modules::litchcraft_frame_store::Manifest& manifest,
    const uint32_t generation,
    const modules::litchcraft_frame_store::Frame& firstFrame) {
    auto& self = *static_cast<SettingsPersistenceWorker*>(context);
    if (xSemaphoreTake(self.m_mutex, portMAX_DELAY) != pdTRUE) return false;
    const uint32_t presentationGeneration =
        self.m_litchcraftFrameStream.Generation();
    const bool adopted = self.m_litchcraftFrameStream.PublishReservedReplacement(
        presentationGeneration, manifest, generation, firstFrame);
    if (adopted) {
        // The durable replacement supersedes terminal recovery failure for
        // both immediate publication and the receiver's pending Commit retry.
        self.m_pendingStorage.frames.recovery =
            modules::litchcraft_frame_store::Status::Ok;
        self.m_litchcraftFrameLastRecovery.store(
            static_cast<uint8_t>(modules::litchcraft_frame_store::Status::Ok),
            std::memory_order_release);
        self.m_litchcraftFramePlaybackPending.store(
            true, std::memory_order_release);
    }
    xSemaphoreGive(self.m_mutex);
    return adopted;
}

namespace {

void LogEditSyncTiming(const LitchcraftFrameFile::SyncTiming& sync,
                       const uint32_t generation) {
    if (sync.maximumPhase == UINT8_MAX) return;
    // Separate bounded records preserve the Logger's 255-byte payload limit.
    // This temporary probe only reads cached scalars after the save/flash lease.
    logger::Trace(
        "artwork sync: gen=%lu max_ms=%lu phase=%u logical_region=%u "
        "file_region=%u file_bytes=%lu cursor=%lu",
        static_cast<unsigned long>(generation),
        static_cast<unsigned long>(sync.maximumMs),
        static_cast<unsigned>(sync.maximumPhase),
        static_cast<unsigned>(sync.maximumRegion),
        static_cast<unsigned>(sync.maximumFileRegion),
        static_cast<unsigned long>(sync.maximumFileBytes),
        static_cast<unsigned long>(sync.maximumCursor));
    logger::Trace(
        "artwork sync: gen=%lu phases_ms_max_calls=%lu/%lu/%lu,%lu/%lu/%lu,%lu/%lu/%lu",
        static_cast<unsigned long>(generation),
        static_cast<unsigned long>(sync.phases[0U].totalMs),
        static_cast<unsigned long>(sync.phases[0U].maximumMs),
        static_cast<unsigned long>(sync.phases[0U].calls),
        static_cast<unsigned long>(sync.phases[1U].totalMs),
        static_cast<unsigned long>(sync.phases[1U].maximumMs),
        static_cast<unsigned long>(sync.phases[1U].calls),
        static_cast<unsigned long>(sync.phases[2U].totalMs),
        static_cast<unsigned long>(sync.phases[2U].maximumMs),
        static_cast<unsigned long>(sync.phases[2U].calls));
}

} // namespace

void SettingsPersistenceWorker::RecordFrameStoreTiming(
    void* const context,
    const modules::litchcraft_frame_store::FrameStore::TimingPhase phase,
    const uint32_t elapsedMs) {
    if (context == nullptr) return;
    auto& self = *static_cast<SettingsPersistenceWorker*>(context);
    using Phase = modules::litchcraft_frame_store::FrameStore::TimingPhase;
#if defined(LITCH_NATIVE)
    if (self.m_litchcraftFrameTimingTestHook != nullptr) {
        self.m_litchcraftFrameTimingTestHook(self.m_litchcraftFrameTimingTestContext, phase, elapsedMs);
    }
#endif
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
    if (phase == Phase::BodyWrite && self.m_ingressPerformanceSampling) {
        // The shared pass budget permits one slot. Publish this together with
        // its physical counters after the synchronous receiver call finishes.
        self.m_ingressPerformanceSlotWritten = true;
        self.m_ingressPerformanceSlotMs = elapsedMs;
    }
#endif
    if (self.m_litchcraftFrameEditTimingActive) {
        switch (phase) {
            case Phase::EditBegin:
                self.m_litchcraftFrameEditBeginMs = elapsedMs;
                self.m_pendingStorage.frames.file.SetEditTimingPhase(Phase::BodyPrepare);
                break;
            case Phase::BodyPrepare:
                self.m_litchcraftFrameEditPrepareMs = elapsedMs;
                self.m_pendingStorage.frames.file.SetEditTimingPhase(Phase::RootPublish);
                break;
            case Phase::RootPublish: self.m_litchcraftFrameEditPublishMs = elapsedMs; break;
            case Phase::BodyWrite: break;
        }
    }
    if (!self.m_litchcraftFrameTransferTimingActive) return;
    switch (phase) {
        case Phase::BodyWrite:
            self.m_frameStoreBodyWriteMs.fetch_add(
                elapsedMs, std::memory_order_acq_rel);
            break;
        case Phase::BodyPrepare:
            self.m_frameStoreBodyPrepareMs.store(
                elapsedMs, std::memory_order_release);
            break;
        case Phase::RootPublish:
            self.m_frameStoreRootPublishMs.store(
                elapsedMs, std::memory_order_release);
            break;
        case Phase::EditBegin:
            break;
    }
}

void SettingsPersistenceWorker::ResetFrameStoreTransferTiming() {
    m_frameStoreTransferMs.store(0U, std::memory_order_release);
    m_frameStoreBodyWriteMs.store(0U, std::memory_order_release);
    m_frameStoreBodyPrepareMs.store(0U, std::memory_order_release);
    m_frameStoreRootPublishMs.store(0U, std::memory_order_release);
    m_frameStoreBodyBytes.store(0U, std::memory_order_release);
    m_frameStoreIngressPackets.store(0U, std::memory_order_release);
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
        m_ingressPerformance = {};
        xSemaphoreGive(m_mutex);
    }
#endif
}

#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
bool SettingsPersistenceWorker::BeginIngressPerformanceSample(const uint32_t ownerEpoch) {
    // Worker-only scope. Never reset an existing settings transaction or leave
    // this active across an iteration, paging service, or another owner call.
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return false;
    const bool current = m_litchcraftFrameTransferTimingActive &&
        m_litchcraftFrameOwnerEpoch == ownerEpoch &&
        m_ingressPerformance.ownerEpoch == ownerEpoch;
    const bool busy = m_settingsPhysicalAccess.IsRecordingTransaction();
    if (current && busy) SettingsPhysicalAccessMicroCounter::Add(m_ingressPerformance.skipped, 1U);
    xSemaphoreGive(m_mutex);
    if (!current || busy) return false;
    m_ingressPerformanceSampling = true;
    m_ingressPerformanceSlotWritten = false;
    m_settingsPhysicalAccess.BeginTransaction();
    return true;
}

void SettingsPersistenceWorker::EndIngressPerformanceSample(
    const uint32_t ownerEpoch, const bool commit) {
    m_settingsPhysicalAccess.EndTransaction();
    m_ingressPerformanceSampling = false;
    const auto measured = m_settingsPhysicalAccess.TransactionSample();
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return;
    auto& result = m_ingressPerformance;
    if (m_litchcraftFrameOwnerEpoch == ownerEpoch && result.ownerEpoch == ownerEpoch) {
        const size_t kind = commit ? 1U : 0U;
        SettingsPhysicalAccessMicroCounter::Add(result.calls[kind], 1U);
        if (m_ingressPerformanceSlotWritten) {
            const uint64_t slot = uint64_t(result.slots[0U].calls) +
                result.slots[1U].calls + result.slots[2U].calls;
            auto& timing = result.slots[slot == 0U ? 0U :
                slot % LitchcraftFrameFile::FRAMES_PER_BODY_FILE == 0U ? 1U : 2U];
            SettingsPhysicalAccessMicroCounter::Add(timing.calls, 1U);
            SettingsPhysicalAccessMicroCounter::Add(timing.totalMs, m_ingressPerformanceSlotMs);
            timing.maximumMs = std::max(timing.maximumMs, m_ingressPerformanceSlotMs);
        }
        result.physicalAvailable = result.physicalAvailable || measured.available;
        auto& total = result.flash[kind];
        const auto& part = measured.work;
        SettingsPhysicalAccessMicroCounter::Add(total.outerCalls, part.outerCalls);
        SettingsPhysicalAccessMicroCounter::Add(total.outerWaitMs, part.outerWaitMs);
        SettingsPhysicalAccessMicroCounter::Add(total.handoffs, part.handoffs);
        SettingsPhysicalAccessMicroCounter::Add(total.handoffWaitMs, part.handoffWaitMs);
        for (size_t index = 0U; index < total.physical.size(); ++index) {
            auto& counter = total.physical[index];
            const auto& value = part.physical[index];
            SettingsPhysicalAccessMicroCounter::Add(counter.calls, value.calls);
            SettingsPhysicalAccessMicroCounter::Add(counter.bytes, value.bytes);
            SettingsPhysicalAccessMicroCounter::Add(counter.totalUs, value.totalUs);
            counter.maxUs = std::max(counter.maxUs, value.maxUs);
        }
    }
    xSemaphoreGive(m_mutex);
}
#endif

void SettingsPersistenceWorker::CompleteFrameStoreTransfer(
    modules::litchcraft_frame_store::FrameStore& store) {
    if (!m_litchcraftFrameTransferTimingActive) return;
    m_frameStoreTransferMs.store(
        fxe::Millis() - m_litchcraftFrameTransferStartedMs,
        std::memory_order_release);
    m_frameStoreBodyBytes.store(store.CurrentBodyBytes(),
                                 std::memory_order_release);
    m_frameStoreIngressPackets.store(m_litchcraftFrameIngressPackets,
                                     std::memory_order_release);
    m_litchcraftFrameTransferStartedMs = 0U;
    m_litchcraftFrameTransferTimingActive = false;
}

modules::litchcraft_frame_store::StoreIngressResult*
SettingsPersistenceWorker::AcquireLitchcraftFrameResponse() {
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return nullptr;
    modules::litchcraft_frame_store::StoreIngressResult* result = nullptr;
    if (m_litchcraftFramePrefetchCount < FRAME_RESPONSE_PREFETCH_CAPACITY) {
        const uint8_t tail = static_cast<uint8_t>(
            (m_litchcraftFramePrefetchHead + m_litchcraftFramePrefetchCount) %
            FRAME_RESPONSE_PREFETCH_CAPACITY);
        result = &m_litchcraftFramePrefetch[tail];
    }
    xSemaphoreGive(m_mutex);
    // Worker-only, synchronous lease: finish using this uncounted slot before
    // acquiring another. Concurrent drains preserve its tail index; owner or
    // priority changes retire visibility but never write the packet storage.
    return result;
}

bool SettingsPersistenceWorker::FillLitchcraftFrameReadResponses(
    modules::litchcraft_frame_store::StoreReceiver& receiver,
    modules::BleOtaChunkQueue& queue, const uint32_t ownerEpoch,
    const uint32_t projectionRevision) {
    bool produced = false;
    uint8_t prepared = 0U;
    // A concurrent BLE drain can reopen queue room. Still stop after one
    // credit window so playback and foreground storage get the next turn.
    uint8_t budget = FRAME_RESPONSE_PREFETCH_CAPACITY + 1U;
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return false;
    if (m_litchcraftFrameResponseSize != 0U) --budget;
    xSemaphoreGive(m_mutex);
    while (prepared < budget && receiver.ReadActive() &&
           !queue.TryPeekLitchcraftFrameStore()) {
        if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) break;
        const bool current = m_litchcraftFrameOwnerEpoch == ownerEpoch &&
            m_litchcraftFrameProjectionRevision.load(
                std::memory_order_acquire) == projectionRevision &&
            m_litchcraftFramePrioritySize == 0U;
        const bool room = m_litchcraftFramePrefetchCount <
            FRAME_RESPONSE_PREFETCH_CAPACITY;
        const uint8_t tail = static_cast<uint8_t>(
            (m_litchcraftFramePrefetchHead + m_litchcraftFramePrefetchCount) %
            FRAME_RESPONSE_PREFETCH_CAPACITY);
        xSemaphoreGive(m_mutex);
        if (!current || !room) break;

        auto& result = m_litchcraftFramePrefetch[tail];
        const bool savedRead = receiver.SavedReadActive();
        if (savedRead) BeginFrameReadDiagnostic(false);
        const auto readElapsed = fxe::ElapsedTime::StartNow();
        receiver.ContinueRead(ownerEpoch, result);
        if (savedRead) CompleteFrameReadDiagnostic(result, readElapsed.ElapsedMs());
        if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) break;
        const bool stillCurrent = m_litchcraftFrameOwnerEpoch == ownerEpoch &&
            m_litchcraftFrameProjectionRevision.load(
                std::memory_order_acquire) == projectionRevision &&
            m_litchcraftFramePrioritySize == 0U;
        if (stillCurrent && result.responseReady) {
            if (m_litchcraftFrameResponseSize == 0U) {
                std::copy_n(result.response.data(), result.responseSize,
                            m_litchcraftFrameResponse.data());
                m_litchcraftFrameResponseOwnerEpoch = ownerEpoch;
                m_litchcraftFrameResponseProjectionRevision =
                    projectionRevision;
                m_litchcraftFrameResponseSize = result.responseSize;
            } else {
                // Draining advances head and reduces count by the same amount,
                // so the borrowed uncounted tail stays put until publication.
                ++m_litchcraftFramePrefetchCount;
            }
            produced = true;
            ++prepared;
        }
        if (stillCurrent) {
            m_litchcraftFrameIngressPending =
                receiver.ReadActive() || queue.TryPeekLitchcraftFrameStore();
        }
        xSemaphoreGive(m_mutex);
        if (!stillCurrent || !result.responseReady) break;
    }
    return produced;
}

void SettingsPersistenceWorker::RefreshLitchcraftBulkActivity() {
    // Sole storage-worker read. Publish retirement after its prepared response
    // is counted, closing the final-chunk handoff without a false idle edge.
    m_litchcraftFrameBulkActive.store(m_litchcraftFrameStorageActive &&
        (m_pendingStorage.frames.receiver.TransferActive() ||
         m_pendingStorage.frames.receiver.ReadActive()), std::memory_order_release);
}

bool SettingsPersistenceWorker::IsLitchcraftBulkTransferActive() {
    if (m_litchcraftFrameBulkActive.load(std::memory_order_acquire)) return true;
    if (m_mutex == nullptr || xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return false;
    const bool current = m_litchcraftFrameResponseOwnerEpoch == m_litchcraftFrameOwnerEpoch &&
        m_litchcraftFrameResponseProjectionRevision ==
            m_litchcraftFrameProjectionRevision.load(std::memory_order_acquire);
    bool active = false;
    if (current) {
        active = m_litchcraftFrameResponseSize > 2U &&
            m_litchcraftFrameResponse[2U] == modules::litchcraft_frame_store::STORE_SNAPSHOT_CHUNK;
        for (uint8_t offset = 0U; offset < m_litchcraftFramePrefetchCount; ++offset) {
            const auto& response = m_litchcraftFramePrefetch[
                (m_litchcraftFramePrefetchHead + offset) % FRAME_RESPONSE_PREFETCH_CAPACITY];
            active = active || (response.responseSize > 2U &&
                response.response[2U] == modules::litchcraft_frame_store::STORE_SNAPSHOT_CHUNK);
        }
    }
    xSemaphoreGive(m_mutex);
    return active || m_litchcraftFrameBulkActive.load(std::memory_order_acquire);
}

bool SettingsPersistenceWorker::ServiceLitchcraftFrameIngress() {
    const FrameActivityPublication activity{*this};
    modules::BleOtaChunkQueue* queue = nullptr;
    modules::litchcraft_frame_store::StoreProjection projection{};
    uint32_t ownerEpoch = 0U;
    bool active = false;
    std::array<uint8_t, 9U> priority{};
    size_t prioritySize = 0U;
    uint32_t projectionRevision = 0U;
    uint32_t modeRevision = 0U;
    uint32_t activeOwnerEpoch = 0U;
    bool expired = false;
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return false;
    const uint32_t nowMs = fxe::Millis();
    active = m_litchcraftFrameStorageActive;
    queue = m_litchcraftFrameQueue;
    activeOwnerEpoch = m_litchcraftFrameActiveOwnerEpoch;
    // Quiet playback owns resident storage without ingress activity. Only an
    // actual transfer/read may expire that residency; pause releases it through
    // the existing playback owner after its pending work converges.
    expired = active && !m_pendingStorage.frames.receiver.CommitPending() &&
        (!m_litchcraftFramePlaybackResident.load(std::memory_order_acquire) ||
         m_pendingStorage.frames.receiver.TransferActive() ||
         m_pendingStorage.frames.receiver.ReadActive()) &&
        static_cast<uint32_t>(nowMs - m_litchcraftFrameLastActivityMs) >=
            LITCHCRAFT_FRAME_TRANSFER_TIMEOUT_MS;
    if (expired) {
        m_litchcraftFrameResponseSize = 0U;
        m_litchcraftFrameLastResponseSize = 0U;
        m_litchcraftFramePrefetchCount = 0U;
        m_litchcraftFramePrefetchHead = 0U;
        m_litchcraftFrameResponseOwnerEpoch = 0U;
        m_litchcraftFrameResponseProjectionRevision = 0U;
        m_litchcraftFramePrioritySize = 0U;
        m_litchcraftFrameDiscardSession = 0U;
        m_litchcraftFrameDiscardTransfer = 0U;
        m_litchcraftFrameDiscardOwnerEpoch = 0U;
    }
    xSemaphoreGive(m_mutex);
    if (expired) {
        m_pendingStorage.frames.receiver.EndOwner(activeOwnerEpoch);
        if (m_litchcraftFrameDeferredOwnerEpoch != 0U && queue != nullptr) {
            const uint8_t* bytes = nullptr;
            size_t size = 0U;
            uint32_t ordinal = 0U;
            if (queue->TryAcquireReadSlot(bytes, size, ordinal)) {
                if (ordinal == m_litchcraftFrameDeferredOrdinal) queue->ReleaseReadSlot();
                else queue->DeferReadSlot();
            }
            m_litchcraftFrameDeferredOrdinal = 0U;
            m_litchcraftFrameDeferredOwnerEpoch = 0U;
            m_litchcraftFrameDeferredModeRevision = 0U;
        }
        ReleaseLitchcraftFrameStorage();
        if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
            m_litchcraftFrameIngressPending =
                m_litchcraftFrameQueue != nullptr &&
                m_litchcraftFrameQueue->TryPeekLitchcraftFrameStore();
            xSemaphoreGive(m_mutex);
        }
        return true;
    }
    if (active && m_pendingStorage.frames.receiver.CommitPending() &&
        static_cast<int32_t>(fxe::Millis() -
                             m_litchcraftFrameNextCommitRetryMs) >= 0) {
        auto& receiver = m_pendingStorage.frames.receiver;
        const uint32_t pendingOwner = receiver.PendingOwnerEpoch();
        auto* const responseSlot = AcquireLitchcraftFrameResponse();
        if (responseSlot == nullptr) return false;
        auto& committed = *responseSlot;
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
        const bool measuredCommit = BeginIngressPerformanceSample(pendingOwner);
#endif
        receiver.RetryPendingCommit(committed);
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
        if (measuredCommit) EndIngressPerformanceSample(pendingOwner, true);
#endif
        if (committed.responseReady) {
            CompleteFrameStoreTransfer(m_pendingStorage.frames.store);
            m_litchcraftFrameCommitPending.store(
                false, std::memory_order_release);
            logger::Info("frame_store: prepared root publication recovered");
            m_litchcraftFrameNextCommitRetryMs = 0U;
            m_litchcraftFrameCommitRetryDelayMs = 100U;
            SynchronizeLitchcraftFramePlayback(m_pendingStorage.frames.store);
            if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
                const auto& receipt = receiver.Receipt();
                if (m_litchcraftFrameOwnerEpoch == pendingOwner &&
                    m_litchcraftFrameProjection.session == receipt.session) {
                    m_litchcraftFramePrefetchCount = 0U;
                    m_litchcraftFramePrefetchHead = 0U;
                    m_litchcraftFrameResponseSize = committed.responseSize;
                    std::copy_n(committed.response.data(),
                                committed.responseSize,
                                m_litchcraftFrameResponse.data());
                    m_litchcraftFrameResponseOwnerEpoch = pendingOwner;
                    m_litchcraftFrameResponseProjectionRevision =
                        m_litchcraftFrameProjectionRevision.load(
                            std::memory_order_acquire);
                }
                xSemaphoreGive(m_mutex);
            }
        } else {
            m_litchcraftFrameNextCommitRetryMs = fxe::Millis() +
                m_litchcraftFrameCommitRetryDelayMs;
            m_litchcraftFrameCommitRetryDelayMs = std::min<uint32_t>(
                m_litchcraftFrameCommitRetryDelayMs * 2U, 5000U);
        }
    }
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return false;
    if (m_litchcraftFrameResponseSize != 0U) {
        xSemaphoreGive(m_mutex);
        return false;
    }
    queue = m_litchcraftFrameQueue;
    projection = m_litchcraftFrameProjection;
    ownerEpoch = m_litchcraftFrameOwnerEpoch;
    const uint16_t packetMaximum = m_litchcraftFramePacketMaximum;
    active = m_litchcraftFrameStorageActive;
    prioritySize = m_litchcraftFramePrioritySize;
    if (prioritySize != 0U) {
        std::copy_n(m_litchcraftFramePriority.data(), prioritySize,
                    priority.data());
    }
    projectionRevision =
        m_litchcraftFrameProjectionRevision.load(std::memory_order_acquire);
    modeRevision = m_litchcraftFrameModeRevision;
    xSemaphoreGive(m_mutex);
    if (queue == nullptr) return false;
    if (prioritySize == 0U &&
        m_litchcraftFrameModeSavePending.load(std::memory_order_acquire) &&
        !m_litchcraftFrameModeSaveFailed.load(std::memory_order_acquire) &&
        (!active || !m_pendingStorage.frames.receiver.TransferActive())) {
        // Inspect must observe the root after Enter/Exit publication. A
        // snapshot based on the prior generation would immediately conflict.
        return false;
    }


    if (!active && prioritySize == 0U &&
        !queue->TryPeekLitchcraftFrameStore()) {
        if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
            m_litchcraftFrameIngressPending = false;
            xSemaphoreGive(m_mutex);
        }
        return false;
    }
    if (!active && !StartLitchcraftFrameStorage()) return false;
    if (!active &&
        (m_pendingStorage.frames.recovery ==
             modules::litchcraft_frame_store::Status::Ok ||
         m_pendingStorage.frames.recovery ==
             modules::litchcraft_frame_store::Status::NoArtwork)) {
        // Ingress can outrun playback recovery. Commit admission compares
        // the stream with the durable root recovered by Start above.
        SynchronizeLitchcraftFramePlayback(m_pendingStorage.frames.store);
    }

    auto& receiver = m_pendingStorage.frames.receiver;
    receiver.SetSnapshotPacketMaximum(packetMaximum);
    if (m_litchcraftFrameActiveOwnerEpoch != ownerEpoch) {
        receiver.EndOwner(m_litchcraftFrameActiveOwnerEpoch);
        m_litchcraftFrameActiveOwnerEpoch = ownerEpoch;
    }
    receiver.SetProjection(projection);

    if (prioritySize != 0U) {
        auto* const responseSlot = AcquireLitchcraftFrameResponse();
        if (responseSlot == nullptr) return false;
        auto& result = *responseSlot;
        bool cancelDeferredSelection = false;
        if (!receiver.TransferActive() &&
            m_litchcraftFrameDeferredOwnerEpoch == ownerEpoch) {
            const uint8_t* bytes = nullptr;
            size_t size = 0U;
            uint32_t ordinal = 0U, packetEpoch = 0U;
            if (queue->TryAcquireReadSlot(bytes, size, ordinal, packetEpoch)) {
                cancelDeferredSelection = ordinal == m_litchcraftFrameDeferredOrdinal &&
                    packetEpoch == queue->CurrentEpoch() && size == 18U &&
                    bytes[2U] == modules::litchcraft_frame_store::STORE_SELECT &&
                    fxe::ReadU32LE(bytes + 3U) == fxe::ReadU32LE(priority.data() + 3U) &&
                    fxe::ReadU16LE(bytes + 7U) == fxe::ReadU16LE(priority.data() + 7U);
                if (!cancelDeferredSelection) queue->DeferReadSlot();
            }
        }
        receiver.Handle(priority.data(), prioritySize, ownerEpoch, result,
                        nullptr, cancelDeferredSelection);
        if (cancelDeferredSelection) {
            if (result.responseReady && result.response[13U] == 0U) {
                queue->ReleaseReadSlot();
                m_litchcraftFrameDeferredOrdinal = 0U;
                m_litchcraftFrameDeferredOwnerEpoch = 0U;
                m_litchcraftFrameDeferredModeRevision = 0U;
            } else {
                queue->DeferReadSlot();
            }
        }
        if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
            const bool stillCurrent =
                m_litchcraftFrameOwnerEpoch == ownerEpoch &&
                m_litchcraftFrameProjectionRevision.load(
                    std::memory_order_acquire) == projectionRevision;
            if (stillCurrent) {
                m_litchcraftFramePrioritySize = 0U;
                m_litchcraftFrameLastActivityMs = fxe::Millis();
            }
            if (stillCurrent && result.responseReady) {
                m_litchcraftFrameLastResponseSize = 0U;
                m_litchcraftFramePrefetchCount = 0U;
                m_litchcraftFramePrefetchHead = 0U;
                m_litchcraftFrameResponseSize = result.responseSize;
                std::copy_n(result.response.data(), result.responseSize,
                            m_litchcraftFrameResponse.data());
                m_litchcraftFrameResponseOwnerEpoch = ownerEpoch;
                m_litchcraftFrameResponseProjectionRevision =
                    projectionRevision;
            }
            if (stillCurrent && result.responseReady &&
                result.response[13] == 0U) {
                m_litchcraftFrameDiscardSession =
                    fxe::ReadU32LE(priority.data() + 3U);
                m_litchcraftFrameDiscardTransfer =
                    fxe::ReadU16LE(priority.data() + 7U);
                m_litchcraftFrameDiscardOwnerEpoch = ownerEpoch;
            }
            if (stillCurrent) {
                m_litchcraftFrameIngressPending =
                    queue->TryPeekLitchcraftFrameStore();
            }
            xSemaphoreGive(m_mutex);
        }
        if (!receiver.TransferActive()) {
            ReleaseLitchcraftFrameStorage();
        }
        return true;
    }

    uint16_t frameBudget = 1U;
    const auto continueBody = [&] {
        if (!receiver.BodyPending() || frameBudget == 0U ||
            xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return;
        const bool current = m_litchcraftFrameOwnerEpoch == ownerEpoch &&
            m_litchcraftFrameProjectionRevision.load(std::memory_order_acquire) == projectionRevision &&
            m_litchcraftFramePrioritySize == 0U;
        xSemaphoreGive(m_mutex);
        if (!current) return;
        const uint32_t stagedBefore = receiver.StagedBodyCursor();
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
        m_liveIngressHandle.store(((fxe::Millis() & 0x00ffffffU) << 8U) |
            modules::litchcraft_frame_store::STORE_CHUNK | 0x20U, std::memory_order_release);
        const bool measuredBody = BeginIngressPerformanceSample(ownerEpoch);
#endif
        receiver.ContinueBody(frameBudget);
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
        if (measuredBody) EndIngressPerformanceSample(ownerEpoch, false);
        m_liveIngressHandle.store(0U, std::memory_order_release);
#endif
        if (receiver.StagedBodyCursor() != stagedBefore &&
            xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
            if (m_litchcraftFrameOwnerEpoch == ownerEpoch &&
                m_litchcraftFrameProjectionRevision.load(std::memory_order_acquire) == projectionRevision)
                m_litchcraftFrameLastActivityMs = fxe::Millis();
            xSemaphoreGive(m_mutex);
        }
    };
    continueBody();
    if (receiver.ReadActive() && !queue->TryPeekLitchcraftFrameStore()) {
        const bool produced = FillLitchcraftFrameReadResponses(
            receiver, *queue, ownerEpoch, projectionRevision);
        if (!receiver.ReadActive() && !receiver.TransferActive() &&
            !m_litchcraftFramePlaybackResident.load(
                std::memory_order_acquire) &&
            !m_litchcraftFramePlaybackPending.load(
                std::memory_order_acquire)) {
            ReleaseLitchcraftFrameStorage();
        }
        return produced;
    }

    bool consumed = false;
    bool rootChanged = false;
    bool presentationDeferred = false;
    // Empty pending storage guarantees room for a whole maximum-size Chunk
    // before claiming it; Checkpoint/Commit keep their FIFO position meanwhile.
    for (uint8_t count = 0U;
         count < modules::BleOtaChunkQueue::CAPACITY &&
         frameBudget != 0U && !receiver.BodyPending() &&
         queue->TryPeekLitchcraftFrameStore(); ++count) {
        if (m_litchcraftFrameProjectionRevision.load(
                std::memory_order_acquire) != projectionRevision) {
            break;
        }
        auto* const responseSlot = AcquireLitchcraftFrameResponse();
        if (responseSlot == nullptr) break;
        const uint8_t* bytes = nullptr;
        size_t size = 0U;
        uint32_t ordinal = 0U;
        uint32_t packetEpoch = 0U;
        if (!queue->TryAcquireReadSlot(
                bytes, size, ordinal, packetEpoch)) {
            break;
        }
        auto& result = *responseSlot;
        result.Reset();
        const bool currentQueueEpoch = packetEpoch == queue->CurrentEpoch();
        const bool discarded = currentQueueEpoch && size >= 9U &&
            m_litchcraftFrameDiscardOwnerEpoch == ownerEpoch &&
            fxe::ReadU32LE(bytes + 3U) == m_litchcraftFrameDiscardSession &&
            fxe::ReadU16LE(bytes + 7U) == m_litchcraftFrameDiscardTransfer &&
            ((bytes[2] >= modules::litchcraft_frame_store::STORE_CHUNK &&
              bytes[2] <= modules::litchcraft_frame_store::STORE_COMMIT) ||
             bytes[2] == modules::litchcraft_frame_store::STORE_SELECT);
        if (!discarded && currentQueueEpoch) {
            m_litchcraftFrameDiscardSession = 0U;
            m_litchcraftFrameDiscardTransfer = 0U;
            m_litchcraftFrameDiscardOwnerEpoch = 0U;
            auto& store = m_pendingStorage.frames.store;
            const uint8_t operation = size >= 3U ? bytes[2] : 0U;
            const bool transferWasActive = receiver.TransferActive();
            const bool selectingResident = size >= 3U &&
                bytes[2] == modules::litchcraft_frame_store::STORE_SELECT;
            modules::litchcraft_frame_store::FrameStore::ResidentDescriptor
                previousResident{};
            const bool hadPreviousResident = selectingResident &&
                store.CurrentResident(previousResident);
            // Corrupt metadata remains intact until an explicit validated
            // replacement. Let the receiver expose absence and fence that
            // transaction; a transient recovery/root-write fault still fails.
            const bool corruptWithoutArtwork = !store.HasArtwork() &&
                m_pendingStorage.frames.recovery ==
                    modules::litchcraft_frame_store::Status::Corrupt;
            const bool redispatched = m_litchcraftFrameDeferredOwnerEpoch != 0U &&
                m_litchcraftFrameDeferredOrdinal == ordinal;
            const bool needsPresentation =
                (operation == modules::litchcraft_frame_store::STORE_SELECT && size == 18U) ||
                (operation == modules::litchcraft_frame_store::STORE_COMMIT && size == 13U);
            bool selectionRestored = false;
            if (needsPresentation && m_litchcraftFrameDeferredOwnerEpoch == 0U) {
                // Latch before the first admission too: Exit or Cancel may
                // arrive between this pass's snapshot and the owner lock.
                m_litchcraftFrameDeferredOrdinal = ordinal;
                m_litchcraftFrameDeferredOwnerEpoch = ownerEpoch;
                m_litchcraftFrameDeferredModeRevision = modeRevision;
            }
            if (needsPresentation && !redispatched &&
                xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
                if (m_litchcraftFrameOwnerEpoch == ownerEpoch) {
                    // A fresh request begins its own existing inactivity lease;
                    // polling the retained head must not extend that lease.
                    m_litchcraftFrameLastActivityMs = fxe::Millis();
                }
                xSemaphoreGive(m_mutex);
            }
            if (m_litchcraftFrameDeferredOwnerEpoch != 0U &&
                ordinal == m_litchcraftFrameDeferredOrdinal &&
                (ownerEpoch != m_litchcraftFrameDeferredOwnerEpoch ||
                 modeRevision != m_litchcraftFrameDeferredModeRevision)) {
                // Physical Exit/session change retires this original request,
                // even before BLE has supplied its next projection.
                receiver.EndOwner(ownerEpoch);
                receiver.RejectResidentPresentation(
                    projection, fxe::ReadU16LE(bytes + 7U),
                    modules::litchcraft_frame_store::WireStatus::StaleSession,
                    result);
            } else if (size >= 9U && ((m_source != nullptr && m_source->IsDisabled()) ||
                (m_litchcraftFrameModeSaveFailed.load(std::memory_order_acquire) &&
                 !corruptWithoutArtwork))) {
                receiver.Unavailable(fxe::ReadU16LE(bytes + 7U), result);
            } else {
                const bool savedRead = operation == modules::litchcraft_frame_store::STORE_SNAPSHOT &&
                    (size == 17U || size == 19U);
                if (savedRead) {
                    logger::Info("storage_read_start: kind=%s request=%u generation=%lu cursor=%lu",
                        size == 19U ? "saved_frame" : "saved_body", unsigned(fxe::ReadU16LE(bytes + 7U)),
                        static_cast<unsigned long>(fxe::ReadU32LE(bytes + 9U)),
                        static_cast<unsigned long>(fxe::ReadU32LE(bytes + 13U)));
                    BeginFrameReadDiagnostic(true, size == 19U);
                }
                const auto readElapsed = fxe::ElapsedTime::StartNow();
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
                m_liveIngressHandle.store(((fxe::Millis() & 0x00ffffffU) << 8U) |
                    (operation & 31U) | 0x20U, std::memory_order_release);
                const bool measuredCall = (operation == modules::litchcraft_frame_store::STORE_CHUNK ||
                    operation == modules::litchcraft_frame_store::STORE_COMMIT) &&
                    BeginIngressPerformanceSample(ownerEpoch);
#endif
                receiver.Handle(bytes, size, ownerEpoch, result, &frameBudget);
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
                if (measuredCall) EndIngressPerformanceSample(ownerEpoch,
                    operation == modules::litchcraft_frame_store::STORE_COMMIT);
                m_liveIngressHandle.store(0U, std::memory_order_release);
#endif
                if (savedRead) CompleteFrameReadDiagnostic(result, readElapsed.ElapsedMs());
            }
            if (operation == modules::litchcraft_frame_store::STORE_COMMIT &&
                !receiver.CommitPending()) {
                m_litchcraftFrameCommitPending.store(
                    false, std::memory_order_release);
            }
            if (operation == modules::litchcraft_frame_store::STORE_BEGIN &&
                !transferWasActive && receiver.TransferActive()) {
                ResetFrameStoreTransferTiming();
#if defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE)
                if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
                    if (m_litchcraftFrameOwnerEpoch == ownerEpoch) {
                        m_ingressPerformance.ownerEpoch = ownerEpoch;
                        m_ingressPerformance.request = fxe::ReadU16LE(bytes + 7U);
                    }
                    xSemaphoreGive(m_mutex);
                }
#endif
                m_litchcraftFrameTransferStartedMs = fxe::Millis();
                m_litchcraftFrameIngressPackets = 1U;
                m_litchcraftFrameTransferTimingActive = true;
            } else if (m_litchcraftFrameTransferTimingActive && !redispatched) {
                ++m_litchcraftFrameIngressPackets;
            }
            if (receiver.CommitPending() &&
                m_litchcraftFrameNextCommitRetryMs == 0U) {
                logger::Error("frame_store: accepted root publication pending retry");
                m_litchcraftFrameNextCommitRetryMs = fxe::Millis() +
                    m_litchcraftFrameCommitRetryDelayMs;
            }
            bool residentSelected = size >= 3U &&
                bytes[2] ==
                    modules::litchcraft_frame_store::STORE_SELECT &&
                result.responseReady && result.response[13U] == 0U;
            if (residentSelected) {
                // SelectResident decoded frame zero before changing its root.
                // Keep the old displayed slot live until this complete frame
                // and its authority are published together.
                using Admission = modules::litchcraft_frame_store::PresentationAdmission;
                Admission admission{};
                if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
                    const bool current = m_litchcraftFrameOwnerEpoch == ownerEpoch &&
                        (m_litchcraftFrameDeferredOwnerEpoch == 0U ||
                         (m_litchcraftFrameDeferredOwnerEpoch == ownerEpoch &&
                          m_litchcraftFrameDeferredModeRevision == m_litchcraftFrameModeRevision)) &&
                        m_litchcraftFramePrioritySize == 0U;
                    if (current) {
                        admission = m_litchcraftFrameStream.TryStageFirstFrame(
                            store.CurrentManifest(), store.Generation(), store.DecodedFrame());
                    }
                    xSemaphoreGive(m_mutex);
                }
                if (admission.kind == Admission::Kind::Accepted) {
                    m_litchcraftFramePlaybackPending.store(
                        true, std::memory_order_release);
                } else {
                    selectionRestored = hadPreviousResident &&
                        (store.Generation() == previousResident.generation ||
                         store.UndoResidentSelection(previousResident));
                    if (!selectionRestored) {
                        logger::Error(
                            "frame_store: failed to restore prior resident after presentation rejection");
                    }
                    receiver.RejectResidentPresentation(
                        projection, fxe::ReadU16LE(bytes + 7U),
                        admission.dirty
                            ? modules::litchcraft_frame_store::WireStatus::Busy
                            : modules::litchcraft_frame_store::WireStatus::ResourceLimit,
                        result);
                    if (selectionRestored && admission.kind == Admission::Kind::PendingPresentation) {
                        // Restore the old root/projection before retaining the
                        // same queue head. No selected authority leaks early.
                        result.Reset();
                    } else if (admission.dirty) {
                        m_litchcraftFramePlaybackPending.store(
                            true, std::memory_order_release);
                    }
                    residentSelected = false;
                }
            }
            if (needsPresentation && !residentSelected && !receiver.CommitPending() &&
                (selectionRestored || receiver.TransferActive()) &&
                xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
                const bool preempted = m_litchcraftFramePrioritySize != 0U &&
                    m_litchcraftFrameOwnerEpoch == ownerEpoch &&
                    m_litchcraftFrameDeferredOwnerEpoch == ownerEpoch &&
                    m_litchcraftFrameDeferredModeRevision == m_litchcraftFrameModeRevision;
                xSemaphoreGive(m_mutex);
                if (preempted) {
                    // Priority is a scheduling decision, distinct from the
                    // stream's Show-only admission result. Keep an unrelated
                    // Cancel from consuming this still-valid original head.
                    result.Reset();
                }
            }
            rootChanged = residentSelected ||
                (operation == modules::litchcraft_frame_store::STORE_COMMIT &&
                 result.responseReady && result.response[13] == 0U);
            if (operation == modules::litchcraft_frame_store::STORE_COMMIT &&
                result.responseReady && result.responseSize >= 14U &&
                result.response[13] == 0U) {
                CompleteFrameStoreTransfer(store);
            }
        }
        if (!result.handled && !receiver.CommitPending() &&
            !discarded && currentQueueEpoch) {
            // The borrowed bytes cease to be owned here. The existing FIFO
            // retains the request and its credit until Show resolves.
            queue->DeferReadSlot();
            presentationDeferred = true;
            break;
        }
        m_litchcraftFrameDeferredOrdinal = 0U;
        m_litchcraftFrameDeferredOwnerEpoch = 0U;
        m_litchcraftFrameDeferredModeRevision = 0U;
        queue->ReleaseReadSlot();
        // The entire packet is now retained in the existing Store workspace.
        // Release FIFO credit before a bounded frame slice touches storage.
        continueBody();
        consumed = true;
        if (currentQueueEpoch &&
            xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
            if (m_litchcraftFrameOwnerEpoch == ownerEpoch) {
                m_litchcraftFrameLastActivityMs = fxe::Millis();
            }
            xSemaphoreGive(m_mutex);
        }
        if (!result.responseReady) continue;

        if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
            const bool stillCurrent =
                m_litchcraftFrameOwnerEpoch == ownerEpoch &&
                m_litchcraftFrameProjectionRevision.load(
                    std::memory_order_acquire) == projectionRevision &&
                m_litchcraftFramePrioritySize == 0U;
            if (stillCurrent) {
                m_litchcraftFrameLastResponseSize = 0U;
                m_litchcraftFrameResponseSize = result.responseSize;
                std::copy_n(result.response.data(), result.responseSize,
                            m_litchcraftFrameResponse.data());
                m_litchcraftFrameResponseOwnerEpoch = ownerEpoch;
                m_litchcraftFrameResponseProjectionRevision =
                    projectionRevision;
                m_litchcraftFrameIngressPending =
                    queue->TryPeekLitchcraftFrameStore() ||
                    receiver.ReadActive();
            }
            xSemaphoreGive(m_mutex);
        }
        break;
    }

    if (receiver.ReadActive() && !queue->TryPeekLitchcraftFrameStore()) {
        consumed = FillLitchcraftFrameReadResponses(
            receiver, *queue, ownerEpoch, projectionRevision) || consumed;
    }

    // Releasing the shared storage union destroys receiver. Retain only the
    // small scheduling facts needed after that lifetime boundary.
    const bool readActive = receiver.ReadActive();
    const bool bodyPending = receiver.BodyPending();
    if (!presentationDeferred && !receiver.TransferActive() && !readActive) {
        if (rootChanged) {
            SynchronizeLitchcraftFramePlayback(
                m_pendingStorage.frames.store);
        }
        // Snapshot/Inspect shares the resident Store with streamed playback.
        // Releasing it here reconstructs FrameStore on the next playback wake
        // and invalidates an in-flight multi-slice decode cursor. Keep the
        // union resident while playback owns it or has work queued; the
        // playback service releases it itself after pause/idle convergence.
        if (!m_litchcraftFramePlaybackResident.load(
                std::memory_order_acquire) &&
            !m_litchcraftFramePlaybackPending.load(
                std::memory_order_acquire)) {
            ReleaseLitchcraftFrameStorage();
        }
    }
    bool moreIngress = false;
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
        const bool current = m_litchcraftFrameOwnerEpoch == ownerEpoch &&
            m_litchcraftFrameProjectionRevision.load(
                std::memory_order_acquire) == projectionRevision;
        if (current) {
            // Buffered output postpones the next worker run, not the read.
            // Its final acceptance must still wake the remaining suffix.
            const bool pending = queue->TryPeekLitchcraftFrameStore() || readActive || bodyPending;
            m_litchcraftFrameIngressPending = pending;
            moreIngress = pending && m_litchcraftFrameResponseSize == 0U;
        }
        xSemaphoreGive(m_mutex);
    }
    if (moreIngress && !presentationDeferred && m_task != nullptr) {
        // A 64-slot queue can refill while this bounded service pass drains
        // its current 64 packets. The BLE callback may already have consumed
        // its notification during that pass, so keep draining immediately
        // instead of falling back to the 100 ms worker timeout.
        xTaskNotifyGive(m_task);
    }
    return consumed;
}

bool SettingsPersistenceWorker::ServiceLitchcraftFrameClear() {
    auto pending = m_litchcraftFrameClearState.load(std::memory_order_acquire);
    if (pending != FrameClearState::Pending &&
        pending != FrameClearState::PrelaunchLC5Pending) return false;
    const bool prelaunchLC5 = pending == FrameClearState::PrelaunchLC5Pending;
    if (m_source != nullptr && m_source->IsDisabled()) {
        if (m_litchcraftFrameClearState.compare_exchange_strong(
                pending, prelaunchLC5 ? FrameClearState::Idle : FrameClearState::Failed,
                std::memory_order_acq_rel)) {
            m_litchcraftFrameModeSavePending.store(false, std::memory_order_release);
            m_litchcraftFrameModeSaveFailed.store(true, std::memory_order_release);
        }
        return false;
    }
    if (!m_litchcraftFrameClearState.compare_exchange_strong(
            pending, FrameClearState::Running, std::memory_order_acq_rel)) return false;
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) {
        m_litchcraftFrameClearState.store(pending, std::memory_order_release);
        return false;
    }
    const auto mode = m_litchcraftFrameBootstrapProjection;
    const uint32_t modeRevision = m_litchcraftFrameModeRevision;
    auto* const queue = m_litchcraftFrameClearQueue;
    const uint32_t queueEpoch = m_litchcraftFrameClearQueueEpoch;
    m_litchcraftFrameResponseSize = 0U;
    m_litchcraftFrameLastResponseSize = 0U;
    m_litchcraftFramePrefetchCount = 0U;
    m_litchcraftFramePrefetchHead = 0U;
    m_litchcraftFramePrioritySize = 0U;
    m_litchcraftFrameIngressPending.store(false, std::memory_order_release);
    m_litchcraftFrameReceipt = {};
    m_litchcraftFramePreviewTransitionPending = false;
    m_litchcraftFrameDiscardSession = 0U;
    m_litchcraftFrameDiscardTransfer = 0U;
    m_litchcraftFrameDiscardOwnerEpoch = 0U;
    xSemaphoreGive(m_mutex);
    if (queue != nullptr) (void)queue->AdvanceEpochIfCurrent(queueEpoch);
    if (!StartLitchcraftFrameStorage()) {
        m_litchcraftFrameClearState.store(pending, std::memory_order_release);
        return false;
    }
    auto& storage = m_pendingStorage.frames;
    storage.receiver.EndOwner(m_litchcraftFrameActiveOwnerEpoch);
    storage.receiver.RestoreReceipt({});
    ResetFrameStoreTransferTiming();
    // LC5 artwork is disposable only for this unreleased transition. Remove
    // known artwork files through their existing owner before normal retirement
    // and blank-root initialization; no settings files belong to this adapter.
    auto status = prelaunchLC5 && !storage.file.Remove()
        ? modules::litchcraft_frame_store::Status::IoFailure
        : storage.store.Clear(m_litchcraftFrameStream.RetiredGeneration());
    if (status == modules::litchcraft_frame_store::Status::Ok) {
        if (m_litchcraftFrameStream.AdvanceDurableRetirementGeneration(
                storage.store.Generation())) {
            auto paused = mode;
            paused.playing = false;
            status = storage.receiver.RecoverOrCreateBlank(paused);
        } else {
            status = modules::litchcraft_frame_store::Status::RevisionConflict;
        }
    }
    bool durable = status == modules::litchcraft_frame_store::Status::Ok ||
        status == modules::litchcraft_frame_store::Status::NoArtwork;
    if (durable) {
        SynchronizeLitchcraftFramePlayback(storage.store);
        if (mode.active &&
            (!storage.store.HasArtwork() ||
             m_litchcraftFrameStream.Generation() != storage.store.Generation() ||
             m_litchcraftFrameStream.FrameCount() != 1U)) {
            status = modules::litchcraft_frame_store::Status::Incomplete;
            durable = false;
        }
    }
    storage.recovery = status;
    m_litchcraftFrameLastRecovery.store(static_cast<uint8_t>(status),
                                        std::memory_order_release);
    m_litchcraftFrameNextSaveRetryMs = 0U;
    m_litchcraftFrameSaveRetryDelayMs = 100U;
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
        if (durable && m_litchcraftFrameModeRevision == modeRevision) {
            m_litchcraftFrameModeSavePending.store(false, std::memory_order_release);
            m_litchcraftFrameModeSaveFailed.store(false, std::memory_order_release);
        } else if (!durable && m_litchcraftFrameModeRevision == modeRevision) {
            m_litchcraftFrameModeSaveFailed.store(true, std::memory_order_release);
            m_litchcraftFramePlaybackPending.store(false, std::memory_order_release);
        }
        xSemaphoreGive(m_mutex);
    }
    m_litchcraftFrameClearState.store(
        prelaunchLC5 ? FrameClearState::Idle
            : durable ? FrameClearState::Succeeded : FrameClearState::Failed,
        std::memory_order_release);
    return true;
}

void SettingsPersistenceWorker::NoteStartupModeResolved() {
    if (m_litchcraftFrameStream.InitialRecoveryPending() ||
        m_litchcraftStartupBeginMs.load(std::memory_order_acquire) == UINT32_MAX) return;
    uint32_t unobserved = UINT32_MAX;
    (void)m_litchcraftStartupModeResolvedMs.compare_exchange_strong(
        unobserved, fxe::Millis(), std::memory_order_acq_rel);
}

void SettingsPersistenceWorker::SynchronizeLitchcraftFramePlayback(
    modules::litchcraft_frame_store::FrameStore& store) {
    auto& stream = m_litchcraftFrameStream;
    if (stream.HasPendingBlankDocument() || stream.HasPendingStructuralSave()) return;
    if (!store.HasArtwork() || store.Generation() == 0U) {
        if (stream.Generation() != 0U || stream.FrameCount() != 0U) {
            stream.Clear();
        }
        return;
    }
    if (stream.Generation() != store.Generation() ||
        stream.FrameCount() != store.CurrentManifest().frameCount) {
#if defined(LITCH_NATIVE)
        if (m_litchcraftFrameBeforeConfigureTestHook != nullptr) {
            m_litchcraftFrameBeforeConfigureTestHook(
                m_litchcraftFrameBeforeConfigureTestContext);
        }
#endif
        if (!stream.Configure(store.CurrentManifest(), store.Generation())) {
            // New may publish after the first pending check, before Configure
            // takes its publication lock. Its refusal must retain New's wake.
            if (stream.HasPendingBlankDocument() || stream.HasPendingStructuralSave()) return;
            m_litchcraftFramePlaybackPending.store(false,
                                                   std::memory_order_release);
            stream.CompleteInitialRecovery();
            NoteStartupModeResolved();
            return;
        }
        NoteStartupModeResolved();
        m_litchcraftFramePlaybackPending.store(true,
                                               std::memory_order_release);
    }
}

bool SettingsPersistenceWorker::ServiceLitchcraftFramePlayback() {
    const FrameActivityPublication activity{*this};
    if (m_source != nullptr && m_source->IsDisabled()) {
        return ServiceDisabledLitchcraftFramePlayback();
    }
    using Stream = modules::litchcraft_frame_stream::FrameStream;
    if (!m_litchcraftFrameRuntimeReady.load(std::memory_order_acquire) ||
        !m_litchcraftFramePlaybackPending.load(std::memory_order_acquire)) {
        return false;
    }
    uint32_t servicingModeRevision = 0U;
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
        servicingModeRevision = m_litchcraftFrameModeRevision;
        xSemaphoreGive(m_mutex);
    }
    if (m_litchcraftFrameNextSaveRetryMs != 0U &&
        static_cast<int32_t>(fxe::Millis() -
                             m_litchcraftFrameNextSaveRetryMs) < 0) {
        return false;
    }
    if (m_litchcraftFrameStream.HasUnpersistedEdit() &&
        !m_litchcraftFrameStream.EditSaveReady(fxe::Millis()) &&
        !m_litchcraftFramePlaybackResident.load(std::memory_order_acquire)) {
        // A paused brush stays in RAM without holding a Store/flash lease.
        // Active playback can still prefetch into the other clean cache slots.
        return false;
    }
    if (!m_litchcraftFramePlaybackResident.load(std::memory_order_acquire) &&
        (m_litchcraftFrameIngressPending.load(std::memory_order_acquire) ||
         m_litchcraftFrameResponseSize.load(std::memory_order_acquire) != 0U) &&
        !m_litchcraftFrameModeSavePending.load(std::memory_order_acquire) &&
        !m_litchcraftFrameStream.HasPendingBlankDocument()) {
        // A paused or static preview can wait for foreground control. Live
        // animation must keep paging even if an upload or receipt is pending.
        return false;
    }
    if (!m_litchcraftFrameStorageActive &&
        !StartLitchcraftFrameStorage()) {
        return false;
    }
    auto& storage = m_pendingStorage.frames;
    if (storage.recovery == modules::litchcraft_frame_store::Status::PrelaunchLC5 &&
        m_litchcraftFrameModeSavePending.load(std::memory_order_acquire) &&
        !storage.receiver.TransferActive() && !storage.receiver.CommitPending() &&
        m_litchcraftFrameStream.Generation() == 0U &&
        m_litchcraftFrameStream.FrameCount() == 0U &&
        !m_litchcraftFrameStream.HasPendingBlankDocument() &&
        RequestLitchcraftFrameClear(FrameClearState::PrelaunchLC5Pending)) {
        return true;
    }
    if (m_litchcraftFrameStream.HasPendingBlankDocument() &&
        storage.receiver.TransferActive() && !storage.receiver.CommitPending()) {
        // A live New has superseded this connection's unfinished upload.
        // Retire it through the existing receiver before using its Store scratch.
        storage.receiver.EndOwner(m_litchcraftFrameActiveOwnerEpoch);
    }
    const bool structuralRecovery = m_litchcraftFrameStream.HasPendingStructuralSave();
    if (!structuralRecovery && !storage.store.HasArtwork() && !storage.receiver.TransferActive() &&
        !storage.receiver.CommitPending()) {
        modules::litchcraft_frame_store::StoreProjection bootstrap{};
        if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
            bootstrap = m_litchcraftFrameBootstrapProjection;
            xSemaphoreGive(m_mutex);
        }
        if (bootstrap.session != 0U && bootstrap.active &&
            m_litchcraftFrameModeSavePending.load(std::memory_order_acquire)) {
            storage.recovery = storage.receiver.RecoverOrCreateBlank(bootstrap);
            m_litchcraftFrameLastRecovery.store(
                static_cast<uint8_t>(storage.recovery), std::memory_order_release);
            if (storage.recovery ==
                modules::litchcraft_frame_store::Status::Ok) {
                m_litchcraftFrameStream.BindRetirementGeneration(
                    storage.store.Generation());
            } else {
                m_litchcraftFrameModeSaveFailed.store(
                    true, std::memory_order_release);
            }
        }
    }
    if (storage.receiver.CommitPending()) {
        // Root publication and first-frame adoption form one worker-owned
        // transition. Keep mode saves and paging off this prepared transaction
        // until its durable root and live frame have both settled.
        return false;
    }
    if (storage.receiver.TransferActive() &&
        (!m_litchcraftFramePlaybackResident.load(std::memory_order_acquire) ||
         !storage.store.HasArtwork() ||
         m_litchcraftFrameStream.HasUnpersistedEdit() ||
         m_litchcraftFrameStream.Generation() !=
             storage.store.Generation())) {
        // Only a clean, already committed resident may page alongside an
        // incoming body. A blank board or a physical edit must keep the
        // transaction intact until its own authority transition resolves.
        return false;
    }
    if (!storage.store.HasArtwork() &&
        storage.recovery == modules::litchcraft_frame_store::Status::NoArtwork &&
        xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
        // Enter may be cancelled before its first root exists. No root is
        // already the durable inactive state; do not retry a nonexistent save.
        if (m_litchcraftFrameBootstrapProjection.session != 0U &&
            !m_litchcraftFrameBootstrapProjection.active) {
            m_litchcraftFrameModeSavePending.store(
                false, std::memory_order_release);
            m_litchcraftFrameModeSaveFailed.store(
                false, std::memory_order_release);
        }
        xSemaphoreGive(m_mutex);
    }
    if (!structuralRecovery &&
        (storage.recovery == modules::litchcraft_frame_store::Status::Ok ||
         storage.recovery == modules::litchcraft_frame_store::Status::NoArtwork)) {
#if defined(LITCH_NATIVE)
        if (m_litchcraftFrameBeforeSyncTestHook != nullptr) {
            m_litchcraftFrameBeforeSyncTestHook(
                m_litchcraftFrameBeforeSyncTestContext);
        }
#endif
        const bool initialRecovery = m_litchcraftFrameStream.InitialRecoveryPending();
        SynchronizeLitchcraftFramePlayback(storage.store);
        if (initialRecovery && !m_litchcraftFrameStream.InitialRecoveryPending() &&
            m_litchcraftFrameStream.Generation() == storage.store.Generation()) {
            // Essential mode/root authority is published first. A bound cache
            // can load cheaply while full repair waits for boot acceptance.
            m_litchcraftFrameSummaryMaintenancePending = storage.store.EnsureWireSummaries(false) !=
                    modules::litchcraft_frame_store::Status::Ok;
        }
    }
    bool hadPending = false;
    bool superseded = false;
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
        hadPending = m_litchcraftFramePlaybackPending.exchange(
            false, std::memory_order_acq_rel);
        superseded = m_litchcraftFrameModeRevision != servicingModeRevision;
        if (superseded) {
            // NotifySession may have published a new Enter while this slice
            // was reading the old empty Store. Keep its wake and mode barrier.
            m_litchcraftFramePlaybackPending.store(
                true, std::memory_order_release);
        }
        xSemaphoreGive(m_mutex);
    }
    if (superseded) {
        ReleaseLitchcraftFrameStorage();
        return true;
    }
    if (!hadPending) {
        ReleaseLitchcraftFrameStorage();
        return false;
    }
    if (m_litchcraftFrameSummaryMaintenancePending && storage.store.HasArtwork() &&
        !m_litchcraftFramePlaybackResident.load(std::memory_order_acquire) &&
        storage.recovery == modules::litchcraft_frame_store::Status::Ok &&
        m_litchcraftFrameSummaryMaintenanceReady.load(std::memory_order_acquire) &&
        !storage.receiver.TransferActive() && !storage.receiver.CommitPending() &&
        !m_litchcraftFrameStream.HasUnpersistedEdit()) {
        const auto result = storage.store.EnsureWireSummaries(true, [](void* p) {
            const auto& self = *static_cast<SettingsPersistenceWorker*>(p);
            return self.m_litchcraftFrameSummaryMaintenanceReady.load(std::memory_order_acquire) &&
                !self.m_litchcraftFramePlaybackResident.load(std::memory_order_acquire) &&
                !self.m_litchcraftFrameIngressPending.load(std::memory_order_acquire) &&
                !self.m_litchcraftFrameModeSavePending.load(std::memory_order_acquire) &&
                !self.m_litchcraftFrameStream.HasUnpersistedEdit() &&
                !self.m_litchcraftFrameStream.HasPendingBlankDocument() &&
                self.m_storageReformatState.load() == StorageReformatState::Idle &&
                self.m_litchcraftFrameClearState.load() == FrameClearState::Idle &&
                self.m_lastRequestedGeneration.load() == self.m_lastCompletedGeneration.load();
        }, this);
        m_litchcraftFrameSummaryMaintenancePending =
            result == modules::litchcraft_frame_store::Status::Incomplete;
        if (m_litchcraftFrameSummaryMaintenancePending)
            m_litchcraftFramePlaybackPending.store(true, std::memory_order_release);
    }
    if (!structuralRecovery && (!storage.store.HasArtwork() ||
        (storage.recovery != modules::litchcraft_frame_store::Status::Ok &&
         storage.recovery !=
             modules::litchcraft_frame_store::Status::NoArtwork))) {
        if (m_litchcraftFrameModeSavePending.load(
                std::memory_order_acquire)) {
            m_litchcraftFrameModeSaveFailed.store(
                true, std::memory_order_release);
            m_litchcraftFramePlaybackPending.store(
                true, std::memory_order_release);
            m_litchcraftFrameNextSaveRetryMs = fxe::Millis() +
                m_litchcraftFrameSaveRetryDelayMs;
            m_litchcraftFrameSaveRetryDelayMs = std::min<uint32_t>(
                m_litchcraftFrameSaveRetryDelayMs * 2U, 1000U);
        }
        ReleaseLitchcraftFrameStorage();
        return false;
    }
    const bool editPendingBeforeService =
        m_litchcraftFrameStream.HasUnpersistedEdit();
    const auto frameServiceElapsed = fxe::ElapsedTime::StartNow();
    // Ingress is serviced on both sides of this bounded playback slice. An
    // open upload or an undelivered response must not starve local animation.
    const auto serviceMode = m_litchcraftFrameStream.EditSaveReady(fxe::Millis())
        ? Stream::ServiceMode::PagingAndEdits : Stream::ServiceMode::PagingOnly;
    m_litchcraftFrameEditBeginMs = UINT32_MAX;
    m_litchcraftFrameEditPrepareMs = 0U;
    m_litchcraftFrameEditPublishMs = 0U;
    m_litchcraftFrameEditTimingActive = editPendingBeforeService &&
        serviceMode == Stream::ServiceMode::PagingAndEdits;
    if (m_litchcraftFrameEditTimingActive) storage.file.ResetAccessTiming();
    const auto prefetch = m_litchcraftFramePlaybackSuspended.load(std::memory_order_acquire)
        ? Stream::PrefetchPolicy::None
        : m_litchcraftFramePlaybackResident.load(std::memory_order_acquire)
        ? Stream::PrefetchPolicy::TwoAhead : Stream::PrefetchPolicy::Next;
    const auto result = m_litchcraftFrameStream.Service(storage.store, false, serviceMode, prefetch);
    m_litchcraftFrameEditTimingActive = false;
    if (m_litchcraftFrameEditBeginMs != UINT32_MAX) {
        const auto& access = storage.file.GetAccessTiming();
        // One record after the save attempt, never within a flash lease or
        // scratch-live checksum phase. Existing transfer metrics stay separate.
        logger::Trace(
            "artwork save: edit_save_ms=%lu result=%u body_bytes=%lu generation=%lu "
            "phase_ms=%lu/%lu/%lu io_max=%lu/%u/%lu/%lu lease_max=%lu io_calls=%lu",
            static_cast<unsigned long>(frameServiceElapsed.ElapsedMs()),
            static_cast<unsigned>(result),
            static_cast<unsigned long>(storage.store.CurrentBodyBytes()),
            static_cast<unsigned long>(storage.store.Generation()),
            static_cast<unsigned long>(m_litchcraftFrameEditBeginMs),
            static_cast<unsigned long>(m_litchcraftFrameEditPrepareMs),
            static_cast<unsigned long>(m_litchcraftFrameEditPublishMs),
            static_cast<unsigned long>(access.maximumMs),
            static_cast<unsigned>(access.maximumOperation),
            static_cast<unsigned long>(access.maximumBytes),
            static_cast<unsigned long>(access.maximumOffset),
            static_cast<unsigned long>(access.maximumLeaseWaitMs),
            static_cast<unsigned long>(access.accesses));
        LogEditSyncTiming(access.sync, storage.store.Generation());
    }
    if (result == Stream::ServiceResult::IoFailure &&
        serviceMode == Stream::ServiceMode::PagingAndEdits &&
        editPendingBeforeService &&
        m_litchcraftFrameStream.HasUnpersistedEdit()) {
        logger::Warn("artwork: save failed; changes remain pending for retry");
        // The selected frame is still pending behind a dirty resident slot.
        // A transient flash fault must not consume its only worker wake.
        m_litchcraftFrameSaveFailureCount.fetch_add(
            1U, std::memory_order_acq_rel);
        m_litchcraftFramePlaybackPending.store(true,
                                              std::memory_order_release);
        m_litchcraftFrameNextSaveRetryMs = fxe::Millis() +
            m_litchcraftFrameSaveRetryDelayMs;
        m_litchcraftFrameSaveRetryDelayMs = std::min<uint32_t>(
            m_litchcraftFrameSaveRetryDelayMs * 2U, 1000U);
        ReleaseLitchcraftFrameStorage();
        return false;
    }
    m_litchcraftFrameNextSaveRetryMs = 0U;
    m_litchcraftFrameSaveRetryDelayMs = 100U;
    modules::litchcraft_frame_store::StoreProjection mode{};
    uint32_t modeRevision = 0U;
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
        mode = m_litchcraftFrameBootstrapProjection;
        modeRevision = m_litchcraftFrameModeRevision;
        xSemaphoreGive(m_mutex);
    }
    if (mode.session != 0U && storage.store.HasArtwork()) {
        if (storage.store.CurrentManifest().active != mode.active) {
            if (m_litchcraftFrameStream.HasUnpersistedEdit()) {
                m_litchcraftFramePlaybackPending.store(
                    true, std::memory_order_release);
            } else {
                const auto saved = m_litchcraftFrameStream.SaveModeRoot(
                    storage.store, mode.active);
                if (saved != modules::litchcraft_frame_store::Status::Ok) {
                    m_litchcraftFrameModeSaveFailed.store(
                        true, std::memory_order_release);
                    m_litchcraftFramePlaybackPending.store(
                        true, std::memory_order_release);
                    m_litchcraftFrameNextSaveRetryMs = fxe::Millis() + 100U;
                    return false;
                }
            }
        }
        if (storage.store.CurrentManifest().active == mode.active &&
            xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
            if (m_litchcraftFrameModeRevision == modeRevision &&
                m_litchcraftFrameBootstrapProjection.session == mode.session &&
                m_litchcraftFrameBootstrapProjection.active == mode.active) {
                m_litchcraftFrameModeSavePending.store(
                    false, std::memory_order_release);
                m_litchcraftFrameModeSaveFailed.store(
                    false, std::memory_order_release);
            }
            xSemaphoreGive(m_mutex);
        }
    }
    switch (result) {
        case Stream::ServiceResult::Loading:
        case Stream::ServiceResult::LoadedRequested:
        case Stream::ServiceResult::PrefetchedNext:
            m_litchcraftFramePlaybackPending.store(true,
                                                   std::memory_order_release);
            return true;
        case Stream::ServiceResult::DeferredForControl:
            m_litchcraftFramePlaybackPending.store(true,
                                                   std::memory_order_release);
            return false;
        case Stream::ServiceResult::WaitingForPresentation:
            // Render swaps the already loaded frame. The regular worker tick
            // resumes the trailing save after that visible boundary.
            m_litchcraftFramePlaybackPending.store(true,
                                                   std::memory_order_release);
            return false;
        case Stream::ServiceResult::Idle:
            if (m_litchcraftFramePlaybackResident.load(
                    std::memory_order_acquire)) {
                return false;
            }
            ReleaseLitchcraftFrameStorage();
            return false;
        case Stream::ServiceResult::Corrupt:
        case Stream::ServiceResult::IoFailure:
            if (!storage.receiver.TransferActive()) {
                ReleaseLitchcraftFrameStorage();
            }
            return m_litchcraftFramePlaybackPending.load(
                std::memory_order_acquire);
    }
    return false;
}

void SettingsPersistenceWorker::ServiceLitchcraftFramePagingDuringSettings() {
    using Stream = modules::litchcraft_frame_stream::FrameStream;
    if (!m_litchcraftFrameStorageActive ||
        !m_litchcraftFrameRuntimeReady.load(std::memory_order_acquire) ||
        !m_litchcraftFramePlaybackResident.load(std::memory_order_acquire)) {
        return;
    }
    auto& storage = m_pendingStorage.frames;
    if (storage.receiver.CommitPending() || !storage.store.HasArtwork() ||
        m_litchcraftFrameStream.Generation() != storage.store.Generation()) {
        return;
    }
    // The settings lease has ended. Reuse the resident pager sequentially,
    // without synchronizing authority or persisting New, edits or mode roots.
    // Its three cache slots and existing read cursor retain the same bounds.
    const bool timing = m_settingsPhysicalAccess.IsRecordingTransaction();
    const auto elapsed = fxe::ElapsedTime::StartNow();
    if (timing) m_settingsPhysicalAccess.SetTransactionPaging(true);
    const auto result = m_litchcraftFrameStream.Service(
        storage.store, false, Stream::ServiceMode::PagingOnly,
        m_litchcraftFramePlaybackSuspended.load(std::memory_order_acquire)
            ? Stream::PrefetchPolicy::None
            : m_litchcraftFramePlaybackResident.load(std::memory_order_acquire)
            ? Stream::PrefetchPolicy::TwoAhead : Stream::PrefetchPolicy::Next);
    if (timing) {
        m_settingsPhysicalAccess.SetTransactionPaging(false);
        SettingsPhysicalAccessMicroCounter::Add(m_activePersistenceTiming.pagingCalls, 1U);
        SettingsPhysicalAccessMicroCounter::Add(m_activePersistenceTiming.pagingMs, elapsed.ElapsedMs());
    }
    if (result != Stream::ServiceResult::Idle) {
        m_litchcraftFramePlaybackPending.store(true, std::memory_order_release);
    }
}

void SettingsPersistenceWorker::ServiceLitchcraftFramePlaybackSlice() {
    // Playback recovery is incremental and lower priority than ingress. One
    // bounded storage step per slice keeps BLE control and bulk packets
    // serviceable while the worker continues itself without timer pacing.
    if (!m_litchcraftFramePlaybackPending.load(std::memory_order_acquire)) {
        return;
    }
    const bool more = ServiceLitchcraftFramePlayback();
    if (more && m_task != nullptr) {
        xTaskNotifyGive(m_task);
    }
}

void SettingsPersistenceWorker::RunSettingsReadDiagnostic() {
    uint8_t state = m_storageReadState.load(std::memory_order_acquire);
    if ((state == 5U || state == 11U) &&
        (!m_litchcraftFrameStorageActive ||
         !m_pendingStorage.frames.receiver.SavedReadActive())) {
        if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
            m_storageReadState.store(state == 11U ? 10U : 8U, std::memory_order_release);
            xSemaphoreGive(m_mutex);
        }
        logger::Info("storage_read: kind=saved_store complete=0 reason=retired file_bytes=%lu local_ms=%lu",
            static_cast<unsigned long>(m_storageReadBytes.load()),
            static_cast<unsigned long>(m_storageReadWorkerMs.load()));
        return;
    }
    if (state != 1U || xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return;
    // Own no source snapshot or buffer while reading. Pending saves retain
    // priority; this request cannot publish a false Saved receipt.
    if (m_lastRequestedGeneration.load() != m_lastCompletedGeneration.load() ||
        m_receiptGeneration.load() != 0U) {
        xSemaphoreGive(m_mutex);
        return;
    }
    m_storageReadState.store(2U, std::memory_order_release);
    xSemaphoreGive(m_mutex);
    const auto elapsed = fxe::ElapsedTime::StartNow();
    size_t bytes = 0U;
    uint32_t hash = 0U;
    m_activePersistenceTiming = {};
    m_settingsOperationActiveGeneration = UINT32_MAX; // Diagnostic, never a save receipt.
    m_settingsPhysicalAccess.BeginTransaction();
    const bool ok = m_writer.ReadPrimaryForDiagnostics(bytes, hash);
    m_settingsPhysicalAccess.EndTransaction();
    m_settingsOperationActiveGeneration = 0U;
    const auto driver = m_settingsPhysicalAccess.TransactionSample();
    const uint32_t localMs = elapsed.ElapsedMs();
    m_writer.Doc().clear();
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
        m_storageReadBytes.store(static_cast<uint32_t>(bytes));
        m_storageReadWorkerMs.store(localMs);
        m_storageReadFileMs.store(0U); // No invented parse/file-time split.
        m_storageReadHash.store(hash);
        m_storageReadCalls.store(0U);
        m_storageReadState.store(ok ? 3U : 4U, std::memory_order_release);
        xSemaphoreGive(m_mutex);
    }
    logger::Info("storage_read: kind=settings_primary complete=%u bytes=%lu local_ms=%lu fnv32=%08lx source=primary_file validation=json_object runtime_applied=0",
        unsigned(ok), static_cast<unsigned long>(bytes), static_cast<unsigned long>(localMs),
        static_cast<unsigned long>(hash));
    const auto& read = driver.work.physical[0U];
    logger::Info("storage_read_driver: kind=settings_primary available=%u read_calls=%lu read_bytes=%lu read_us=%lu read_max_us=%lu outer_ms=%lu inner_ms=%lu",
        unsigned(driver.available), static_cast<unsigned long>(read.calls),
        static_cast<unsigned long>(read.bytes), static_cast<unsigned long>(read.totalUs),
        static_cast<unsigned long>(read.maxUs), static_cast<unsigned long>(driver.work.outerWaitMs),
        static_cast<unsigned long>(driver.work.handoffWaitMs));
}

void SettingsPersistenceWorker::BeginFrameReadDiagnostic(const bool reset,
                                                         const bool singleFrame) {
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return;
    const uint8_t state = m_storageReadState.load();
    if (reset && state != 1U && state != 2U) {
        m_storageReadState.store(singleFrame ? 12U : 6U, std::memory_order_release);
        m_storageReadBytes.store(0U); m_storageReadWorkerMs.store(0U);
        m_storageReadFileMs.store(0U); m_storageReadHash.store(0U);
        m_storageReadCalls.store(0U);
    } else if (!reset && (state == 5U || state == 11U)) {
        m_storageReadState.store(state == 11U ? 12U : 6U, std::memory_order_release);
    }
    xSemaphoreGive(m_mutex);
}

void SettingsPersistenceWorker::RecordFrameReadAccess(
    const LitchcraftFrameFile::AccessOperation operation,
    const size_t bytes, const uint32_t elapsedMs) {
    const uint8_t state = m_storageReadState.load();
    if ((state != 6U && state != 12U) || operation != LitchcraftFrameFile::AccessOperation::Read) return;
    m_storageReadBytes.fetch_add(static_cast<uint32_t>(bytes));
    m_storageReadFileMs.fetch_add(elapsedMs);
    m_storageReadCalls.fetch_add(1U);
}

void SettingsPersistenceWorker::CompleteFrameReadDiagnostic(
    const modules::litchcraft_frame_store::StoreIngressResult& result,
    const uint32_t elapsedMs) {
    const uint8_t state = m_storageReadState.load();
    if (state != 6U && state != 12U) return;
    m_storageReadWorkerMs.fetch_add(elapsedMs);
    const bool snapshot = result.responseReady && result.responseSize >= 60U &&
        result.response[2U] == modules::litchcraft_frame_store::STORE_SNAPSHOT_CHUNK;
    const bool ok = snapshot && result.response[13U] == 0U;
    const uint32_t total = snapshot ? fxe::ReadU32LE(result.response.data() + 20U) : 0U;
    const uint32_t through = snapshot ? fxe::ReadU32LE(result.response.data() + 28U) +
        static_cast<uint32_t>(result.responseSize - 60U) : 0U;
    const bool complete = ok && total != 0U && through >= total;
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return;
    if (snapshot) m_storageReadHash.store(fxe::ReadU32LE(result.response.data() + 24U));
    m_storageReadState.store(complete || !ok
        ? (state == 12U ? (ok ? 9U : 10U) : (ok ? 7U : 8U))
        : (state == 12U ? 11U : 5U), std::memory_order_release);
    xSemaphoreGive(m_mutex);
    if (complete || !ok) {
        logger::Info("storage_read: kind=%s complete=%u request=%u generation=%lu body_bytes=%lu through_cursor=%lu file_bytes=%lu local_ms=%lu file_ms=%lu file_calls=%lu crc32=%08lx",
            state == 12U ? "saved_frame" : "saved_body", unsigned(complete),
            result.responseReady && result.responseSize >= 13U ? unsigned(fxe::ReadU16LE(result.response.data() + 11U)) : 0U,
            result.responseReady && result.responseSize >= 11U ? static_cast<unsigned long>(fxe::ReadU32LE(result.response.data() + 7U)) : 0UL,
            static_cast<unsigned long>(total), static_cast<unsigned long>(through),
            static_cast<unsigned long>(m_storageReadBytes.load()),
            static_cast<unsigned long>(m_storageReadWorkerMs.load()),
            static_cast<unsigned long>(m_storageReadFileMs.load()),
            static_cast<unsigned long>(m_storageReadCalls.load()),
            static_cast<unsigned long>(m_storageReadHash.load()));
        logger::Info("storage_read_source: request=%u generation=%lu source=saved_slots cache=%s scope=requested_cursor_suffix excludes=radio_idle,paging",
            result.responseReady && result.responseSize >= 13U ? unsigned(fxe::ReadU16LE(result.response.data() + 11U)) : 0U,
            result.responseReady && result.responseSize >= 11U ? static_cast<unsigned long>(fxe::ReadU32LE(result.response.data() + 7U)) : 0UL,
            m_storageReadCalls.load() == 0U ? "wire_record" : "file_and_wire_record");
    }
}
#endif

bool SettingsPersistenceWorker::PrepareBuffer(
    const uint8_t index,
    bool& outTransactionInvoked) {
    outTransactionInvoked = false;
    m_snapshotStorageError = fxe::SettingsStoreError::None;
    auto& doc = m_writer.Doc();
    doc.clear();
    // The log-before-settings callback reuses the serialized input buffer as
    // bounded scratch. Own all decoded keys and values in the existing JSON
    // pool first; writable-input parsing would leave live strings in that buffer.
    const auto error = deserializeJson(
        doc, static_cast<const char*>(SettingsBuffer(index)));
    if (error) {
        m_snapshotStorageError = fxe::SettingsStoreError::ParseFailed;
        logger::Warn("settings_io: snapshot parse failed: %s", error.c_str());
        return false;
    }

    if (m_receiptGeneration.load(std::memory_order_acquire) == m_bufferGeneration[index] &&
        m_trackedPrerequisite != nullptr) {
        // The prerequisite owns its actual flash lease and durability result.
        if (!m_trackedPrerequisite(m_trackedPrerequisiteContext)) return false;
    }

    const uint32_t incomingTotal =
        doc[fxe::SETTINGS_SAVE_TOTAL_KEY] | 0U;
    const uint32_t persistedTotal =
        m_persistedSaveTotal.load(std::memory_order_acquire);
    doc[fxe::SETTINGS_SAVE_TOTAL_KEY] =
        incomingTotal > persistedTotal ? incomingTotal : persistedTotal;
    m_activeBufferIndex = index;
    if (m_prepareSnapshotCallback != nullptr) {
        outTransactionInvoked = true;
        m_snapshotTransactionActive = true;
        const bool prepared = m_prepareSnapshotCallback(
            m_snapshotTransactionContext, doc);
        m_snapshotTransactionActive = false;
        return prepared;
    }
    return true;
}

bool SettingsPersistenceWorker::SavePreparedBuffer(const uint8_t index) {
    m_activeBufferIndex = index;
    m_settingsOperationActiveGeneration = m_bufferGeneration[index];
    m_snapshotStorageError = fxe::SettingsStoreError::None;
    if (m_supportRuntimeStats != nullptr &&
        !m_supportRuntimeStats->PrepareSave(m_writer.Doc(), modules::SupportUptimeMs())) {
        m_snapshotStorageError = fxe::SettingsStoreError::PreparationFailed;
        m_settingsOperationActiveGeneration = 0U;
        return false;
    }
    const bool saved = m_writer.SaveNowBlocking();
    m_settingsOperationActiveGeneration = 0U;
    return saved;
}

void SettingsPersistenceWorker::WaitForSettingsRetry() {
#if defined(LITCH_ESP32S3)
    // Preserve the existing retry delay while render requests keep changing
    // the resident page. A full 250 ms sleep would exhaust a 50 ms animation's
    // cache even when every filesystem access releases storage correctly.
    const uint32_t yieldTicks = std::max<uint32_t>(pdMS_TO_TICKS(5U), 1U);
    for (uint32_t remaining = pdMS_TO_TICKS(RETRY_DELAY_MS); remaining != 0U;) {
        const uint32_t slice = std::min(remaining, yieldTicks);
        vTaskDelay(slice);
        ServiceLitchcraftFramePagingDuringSettings();
        remaining -= slice;
    }
#else
    vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
#endif
}

void SettingsPersistenceWorker::PublishSuccess() {
    auto& doc = m_writer.Doc();
    if (m_supportRuntimeStats != nullptr) m_supportRuntimeStats->NoteSaved(doc);
    const uint32_t persistedTotal =
        m_persistedSaveTotal.load(std::memory_order_acquire);
    m_persistedSaveTotal.store(
        doc[fxe::SETTINGS_SAVE_TOTAL_KEY] | persistedTotal,
        std::memory_order_release);
    m_lastError.store(
        static_cast<uint8_t>(fxe::SettingsStoreError::None),
        std::memory_order_release);
    m_completedSaveCount.fetch_add(1U, std::memory_order_acq_rel);
    if (m_source != nullptr) {
        m_source->ReportAsyncSaveSuccess(
            m_writer.GetLastSaveSizeBytes(),
            m_persistedSaveTotal.load(std::memory_order_acquire));
    }
}

void SettingsPersistenceWorker::AcquireFlashAccessWindow() {
    // SDK access observation, not a hardware lock or LED scheduling policy.
#if defined(LITCH_NATIVE)
    if (m_storageAccessTestHook != nullptr) {
        m_storageAccessTestHook(m_storageAccessTestContext, true);
    }
#endif
#if defined(LITCH_ESP32S3) && (defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE))
    SetLiveIngressHandlePhase(3U);
#endif
    m_settingsPhysicalAccess.BeginFlashLease();
    if (m_settingsPhysicalAccess.IsRecordingTransaction()) {
        m_settingsPhysicalAccess.RecordOuterAccess(0U);
    }
    m_flashAccesses.fetch_add(1U, std::memory_order_relaxed);
}

void SettingsPersistenceWorker::BeginPersistenceTiming() {
    m_activePersistenceTiming = {};
    m_activePersistenceTiming.sequence = ++m_persistenceTimingSequence;
    m_settingsPhysicalAccess.BeginTransaction();
}

void SettingsPersistenceWorker::CompletePersistenceTiming(
    PersistenceTiming& destination, const uint32_t generation,
    const bool succeeded, const uint32_t elapsedMs) {
    m_settingsPhysicalAccess.EndTransaction();
    m_activePersistenceTiming.available = true;
    m_activePersistenceTiming.generation = generation;
    m_activePersistenceTiming.succeeded = succeeded;
    m_activePersistenceTiming.elapsedMs = elapsedMs;
    m_activePersistenceTiming.flash = m_settingsPhysicalAccess.TransactionSample();
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
        destination = m_activePersistenceTiming;
        xSemaphoreGive(m_mutex);
    }
}

void SettingsPersistenceWorker::ReleaseFlashAccessWindow() {
    m_settingsPhysicalAccess.EndFlashLease();
#if defined(LITCH_NATIVE)
    if (m_storageAccessTestHook != nullptr) {
        m_storageAccessTestHook(m_storageAccessTestContext, false);
    }
#endif
#if defined(LITCH_ESP32S3) && (defined(LITCH_PANIC_DIAGNOSTIC) || defined(LITCH_NATIVE))
    SetLiveIngressHandlePhase(1U);
#endif
}

bool SettingsPersistenceWorker::DrainForDirectStorageAccess() {
    return WaitUntilIdle(DIRECT_STORAGE_WAIT_TIMEOUT_MS);
}

bool SettingsPersistenceWorker::HasNewerPending(
    const uint32_t generation) {
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    bool found = false;
    for (uint8_t i = 0U; i < BUFFER_COUNT; ++i) {
        if (m_bufferState[i] == BufferState::Pending &&
            GenerationReached(m_bufferGeneration[i], generation + 1U)) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(m_mutex);
    return found;
}

void SettingsPersistenceWorker::ReleaseBuffer(const uint8_t index) {
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    m_bufferState[index] = BufferState::Free;
    xSemaphoreGive(m_mutex);
}

void SettingsPersistenceWorker::PublishFailure(
    fxe::SettingsStoreError error) {
    if (error == fxe::SettingsStoreError::None) {
        error = m_writer.GetLastError();
    }
    if (error == fxe::SettingsStoreError::None) {
        error = fxe::SettingsStoreError::SerializeFailed;
    }
    m_lastError.store(
        static_cast<uint8_t>(error),
        std::memory_order_release);
    m_failureCount.fetch_add(1U, std::memory_order_acq_rel);
}

void SettingsPersistenceWorker::Run() {
    while (true) {
#if defined(LITCH_ESP32S3)
        const auto wait = (m_litchcraftFrameStorageActive ||
                           m_litchcraftFramePlaybackPending.load(
                               std::memory_order_acquire) ||
                           m_storageReadState.load(std::memory_order_acquire) == 1U)
            ? pdMS_TO_TICKS(LITCHCRAFT_FRAME_TRANSFER_TICK_MS)
            : portMAX_DELAY;
        (void)ulTaskNotifyTake(pdTRUE, wait);
#else
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#endif
        if (ServiceStorageReformat()) continue;
        if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
            CancelDisabledPending();
            xSemaphoreGive(m_mutex);
        }
        if (m_currentGameSyncServiceCallback != nullptr) {
            m_currentGameSyncServiceCallback(
                m_currentGameSyncServiceContext);
        }
#if defined(LITCH_ESP32S3)
        (void)ServiceLitchcraftFrameClear();
        (void)ServiceLitchcraftFrameIngress();
        ServiceLitchcraftFramePlaybackSlice();
        (void)ServiceLitchcraftFrameIngress();
        // While frames own the union, Enqueue uses only the independent
        // settings slot. Service that snapshot between bounded frame slices;
        // playback and an open transfer retain their existing Store authority.
#endif
        uint8_t index = 0U;
        while (TryTakePending(index)) {
            const auto saveElapsed = fxe::ElapsedTime::StartNow();
            BeginPersistenceTiming();
            runtime::WatchCurrentTask(2U, runtime::ProgressStage::Storage);
            const uint32_t generation = m_bufferGeneration[index];

            bool saved = false;
            bool abandonedForNewerSnapshot = false;
            bool transactionInvoked = false;
            auto evidence = fxe::SettingsStore::StorageFailureEvidence::Unconfirmed;
            const bool prepared =
                PrepareBuffer(index, transactionInvoked);
            if (!prepared) {
                PublishFailure(m_snapshotStorageError == fxe::SettingsStoreError::None
                    ? fxe::SettingsStoreError::PreparationFailed : m_snapshotStorageError);
                if (m_snapshotStorageError == fxe::SettingsStoreError::WriteFailed ||
                    m_snapshotStorageError == fxe::SettingsStoreError::ReadFailed ||
                    m_snapshotStorageError == fxe::SettingsStoreError::VerifyFailed) {
                    evidence = fxe::SettingsStore::StorageFailureEvidence::Confirmed;
                }
            } else {
                uint8_t attempts = 0U;
                const uint32_t priorStorageFailures = m_writer.GetConfirmedStorageFailureCount();
                while (!(saved = SavePreparedBuffer(index))) {
                    ++attempts;
                    PublishFailure(m_snapshotStorageError);
                    if (m_writer.GetConfirmedStorageFailureCount() != priorStorageFailures) {
                        evidence = fxe::SettingsStore::StorageFailureEvidence::Confirmed;
                    }
                    if (attempts >= MAX_SAVE_ATTEMPTS) {
                        break;
                    }
                    if (HasNewerPending(generation)) {
                        abandonedForNewerSnapshot = true;
                        break;
                    }
                    WaitForSettingsRetry();
                }
            }
            if (transactionInvoked) {
                m_snapshotTransactionResultCallback(
                    m_snapshotTransactionContext,
                    saved ? SnapshotTransactionOutcome::Saved
                          : SnapshotTransactionOutcome::Failed,
                    m_writer.Doc());
            }
            if (m_currentGameSyncServiceCallback != nullptr) {
                m_currentGameSyncServiceCallback(
                    m_currentGameSyncServiceContext);
            }
            if (saved) {
                if (m_durableSnapshotCallback != nullptr) {
                    m_callbackReceipt.store(
                        m_receiptGeneration.load(std::memory_order_acquire) == generation
                            ? generation : 0U, std::memory_order_release);
                    m_durableSnapshotCallback(
                        m_durableSnapshotContext, m_writer.Doc());
                    m_callbackReceipt.store(0U, std::memory_order_release);
                }
                PublishSuccess();
                m_lastSavedGeneration.store(
                    generation, std::memory_order_release);
            } else if (!abandonedForNewerSnapshot && m_source != nullptr) {
                // A consumer/resource refusal fails this transaction, not the
                // shared writer. Preserve its Failed receipt and retry the
                // latest RAM document through the source's bounded cooldown.
                m_source->ReportAsyncSaveFailure(GetLastError(), evidence);
            }
            const uint32_t elapsedMs = saveElapsed.ElapsedMs();
            CompletePersistenceTiming(m_settingsTiming, generation, saved, elapsedMs);
            m_lastSettingsMs.store(elapsedMs, std::memory_order_release);
            m_lastCompletedGeneration.store(generation, std::memory_order_release);
            ReleaseBuffer(index);
            if (m_receiptGeneration.load(std::memory_order_acquire) == generation) {
                m_receiptOutcome.store(saved ? SaveOutcome::Saved : SaveOutcome::Failed,
                                       std::memory_order_release);
            }
            runtime::UnwatchCurrentTask(2U);
        }
#if defined(LITCH_ESP32S3)
        if (m_litchcraftFrameStorageActive) {
            // Auxiliary artwork and benchmark work retain their existing
            // exclusion. Only the independent settings slot shares residency.
            continue;
        }
        (void)ServiceLitchcraftFrameIngress();
        if (TryTakeLitchcraftArtwork()) {
            const auto saveElapsed = fxe::ElapsedTime::StartNow();
            runtime::WatchCurrentTask(2U, runtime::ProgressStage::Storage);
            const uint32_t generation = m_litchcraftArtworkGeneration;

            bool saved = false;
            bool encoded = true;
            if (m_litchcraftArtworkDocument != nullptr &&
                m_litchcraftArtworkEnvelope != nullptr) {
                encoded = modules::LitchcraftPersistence::EncodeAdvancedEnvelope(
                    *m_litchcraftArtworkDocument, m_litchcraftArtworkActive,
                    *m_litchcraftArtworkEnvelope);
                if (encoded) {
                    m_litchcraftArtworkBytes =
                        m_litchcraftArtworkEnvelope->bytes.data();
                    m_litchcraftArtworkSize = m_litchcraftArtworkEnvelope->size;
                }
            }
            uint8_t attempts = 0U;
            while (encoded) {
                saved = m_litchcraftArtworkStorage.Save(
                    m_litchcraftArtworkBytes, m_litchcraftArtworkSize);
                if (saved && m_litchcraftFrameStream.NeedsDurableRetirement()) {
                    LitchcraftFrameFile file;
                    (void)file.SetAccessOps({this,
                        [](void* context) {
                            static_cast<SettingsPersistenceWorker*>(context)
                                ->AcquireFlashAccessWindow();
                        },
                        [](void* context, LitchcraftFrameFile::AccessOperation,
                           size_t, uint32_t) {
                            static_cast<SettingsPersistenceWorker*>(context)
                                ->ReleaseFlashAccessWindow();
                        }});
                    uint32_t retired = m_litchcraftFrameStream.RetiredGeneration();
                    saved = modules::litchcraft_frame_store::FrameStore::Retire(
                        file.StoreOps(), retired, &retired) ==
                        modules::litchcraft_frame_store::Status::Ok;
                    if (saved) m_litchcraftFrameStream.BindRetirementGeneration(retired);
                }
                if (saved || ++attempts >= MAX_SAVE_ATTEMPTS) break;
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            }
            if (!saved) {
                m_litchcraftArtworkFailureCount.fetch_add(
                    1U, std::memory_order_acq_rel);
            }
            m_lastArtworkMs.store(saveElapsed.ElapsedMs(), std::memory_order_release);
            m_lastCompletedGeneration.store(generation, std::memory_order_release);
            ReleaseLitchcraftArtwork();
            if (m_receiptGeneration.load(std::memory_order_acquire) ==
                generation) {
                m_receiptOutcome.store(
                    saved ? SaveOutcome::Saved : SaveOutcome::Failed,
                    std::memory_order_release);
            }
            runtime::UnwatchCurrentTask(2U);
        }
        (void)ServiceLitchcraftFrameIngress();
        ServiceLitchcraftFramePlaybackSlice();
        (void)ServiceLitchcraftFrameIngress();
        RunSettingsReadDiagnostic();
#endif
    }
}

} // namespace firmware

#endif
