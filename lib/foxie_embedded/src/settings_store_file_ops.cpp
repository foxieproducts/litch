#include "foxie_embedded/settings_store.hpp"

#include "settings_store_internal.hpp"

#include <cerrno>
#include <cstdio>
#include <memory>
#include <new>

#include "foxie_embedded/logger.hpp"

#ifdef ARDUINO
#include <LittleFS.h>
#else
#include <sys/stat.h>
#endif

namespace fxe {

namespace {

#ifdef ARDUINO

bool RemoveSettingsArtifact(const char* const path) {
    // Arduino's exists() reports an open failure as absence. Only a verified
    // missing path counts as erased, both before and after unlink.
    const auto before = ProbeSettingsPath(path);
    if (before == SettingsPathState::Missing) return true;
    return before == SettingsPathState::Present && RemoveSettingsFile(path) &&
           ProbeSettingsPath(path) == SettingsPathState::Missing;
}

#else

enum class NativePathState : uint8_t {
    Absent = 0U,
    Present,
    Unknown,
};

NativePathState GetNativePathState(const char* const path) {
    struct stat info {};
    if (::lstat(path, &info) == 0) {
        return NativePathState::Present;
    }
    return (errno == ENOENT || errno == ENOTDIR)
               ? NativePathState::Absent
               : NativePathState::Unknown;
}

bool RemoveSettingsArtifact(const char* const path) {
    const NativePathState before = GetNativePathState(path);
    if (before == NativePathState::Absent) {
        return true;
    }
    if (before != NativePathState::Present || std::remove(path) != 0) {
        return false;
    }
    return GetNativePathState(path) == NativePathState::Absent;
}

#endif

} // namespace

bool SettingsStore::ResetFileForRecovery(bool (*prepare)(JsonDocument&)) {
    NotifyBeforePersistence();
    if (prepare == nullptr || !WaitForDirectStorageAccess()) return CompletePersistence(false);
    // Do not activate old game/settings callbacks or reuse a disabled live
    // document in safe mode. Recovery is the sole writer until restart.
    SettingsStoreConfig config{};
    config.path = m_path;
    config.verifyWrites = true;
    std::unique_ptr<SettingsStore> writer(new (std::nothrow) SettingsStore(config));
    if (!writer) return CompletePersistence(false);
    const SettingsStore& source = *this;
    writer->m_beforeStorageAccessCb = source.m_beforeStorageAccessCb;
    writer->m_afterStorageAccessCb = source.m_afterStorageAccessCb;
    writer->m_saveOperationCb = source.m_saveOperationCb;
    const bool loaded = writer->Load();
    const auto readError = writer->GetLastError();
    // Explicit reset may replace proven corrupt/missing data, but an I/O or
    // resource failure does not prove the retained identity is unreadable.
    if (readError != SettingsStoreError::None && readError != SettingsStoreError::ParseFailed) {
        SetError(readError, "factory reset source is unavailable");
        return CompletePersistence(false);
    }
    if (!loaded) writer->m_doc.clear();
    if (!prepare(writer->m_doc) || writer->m_doc.overflowed()) return CompletePersistence(false);
    writer->m_recoveryResetReplacement = true;
    // A complete staging document may become boot authority even before its
    // promotion. Its domain tombstone already resumes remaining cleanup.
    // The second promotion replaces the old backup without an identity gap.
    if (!writer->SaveNowBlocking() || !writer->SaveNowBlocking()) {
        SetError(writer->GetLastError(), "factory reset replacement was not fully persisted");
        return CompletePersistence(false);
    }
    char retryPath[SETTINGS_STORE_PATH_CAPACITY] = {};
    if (!MakeDerivedPath(retryPath, sizeof retryPath, m_path, ".retry")) return CompletePersistence(false);
    if (m_beforeStorageAccessCb) m_beforeStorageAccessCb();
    const bool removedRetry = RemoveSettingsArtifact(retryPath);
    if (m_afterStorageAccessCb) m_afterStorageAccessCb();
    if (!removedRetry) {
        SetError(SettingsStoreError::DeleteFailed, "factory reset retry artifact could not be removed");
        return CompletePersistence(false);
    }
    m_doc.set(writer->m_doc);
    m_dirty = false;
    m_mustSaveDirty = false;
    m_loadRepairPending.store(false, std::memory_order_release);
    m_asyncRetryNeeded.store(false, std::memory_order_release);
    m_lastError = SettingsStoreError::None;
    return CompletePersistence(true);
}

bool SettingsStore::DeleteFile() {
    NotifyBeforePersistence();
    if (!WaitForDirectStorageAccess()) {
        return CompletePersistence(false);
    }

    char tmpPath[SETTINGS_STORE_PATH_CAPACITY] = {};
    char backupPath[SETTINGS_STORE_PATH_CAPACITY] = {};
    char retryPath[SETTINGS_STORE_PATH_CAPACITY] = {};
    if (!MakeDerivedPath(tmpPath, sizeof(tmpPath), m_path, ".tmp") ||
        !MakeDerivedPath(backupPath, sizeof(backupPath), m_path, ".bak") ||
        !MakeDerivedPath(retryPath, sizeof(retryPath), m_path, ".retry")) {
        SetError(SettingsStoreError::Unsupported,
                 "settings path too long for delete scratch/backup");
        return CompletePersistence(false);
    }

#ifdef ARDUINO
    if (!TryMountLittleFsNoAutoFormat()) {
        SetError(SettingsStoreError::MountFailed,
                 "LittleFS mount failed; explicit filesystem recovery required");
        return CompletePersistence(false);
    }

    m_mounted = true;
#endif

    bool removedAllArtifacts = true;
    if (!RemoveSettingsArtifact(m_path)) {
        removedAllArtifacts = false;
    }
    if (!RemoveSettingsArtifact(tmpPath)) {
        removedAllArtifacts = false;
    }
    if (!RemoveSettingsArtifact(backupPath)) {
        removedAllArtifacts = false;
    }
    if (!RemoveSettingsArtifact(retryPath)) {
        removedAllArtifacts = false;
    }
    if (!removedAllArtifacts) {
        SetError(SettingsStoreError::DeleteFailed,
                 "failed to remove or verify settings artifacts");
        return CompletePersistence(false);
    }

    // Treat a completely successful delete as a runtime reset as well:
    // - Clear in-memory state so console commands reflect the erase immediately.
    // - Cancel any pending deferred save so we don't recreate the file.
    // Failure deliberately retains all RAM state and pending-save bookkeeping.
    m_doc.clear();
    m_dirty = false;
    m_mustSaveDirty = false;
    m_lastError = SettingsStoreError::None;
    m_loadRepairPending.store(false, std::memory_order_release);
    m_consecutiveSaveFailures.store(0U, std::memory_order_release);
    m_retryCooldownActive = false;
    m_asyncRetryNeeded.store(false, std::memory_order_release);

    return CompletePersistence(true);
}

bool SettingsStore::CorruptFileForTest() {
    NotifyBeforePersistence();
    if (!WaitForDirectStorageAccess()) {
        return CompletePersistence(false);
    }
#ifdef ARDUINO
    if (!TryMountLittleFsNoAutoFormat()) {
        SetError(SettingsStoreError::MountFailed,
                 "LittleFS mount failed; corrupt skipped");
        return CompletePersistence(false);
    }

    m_mounted = true;

    File f = LittleFS.open(m_path, "w");
    if (!f) {
        SetError(SettingsStoreError::OpenWriteFailed, "open(write) failed");
        return CompletePersistence(false);
    }

    // Intentionally invalid JSON so Load() will fail and recovery paths can be validated.
    const size_t written = f.print("{");
    f.flush();
    f.close();
    if (written == 0) {
        SetError(SettingsStoreError::WriteFailed, "corrupt write failed");
        return CompletePersistence(false);
    }
    return CompletePersistence(true);
#else
    FILE* f = std::fopen(m_path, "wb");
    if (!f) {
        SetError(SettingsStoreError::OpenWriteFailed, "open(write) failed");
        return CompletePersistence(false);
    }
    const char bad = '{';
    const size_t written = std::fwrite(&bad, 1, 1, f);
    std::fclose(f);
    if (written != 1) {
        SetError(SettingsStoreError::WriteFailed, "corrupt write failed");
        return CompletePersistence(false);
    }
    return CompletePersistence(true);
#endif
}


} // namespace fxe
