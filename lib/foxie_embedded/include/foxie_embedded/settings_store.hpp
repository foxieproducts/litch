#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <ArduinoJson.h>

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/fixed_function.hpp"

namespace fxe {

enum class SettingsStoreError : uint8_t {
    None = 0,
    MountFailed,
    OpenReadFailed,
    ParseFailed,
    OpenWriteFailed,
    SerializeFailed,
    WriteFailed,
    VerifyFailed,
    DeleteFailed,
    Unsupported,
    ReadFailed,
    ResourceExhausted,
    FileTooLarge,
    FormatFailed,
    AdmissionRejected,
    PreparationFailed,
    AccessTimeout,
};

inline const char* SettingsStoreErrorToString(const SettingsStoreError e) {
    switch (e) {
        case SettingsStoreError::None:
            return "None";
        case SettingsStoreError::MountFailed:
            return "MountFailed";
        case SettingsStoreError::OpenReadFailed:
            return "OpenReadFailed";
        case SettingsStoreError::ParseFailed:
            return "ParseFailed";
        case SettingsStoreError::OpenWriteFailed:
            return "OpenWriteFailed";
        case SettingsStoreError::SerializeFailed:
            return "SerializeFailed";
        case SettingsStoreError::WriteFailed:
            return "WriteFailed";
        case SettingsStoreError::VerifyFailed:
            return "VerifyFailed";
        case SettingsStoreError::DeleteFailed:
            return "DeleteFailed";
        case SettingsStoreError::Unsupported:
            return "Unsupported";
        case SettingsStoreError::ReadFailed:
            return "ReadFailed";
        case SettingsStoreError::ResourceExhausted:
            return "ResourceExhausted";
        case SettingsStoreError::FileTooLarge:
            return "FileTooLarge";
        case SettingsStoreError::FormatFailed:
            return "FormatFailed";
        case SettingsStoreError::AdmissionRejected:
            return "AdmissionRejected";
        case SettingsStoreError::PreparationFailed:
            return "PreparationFailed";
        case SettingsStoreError::AccessTimeout:
            return "AccessTimeout";
    }

    return "?";
}

// Save-only filesystem witness. Paths are roles, never user/private filenames.
enum class SettingsSaveOperationKind : uint8_t {
    None, Mount, Probe, CreateParents, OpenWrite, OpenRead, Write, Sync,
    CloseWrite, Read, CloseRead, Exists, Remove, Rename,
};
enum class SettingsSavePathRole : uint8_t {
    None, Filesystem, Primary, Pending, Retry, Backup,
};
struct SettingsSaveOperation {
    uint32_t saveGeneration = 0U;
    uint32_t durationMs = 0U;
    uint32_t requestedBytes = 0U;
    int operationError = 0;
    SettingsSaveOperationKind kind = SettingsSaveOperationKind::None;
    SettingsSavePathRole path = SettingsSavePathRole::None;
    SettingsSavePathRole destination = SettingsSavePathRole::None;
    bool succeeded = false;
};
inline const char* SettingsSaveOperationKindToString(const SettingsSaveOperationKind kind) {
    switch (kind) {
        case SettingsSaveOperationKind::None: return "None";
        case SettingsSaveOperationKind::Mount: return "Mount";
        case SettingsSaveOperationKind::Probe: return "Probe";
        case SettingsSaveOperationKind::CreateParents: return "CreateParents";
        case SettingsSaveOperationKind::OpenWrite: return "OpenWrite";
        case SettingsSaveOperationKind::OpenRead: return "OpenRead";
        case SettingsSaveOperationKind::Write: return "Write";
        case SettingsSaveOperationKind::Sync: return "Sync";
        case SettingsSaveOperationKind::CloseWrite: return "CloseWrite";
        case SettingsSaveOperationKind::Read: return "Read";
        case SettingsSaveOperationKind::CloseRead: return "CloseRead";
        case SettingsSaveOperationKind::Exists: return "Exists";
        case SettingsSaveOperationKind::Remove: return "Remove";
        case SettingsSaveOperationKind::Rename: return "Rename";
    }
    return "?";
}

inline const char* SettingsSavePathRoleToString(const SettingsSavePathRole role) {
    switch (role) {
        case SettingsSavePathRole::None: return "None";
        case SettingsSavePathRole::Filesystem: return "Filesystem";
        case SettingsSavePathRole::Primary: return "Primary";
        case SettingsSavePathRole::Pending: return "Pending";
        case SettingsSavePathRole::Retry: return "Retry";
        case SettingsSavePathRole::Backup: return "Backup";
    }
    return "?";
}

// Load-scoped sanitation provenance. Call Record before removing a corrupt
// top-level key. The selected record owns this context; callers must not retain it.
class SettingsRemovedKeys final {
  public:
    explicit SettingsRemovedKeys(bool enabled = false, JsonDocument* scratch = nullptr)
        : m_enabled(enabled), m_scratch(scratch) {}
    ~SettingsRemovedKeys() { if (m_scratchInitialized && m_scratch != nullptr) m_scratch->clear(); }
    bool Record(const JsonString& key);
    bool Record(const char* key) { return Record(JsonString(key)); }
    bool Failed() const { return m_failed; }

  private:
    friend class SettingsStore;
    bool m_enabled = false;
    bool m_failed = false;
    JsonDocument* m_scratch = nullptr;
    bool m_scratchInitialized = false;
#if !defined(ARDUINO)
    std::optional<DynamicJsonDocument> m_nativeScratch;
#endif
};

struct SettingsStoreConfig {
    // Default path used for both device and native. On device this is a LittleFS path.
    // On native/emulator it's a filesystem path relative to the working directory.
    const char* path = "settings.json";

    // Ordinary settings use a trailing debounce to coalesce interactive edits.
    uint32_t deferredSaveDelayMs = 2000;

    // High-value state also uses a trailing debounce, but with a much shorter
    // durability window. Its timer is independent from ordinary settings.
    uint32_t mustSaveDelayMs = 100;

    // Generic/native integrations may perform an elapsed deferred save on the
    // caller. Firmware should disable this and install an asynchronous sink so
    // a worker-start failure cannot move flash I/O onto the render/input loop.
    bool allowSynchronousDeferredSaves = true;

    // \brief When enabled, Save() verifies the tmp file matches the bytes written.
    // \note This adds a read-back pass on every save.
    //       Default is enabled to catch storage/flush issues early; disable if it
    //       impacts UX latency.
    bool verifyWrites = true;

    // \brief Optional project-defined migration hook executed after a successful Load().
    // \note Use this to rename keys, populate defaults, and remove obsolete fields.
    //       Return true if the document was mutated.
    using MigrateFunc = fxe::FixedFunction<bool(JsonDocument& doc), 64>;
    MigrateFunc migrateFunc;

    // \brief Optional project-defined scrub/validation hook executed on Load() and before Save().
    // \note Use this for schema enforcement that is specific to a project (e.g., bounded ranges).
    //       Return true if the document was mutated.
    // Only reported sanitation removals authorize load-time alternate recovery.
    // A null context disables reporting (alternate validation and pre-save).
    using ScrubFunc = fxe::FixedFunction<bool(JsonDocument& doc, SettingsRemovedKeys* removed), 64>;
    ScrubFunc scrubFunc;
};

// Resident capacity of the live settings document. The maximum firmware-schema
// fixture includes 128 undo entries, artwork, manufacturing evidence and
// per-player assistance preferences, and reserves at
// least 2 KiB after reloading owned strings. Key canonicalization is in-place;
// alternate-file merge uses one bounded temporary document during Load().
static constexpr size_t SETTINGS_STORE_JSON_CAPACITY = 9600;
// Shared runtime staging bound; includes the serialized document terminator.
static constexpr size_t SETTINGS_STORE_SNAPSHOT_BUFFER_CAPACITY = 5376U;
static constexpr size_t SETTINGS_STORE_PATH_CAPACITY = 64;
// Maximum accepted serialized file size. Settings I/O is streamed; this is a
// validation limit and does not reserve a buffer of this size.
static constexpr size_t SETTINGS_STORE_MAX_FILE_SIZE_BYTES = 8192;

// Persisted counter: total number of successful settings saves.
// This value survives reboots because it is stored inside the settings JSON.
static constexpr const char* SETTINGS_SAVE_TOTAL_KEY = "SETTINGS_SAVE_TOTAL";

// Minimal cross-target settings persistence.
// - Device: LittleFS + ArduinoJson
// - Native/emulator: filesystem + ArduinoJson
class SettingsStore final {
  public:
    // A rejected operation is not evidence that flash failed. Only the I/O
    // owner can certify an attempted write/readback or underlying I/O failure.
    enum class StorageFailureEvidence : uint8_t { Unconfirmed, Confirmed };
    enum class AsyncSaveAdmission : uint8_t { Accepted, Deferred, Rejected };
    using BeforePersistenceCallback = fxe::FixedFunction<void(), 32>;
    using AsyncSaveRequestCallback =
        fxe::FixedFunction<bool(const JsonDocument&), 32>;
    using AsyncSaveAdmissionCallback =
        fxe::FixedFunction<AsyncSaveAdmission(const JsonDocument&), 32>;
    using BeforeDirectStorageAccessCallback = fxe::FixedFunction<bool(), 32>;
    using StorageAccessCallback = fxe::FixedFunction<void(), 32>;
    using SaveOperationCallback =
        fxe::FixedFunction<void(const SettingsSaveOperation&), 32>;
    using VerifiedPrimaryLoadCallback = void (*)(void*, const JsonDocument&);
    // Backward-compatible alias for integrations that used the original,
    // overly narrow name. Reads and metadata access must be coordinated too.
    using StorageMutationCallback = StorageAccessCallback;

    explicit SettingsStore(const SettingsStoreConfig& cfg = {});

    // Observe only a complete canonical-primary read, before migration or RAM
    // defaults. A selected pending/backup file does not establish this proof.
    void SetVerifiedPrimaryLoadCallback(VerifiedPrimaryLoadCallback callback,
                                       void* context) {
        m_verifiedPrimaryLoadCallback = callback;
        m_verifiedPrimaryLoadContext = context;
    }

    // \brief Install a guard before armed synchronous storage operations.
    // \note Async snapshot admission does not access storage or invoke this
    //       callback; its owner guards actual I/O. Deferred admission retains
    //       the armed intent, and accepted admission clears it. Direct blocking
    //       operations keep this guard even with an async callback installed.
    void SetBeforePersistenceCallback(BeforePersistenceCallback cb) {
        m_beforePersistenceCb = std::move(cb);
    }
    // \brief Route runtime save requests to an asynchronous persistence owner.
    // \note The callback must copy or serialize the document before returning.
    //       SaveNow() reports queue acceptance while this callback is installed.
    void SetAsyncSaveRequestCallback(AsyncSaveRequestCallback cb) {
        m_asyncSaveAdmissionCb = nullptr;
        m_asyncSaveRequestCb = std::move(cb);
    }
    // Deferred means no snapshot was accepted yet. Preserve dirty intent and
    // diagnostics; only Rejected reports a new failure. Installing either
    // callback replaces the other, retaining legacy bool callback semantics.
    void SetAsyncSaveAdmissionCallback(AsyncSaveAdmissionCallback cb) {
        m_asyncSaveRequestCb = nullptr;
        m_asyncSaveAdmissionCb = std::move(cb);
    }
    bool HasAsyncSaveRequestCallback() const {
        return static_cast<bool>(m_asyncSaveRequestCb) ||
               static_cast<bool>(m_asyncSaveAdmissionCb);
    }
    void SetBeforeDirectStorageAccessCallback(
        BeforeDirectStorageAccessCallback cb) {
        m_beforeDirectStorageAccessCb = std::move(cb);
    }
    // \brief Bracket each individual filesystem access during persistence.
    // \note Reads, metadata queries, mounts, and mutations can all touch the
    //       flash/cache path and must not overlap time-critical peripherals.
    void SetStorageAccessCallbacks(StorageAccessCallback before,
                                   StorageAccessCallback after) {
        m_beforeStorageAccessCb = std::move(before);
        m_afterStorageAccessCb = std::move(after);
    }
    // Install before saving. Runs synchronously after the access-release callback,
    // with operation errno restored. Timing excludes acquisition and release work.
    // The observer must not save recursively or change the callbacks while saving.
    void SetSaveOperationCallback(SaveOperationCallback callback) {
        m_saveOperationCb = std::move(callback);
    }
    void SetStorageMutationCallbacks(StorageAccessCallback before,
                                     StorageAccessCallback after) {
        SetStorageAccessCallbacks(std::move(before), std::move(after));
    }
    void ArmPersistenceGuard() { m_persistenceGuardArmed = true; }
    // Arm only when a deferred or failed save is actually pending. This lets
    // policy observers protect an upcoming write without leaving a stale
    // guard after an explicit save already succeeded.
    bool ArmPersistenceGuardIfSavePending() {
        if (!m_dirty && !m_mustSaveDirty) {
            return false;
        }
        m_persistenceGuardArmed = true;
        return true;
    }
    [[nodiscard]] bool IsPersistenceGuardArmed() const {
        return m_persistenceGuardArmed;
    }

    // Recovery scratch is borrowed only for this call and cleared before return.
    // Embedded callers supply a distinct document with SETTINGS_STORE_JSON_CAPACITY.
    bool Load(JsonDocument* recoveryScratch = nullptr);

    // Debug/isolation helper: mark settings disabled without touching storage.
    // Intended for bring-up when the system is crash-looping during early boot.
    void DisableForBoot();

    // \brief Schedule an ordinary deferred save. Call Update() periodically.
    //
    // The deferral timer is debounced: calling MarkDirty() again resets the
    // delay, so the eventual save happens after settings have been stable for
    // deferredSaveDelayMs.
    void MarkDirty();

    // \brief Schedule a high-value save after mustSaveDelayMs.
    //
    // This is also a trailing debounce, but ordinary MarkDirty() calls do not
    // reset its timer. A save from either tier persists and clears both tiers.
    void MarkMustSaveDirty();

    // \brief Flush a pending save if the delay has elapsed.
    void Update();

    // \brief Request an immediate save (still best-effort).
    // \note With an async callback installed, this returns queue acceptance;
    //       the storage write is performed by the callback's owner.
    bool SaveNow();

    // \brief Perform storage I/O synchronously, bypassing the async callback.
    // \warning Runtime callers should reserve this for boot/shutdown boundaries.
    bool SaveNowBlocking();

    // Worker-owned diagnostic use only: read primary to this instance's private
    // document, without mount, recovery, repair, normalization or callbacks into
    // the live runtime. Byte/hash outputs describe bytes actually read.
    bool ReadPrimaryForDiagnostics(size_t& outBytes, uint32_t& outFnv32);

    // \brief Access underlying JSON document.
    JsonDocument& Doc() { return m_doc; }
    const JsonDocument& Doc() const { return m_doc; }

    const char* GetPath() const { return m_path; }
    uint32_t GetDeferredSaveDelayMs() const {
        return m_cfg.deferredSaveDelayMs;
    }
    uint32_t GetMustSaveDelayMs() const { return m_cfg.mustSaveDelayMs; }

    // Unique id for this SettingsStore instance.
    // This avoids cache collisions when multiple SettingsStore objects are created at the
    // same address over time (e.g., native tests that reuse stack frames).
    uint32_t GetInstanceId() const { return m_instanceId; }

    uint32_t GetSaveCount() const {
        return m_saveCount +
               m_asyncSaveCount.load(std::memory_order_acquire);
    }
    size_t GetLastSaveSizeBytes() const {
        if (m_asyncSaveCount.load(std::memory_order_acquire) != 0U) {
            return m_asyncLastSaveSizeBytes.load(std::memory_order_acquire);
        }
        return m_lastSaveSizeBytes;
    }
    uint32_t GetPersistedSaveTotal() const {
        const uint32_t documentTotal =
            m_doc[SETTINGS_SAVE_TOTAL_KEY] | 0U;
        const uint32_t asyncTotal =
            m_asyncPersistedSaveTotal.load(std::memory_order_acquire);
        return asyncTotal > documentTotal ? asyncTotal : documentTotal;
    }

    // \brief Incremented whenever the JSON document changes.
    // \note Use this to invalidate cached settings values.
    uint32_t GetRevision() const { return m_revision; }

    bool IsDisabled() const { return m_disabled.load(std::memory_order_acquire); }
    // Stop persistence for this boot while retaining RAM values and evidence.
    void DisablePersistenceForSession();
    bool HasTerminalSaveFailure() const {
        return m_terminalSaveFailure.load(std::memory_order_acquire);
    }
    // A valid document was loaded, but an alternate read or filename repair failed.
    // Keep this document usable and retryable until a verified save completes.
    bool IsLoadRepairPending() const {
        return m_loadRepairPending.load(std::memory_order_acquire);
    }
    uint32_t GetErrorCount() const {
        return m_errorCount +
               m_asyncErrorCount.load(std::memory_order_acquire);
    }
    SettingsStoreError GetLastError() const {
        return m_lastError.load(std::memory_order_acquire);
    }
    bool HasConfirmedStorageFailure() const {
        return m_confirmedStorageFailure.load(std::memory_order_acquire);
    }
    // Monotonic evidence counter distinguishes this attempt from an older
    // failed write whose sticky diagnostic has not yet been cleared by success.
    uint32_t GetConfirmedStorageFailureCount() const {
        return m_confirmedStorageFailureCount.load(std::memory_order_acquire);
    }

    // Platform async writers report completion without touching the live JSON
    // document from their worker task.
    void ReportAsyncSaveSuccess(size_t bytesWritten,
                                uint32_t persistedSaveTotal) {
        m_asyncLastSaveSizeBytes.store(
            bytesWritten, std::memory_order_release);
        m_asyncSaveCount.fetch_add(1U, std::memory_order_acq_rel);
        m_asyncPersistedSaveTotal.store(
            persistedSaveTotal, std::memory_order_release);
        m_lastError.store(SettingsStoreError::None, std::memory_order_release);
        m_terminalSaveFailure.store(false, std::memory_order_release);
        m_consecutiveSaveFailures.store(0U, std::memory_order_release);
        // Only Update consumes retry intent: this completion may belong to an
        // older snapshot than a newer request rejected by its consumer.
        m_loadRepairPending.store(false, std::memory_order_release);
        m_confirmedStorageFailure.store(false, std::memory_order_release);
    }
    // A request failure retains retry intent. Only concrete shared-storage
    // evidence can make saving terminal for all consumers.
    void ReportAsyncSaveFailure(SettingsStoreError error,
        StorageFailureEvidence evidence = StorageFailureEvidence::Unconfirmed) {
        if (evidence == StorageFailureEvidence::Confirmed) {
            m_confirmedStorageFailure.store(true, std::memory_order_release);
            m_confirmedStorageFailureCount.fetch_add(1U, std::memory_order_acq_rel);
            m_terminalSaveFailure.store(true, std::memory_order_release);
        }
        m_lastError.store(error, std::memory_order_release);
        m_asyncErrorCount.fetch_add(1U, std::memory_order_acq_rel);
        NoteSaveFailure();
    }

    // \brief Notify that the document was mutated without a save request.
    // \note Prefer MarkDirty() when changes should be persisted.
    void NotifyDocChanged();

    // \brief Fast typed readers without extra containsKey() lookups.
    uint32_t ReadValue(const char* key, uint32_t defaultValue) const;
    int32_t ReadValue(const char* key, int32_t defaultValue) const;
    bool ReadValue(const char* key, bool defaultValue) const;
    const char* ReadValue(const char* key, const char* defaultValue) const;

    // Test/recovery helpers.
    bool DeleteFile();
    // Explicit recovery reset only. Prepare a minimal replacement from a raw
    // durable snapshot, even when the live store is disabled for this boot.
    // Two verified publications replace both primary and pre-reset backup.
    bool ResetFileForRecovery(bool (*prepare)(JsonDocument&));
    bool CorruptFileForTest();

// Recovery owner only: erase the complete board filesystem, remount, then
    // write and verify this private writer's prepared document. The live source
    // stays disabled until reboot; mounting alone never establishes recovery.
    bool ReformatFilesystemAndSaveNowBlocking();

  private:
    bool SaveImpl();
    void PrepareDocumentForPersistence();
    bool WaitForDirectStorageAccess();
    void SetError(SettingsStoreError err, const char* msg,
        StorageFailureEvidence evidence = StorageFailureEvidence::Unconfirmed);
    void MarkLoadRepairPending(const char* message,
                               SettingsStoreError error = SettingsStoreError::WriteFailed);
    void NoteSaveFailure();
    void NotifyBeforePersistence() const {
        if (m_persistenceGuardArmed && m_beforePersistenceCb) {
            m_beforePersistenceCb();
        }
    }
    bool CompletePersistence(const bool success) {
        if (success && !m_dirty && !m_mustSaveDirty) {
            m_persistenceGuardArmed = false;
        }
        return success;
    }

    SettingsStoreConfig m_cfg;
    bool m_recoveryResetReplacement = false;
    BeforePersistenceCallback m_beforePersistenceCb = nullptr;
    AsyncSaveRequestCallback m_asyncSaveRequestCb = nullptr;
    AsyncSaveAdmissionCallback m_asyncSaveAdmissionCb = nullptr;
    BeforeDirectStorageAccessCallback m_beforeDirectStorageAccessCb = nullptr;
    StorageAccessCallback m_beforeStorageAccessCb = nullptr;
    StorageAccessCallback m_afterStorageAccessCb = nullptr;
    SaveOperationCallback m_saveOperationCb = nullptr;
    uint32_t m_saveOperationGeneration = 0U;
    bool m_persistenceGuardArmed = false;

    char m_path[SETTINGS_STORE_PATH_CAPACITY] = {};
    StaticJsonDocument<SETTINGS_STORE_JSON_CAPACITY> m_doc;

    bool m_dirty = false;
    fxe::ElapsedTime m_dirtyTime;
    bool m_mustSaveDirty = false;
    fxe::ElapsedTime m_mustSaveDirtyTime;

    uint32_t m_saveCount = 0;
    size_t m_lastSaveSizeBytes = 0;

    uint32_t m_revision = 0;

    std::atomic<bool> m_disabled{false};
    std::atomic<bool> m_terminalSaveFailure{false};
    std::atomic<bool> m_confirmedStorageFailure{false};
    std::atomic<uint32_t> m_confirmedStorageFailureCount{0U};
    bool m_mounted = false;
    uint32_t m_errorCount = 0;
    std::atomic<SettingsStoreError> m_lastError{SettingsStoreError::None};
    std::atomic<uint32_t> m_asyncSaveCount{0U};
    std::atomic<size_t> m_asyncLastSaveSizeBytes{0U};
    std::atomic<uint32_t> m_asyncPersistedSaveTotal{0U};
    std::atomic<uint32_t> m_asyncErrorCount{0U};
    std::atomic<bool> m_asyncRetryNeeded{false};
    std::atomic<uint8_t> m_consecutiveSaveFailures{0U};
    bool m_retryCooldownActive = false;
    fxe::ElapsedTime m_retryCooldown;
    std::atomic<bool> m_loadRepairPending{false};

    uint32_t m_instanceId = 0;
    VerifiedPrimaryLoadCallback m_verifiedPrimaryLoadCallback = nullptr;
    void* m_verifiedPrimaryLoadContext = nullptr;
};

template <typename T>
class CachedSetting final {
  public:
    CachedSetting() = default;
    CachedSetting(const char* key, const T defaultValue) { Configure(key, defaultValue); }

    void Configure(const char* key, const T defaultValue) {
        m_key = key;
        m_default = defaultValue;
        m_value = defaultValue;
        m_revision = 0;
        m_storeInstanceId = 0;
        m_store = nullptr;
    }

    T Get(const SettingsStore& store) {
        const uint32_t revision = store.GetRevision();
        const uint32_t instanceId = store.GetInstanceId();
        if (m_store != &store || m_storeInstanceId != instanceId || m_revision != revision) {
            m_value = store.ReadValue(m_key, m_default);
            m_revision = revision;
            m_storeInstanceId = instanceId;
            m_store = &store;
        }
        return m_value;
    }

  private:
    const char* m_key = nullptr;
    T m_default{};
    T m_value{};
    uint32_t m_revision = 0;
    uint32_t m_storeInstanceId = 0;
    const SettingsStore* m_store = nullptr;
};

} // namespace fxe
