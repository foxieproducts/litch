#include "foxie_embedded/settings_store.hpp"

#include "settings_store_internal.hpp"

#include <cerrno>
#include <cstdio>

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
