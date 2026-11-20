#include "settings_store_internal.hpp"

#include <cstdio>
#include <cstring>

#include "foxie_embedded/settings_store.hpp"

#include "foxie_embedded/logger.hpp"

#ifdef ARDUINO
#include <LittleFS.h>
#endif

namespace fxe {
namespace {
SettingsStoreError LoadFailureError(const JsonLoadResult result) {
    switch (result) {
        case JsonLoadResult::OpenError: return SettingsStoreError::OpenReadFailed;
        case JsonLoadResult::ReadError: return SettingsStoreError::ReadFailed;
        case JsonLoadResult::NoMemory: return SettingsStoreError::ResourceExhausted;
        case JsonLoadResult::TooLarge: return SettingsStoreError::FileTooLarge;
        default: return SettingsStoreError::ParseFailed;
    }
}

bool IsUnavailableRead(const JsonLoadResult result) {
    return result == JsonLoadResult::OpenError || result == JsonLoadResult::ReadError ||
           result == JsonLoadResult::NoMemory || result == JsonLoadResult::TooLarge;
}
} // namespace

bool SettingsStore::Load(JsonDocument* const recoveryScratch) {
    if (recoveryScratch == &m_doc) {
        SetError(SettingsStoreError::ResourceExhausted, "settings recovery scratch aliases source");
        return false;
    }
    NotifyBeforePersistence();
    if (!WaitForDirectStorageAccess()) {
        return CompletePersistence(false);
    }
    m_doc.clear();
    NotifyDocChanged();
    m_lastError = SettingsStoreError::None;
    m_consecutiveSaveFailures.store(0U, std::memory_order_release);
    m_retryCooldownActive = false;
    m_asyncRetryNeeded.store(false, std::memory_order_release);
    m_loadRepairPending.store(false, std::memory_order_release);
    m_mounted = false;

    char tmpPath[SETTINGS_STORE_PATH_CAPACITY] = {};
    const bool haveTmpPath = MakeDerivedPath(tmpPath, sizeof(tmpPath), m_path, ".tmp");

    char backupPath[SETTINGS_STORE_PATH_CAPACITY] = {};
    const bool haveBackupPath = MakeDerivedPath(backupPath, sizeof(backupPath), m_path, ".bak");

#ifdef ARDUINO
    bool erasedStorage = false;
    if (!TryMountLittleFsErasedPartitionSafe(false, false, &erasedStorage)) {
        if (erasedStorage) {
            // No filesystem exists yet. Keep defaults in RAM; its first
            // user-owned save may initialize proven erased storage.
            m_disabled = false;
            MarkLoadRepairPending("erased filesystem awaits user initialization",
                                  SettingsStoreError::None);
            return CompletePersistence(true);
        }
        m_disabled = true;
        SetError(SettingsStoreError::MountFailed,
                 "LittleFS mount failed; settings disabled");
        return CompletePersistence(false);
    }

    m_disabled = false;
    m_mounted = true;
#endif

    const auto Return = [&](const bool value) {
        return CompletePersistence(value);
    };

    bool loadedFromTmp = false;
    enum class LoadedSource { Primary, Tmp, Backup };
    auto loadedSource = LoadedSource::Primary;
    bool candidateProcessed = false;
    bool candidateNeedsRepair = false;
    JsonLoadResult alternateFailure = JsonLoadResult::Ok;
    bool primaryReadUnavailable = false;
    JsonLoadResult primaryLoadFailure = JsonLoadResult::Ok;
    const SettingsReadAccess readAccess{&m_beforeStorageAccessCb, &m_afterStorageAccessCb,
                                       &m_confirmedStorageFailure};

    SettingsRemovedKeys removedKeys(false, recoveryScratch);
    bool recoveryPrimary = false, recoveryTmp = false, recoveryBackup = false;
    const auto ProcessSelected = [&](const char* selectedPath) {
        // Only a proven missing path excludes an alternate. An unavailable
        // source must retain its read failure instead of becoming empty data.
        recoveryPrimary = loadedSource != LoadedSource::Primary &&
            (primaryReadUnavailable || ProbeSettingsPath(m_path) != SettingsPathState::Missing);
        recoveryTmp = loadedSource != LoadedSource::Tmp && haveTmpPath &&
            (alternateFailure != JsonLoadResult::Ok ||
             ProbeSettingsPath(tmpPath) != SettingsPathState::Missing);
        recoveryBackup = loadedSource != LoadedSource::Backup && haveBackupPath &&
            ProbeSettingsPath(backupPath) != SettingsPathState::Missing;
        removedKeys.m_enabled = recoveryPrimary || recoveryTmp || recoveryBackup;
        return ProcessSettingsDocument(m_doc, m_cfg, selectedPath, &removedKeys);
    };

    const auto TrySelectPending = [&]() -> bool {
        if (!haveTmpPath || tmpPath[0] == '\0') {
            return false;
        }

        m_doc.clear();
        const JsonLoadResult recoveredResult = LoadJsonFromPath(tmpPath, m_doc, readAccess);
        if (recoveredResult != JsonLoadResult::Ok) {
            if (IsUnavailableRead(recoveredResult)) {
                alternateFailure = recoveredResult;
            } else if (recoveredResult != JsonLoadResult::Missing) {
                fxe::Warn("settings: incomplete or invalid tmp; retaining source for diagnosis");
            }
            m_doc.clear();
            return false;
        }

        loadedFromTmp = true;
        loadedSource = LoadedSource::Tmp;

        // JSON syntax alone cannot establish a valid project record. Preserve
        // every source while recovering removed fields from main and backup;
        // rotating an invalid main over backup here could lose the last good
        // record before the ordinary scoped recovery below ever sees it.
        candidateNeedsRepair = ProcessSelected(tmpPath);
        candidateProcessed = true;
        if (candidateNeedsRepair || removedKeys.Failed()) {
            fxe::Warn("settings: tmp needs scoped recovery; retaining source files");
            return true;
        }

        MarkLoadRepairPending("selected tmp retained in RAM; primary untouched",
                              SettingsStoreError::None);
        return true;
    };

    const auto TryRecoverFromPath = [&](const char* path, const char* label) -> bool {
        if (path == nullptr || path[0] == '\0') {
            return false;
        }

        m_doc.clear();
        const JsonLoadResult recoveredResult = LoadJsonFromPath(path, m_doc, readAccess);
        if (recoveredResult != JsonLoadResult::Ok) {
            if (IsUnavailableRead(recoveredResult)) alternateFailure = recoveredResult;
            m_doc.clear();
            return false;
        }

        fxe::Warn("settings: recovered from %s", (label != nullptr) ? label : "alternate");

        loadedSource = path == tmpPath ? LoadedSource::Tmp : LoadedSource::Backup;
        candidateNeedsRepair = ProcessSelected(path);
        candidateProcessed = true;
        if (candidateNeedsRepair || removedKeys.Failed()) {
            // Preserve a semantically invalid alternate for scoped recovery,
            // just as for an invalid temporary candidate above.
            return true;
        }

        if (primaryReadUnavailable) {
            // An unreadable primary is not proven corrupt. Keep both artifacts
            // until the worker has a verified replacement ready to promote.
            MarkLoadRepairPending("primary read unavailable; valid alternate retained",
                                  LoadFailureError(primaryLoadFailure));
            return true;
        }

        MarkLoadRepairPending("selected alternate retained in RAM; primary untouched",
                              SettingsStoreError::None);
        return true;
    };

    // Prefer a valid interrupted-save candidate in RAM. Its files remain
    // untouched until an actual user mutation owns a verified promotion.
    (void)TrySelectPending();

    const JsonLoadResult primaryResult = loadedFromTmp ? JsonLoadResult::Ok
                                                       : LoadJsonFromPath(m_path, m_doc, readAccess);
    if (!loadedFromTmp && primaryResult == JsonLoadResult::Ok &&
        m_verifiedPrimaryLoadCallback != nullptr) {
        m_verifiedPrimaryLoadCallback(m_verifiedPrimaryLoadContext, m_doc);
    }
    primaryReadUnavailable = IsUnavailableRead(primaryResult);
    primaryLoadFailure = primaryResult;
    if (primaryResult != JsonLoadResult::Ok) {
        // Important recovery case: SaveImpl() uses main->bak + tmp->main renames.
        // A power loss between those renames can leave the primary file missing
        // while a valid .tmp or .bak exists. Treat that as recoverable.
        if (primaryResult == JsonLoadResult::Missing || primaryResult == JsonLoadResult::Empty) {
            if (primaryResult == JsonLoadResult::Missing) {
                fxe::Info("settings: no primary file; using defaults");
            } else {
                fxe::Warn("settings: primary file empty; using defaults");
            }
            // The pending file was already examined above, including its
            // bounded retries. Do not repeat a failed read in this fallback.
            if (!(haveBackupPath && TryRecoverFromPath(backupPath, "backup"))) {
                if (alternateFailure != JsonLoadResult::Ok) {
                    SetError(LoadFailureError(alternateFailure), "alternate settings unavailable after bounded read attempts");
                    return Return(false);
                }
                return Return(true);
            }
        } else if (!(haveBackupPath && TryRecoverFromPath(backupPath, "backup"))) {
            SetError(LoadFailureError(alternateFailure != JsonLoadResult::Ok
                                          ? alternateFailure : primaryResult),
                     "settings load failed");
            return Return(false);
        }
    }

    if (alternateFailure != JsonLoadResult::Ok && !m_doc.isNull()) {
        MarkLoadRepairPending("alternate settings unavailable; valid document retained",
                              LoadFailureError(alternateFailure));
    }

    bool mutated = candidateProcessed ? candidateNeedsRepair
                                      : ProcessSelected(m_path);

    if (removedKeys.Failed()) {
        SetError(SettingsStoreError::ResourceExhausted, "settings removal provenance exhausted");
        return Return(false);
    }
    if (mutated) {
        if (removedKeys.m_scratchInitialized) {
            // The selected field needs an alternate, but these sources already
            // exhausted their bounded read attempts. Do not read them again.
            const JsonLoadResult priorFailure = recoveryPrimary && primaryReadUnavailable
                ? primaryLoadFailure : alternateFailure;
            if (priorFailure != JsonLoadResult::Ok) {
                SetError(LoadFailureError(priorFailure), "settings recovery source unavailable");
                return Return(false);
            }
            auto& scratch = *removedKeys.m_scratch;
            JsonVariant selected = scratch[0U].to<JsonVariant>();
            if (!CopyJsonValueOwned(selected, m_doc.as<JsonVariantConst>(), 0U) || scratch.overflowed()) {
                SetError(SettingsStoreError::ResourceExhausted, "settings recovery compaction failed");
                return Return(false);
            }
            const JsonObjectConst eligible = scratch[1U].as<JsonObjectConst>();
            JsonLoadResult recoveryFailure = JsonLoadResult::Ok;
            const auto Merge = [&](const char* path) {
                mutated = MergeMissingSettingsFromPath(path, selected.as<JsonObject>(), eligible,
                    m_cfg, m_doc, recoveryFailure, readAccess) || mutated;
                return recoveryFailure == JsonLoadResult::Ok && !scratch.overflowed();
            };
            if ((recoveryPrimary && !Merge(m_path)) || (recoveryTmp && !Merge(tmpPath)) ||
                (recoveryBackup && !Merge(backupPath))) {
                m_doc.clear();
                (void)CopyJsonValueOwned(m_doc.to<JsonVariant>(), selected.as<JsonVariantConst>(), 0U);
                SetError(scratch.overflowed() ? SettingsStoreError::ResourceExhausted : LoadFailureError(recoveryFailure),
                    "settings scoped recovery failed");
                return Return(false);
            }
            m_doc.clear();
            if (!CopyJsonValueOwned(m_doc.to<JsonVariant>(), selected.as<JsonVariantConst>(), 0U) || m_doc.overflowed()) {
                SetError(SettingsStoreError::ResourceExhausted, "settings recovery restore failed");
                return Return(false);
            }
        }
        if (loadedSource != LoadedSource::Primary && !IsLoadRepairPending()) {
            // A usable alternate is not yet the verified canonical primary.
            // Keep repair pending through queue acceptance until save completion.
            MarkLoadRepairPending("selected alternate awaits verified repair",
                primaryReadUnavailable ? LoadFailureError(primaryLoadFailure)
                                       : SettingsStoreError::None);
        } else {
            MarkLoadRepairPending("migration or sanitation pending user mutation",
                                  SettingsStoreError::None);
        }
    }

    return Return(true);
}

} // namespace fxe
