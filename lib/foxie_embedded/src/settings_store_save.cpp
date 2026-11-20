#include "settings_store_internal.hpp"

#include <cstdio>
#include <cerrno>
#include <cstring>

#ifndef ARDUINO

#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "foxie_embedded/settings_store.hpp"

#include "foxie_embedded/logger.hpp"

#ifdef ARDUINO
#include <LittleFS.h>
#include <Print.h>
#endif

namespace fxe {

namespace {

static constexpr uint32_t FNV1A32_OFFSET_BASIS = 2166136261u;
static constexpr uint32_t FNV1A32_PRIME = 16777619u;

uint32_t Fnv1a32UpdateByte(uint32_t h, const uint8_t b) {
    h ^= static_cast<uint32_t>(b);
    h *= FNV1A32_PRIME;
    return h;
}

uint32_t Fnv1a32UpdateBytes(uint32_t h, const uint8_t* data, const size_t n) {
    for (size_t i = 0; i < n; ++i) {
        h = Fnv1a32UpdateByte(h, data[i]);
    }
    return h;
}

#ifndef ARDUINO
bool EnsureParentDirsExist(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return true;
    }

    char buf[SETTINGS_STORE_PATH_CAPACITY] = {};
    std::strncpy(buf, path, sizeof(buf) - 1);

    for (char* p = buf; *p != '\0'; ++p) {
        if (*p != '/') {
            continue;
        }

        *p = '\0';
        if (buf[0] != '\0') {
            if (::mkdir(buf, 0755) != 0 && errno != EEXIST) {
                *p = '/';
                return false;
            }
        }
        *p = '/';
    }

    return true;
}
#endif

struct SaveAccess {
    SettingsStore::StorageAccessCallback& before;
    SettingsStore::StorageAccessCallback& after;
    SettingsStore::SaveOperationCallback& observer;
    uint32_t generation;
    const char* primary;
    SettingsSavePathRole staging = SettingsSavePathRole::Pending;

    SettingsSavePathRole Role(const char* path) const {
        if (std::strcmp(path, primary) == 0) return SettingsSavePathRole::Primary;
        const size_t length = std::strlen(primary);
        if (std::strncmp(path, primary, length) != 0) return SettingsSavePathRole::None;
        const char* suffix = path + length;
        if (std::strcmp(suffix, ".tmp") == 0) return SettingsSavePathRole::Pending;
        if (std::strcmp(suffix, ".retry") == 0) return SettingsSavePathRole::Retry;
        if (std::strcmp(suffix, ".bak") == 0) return SettingsSavePathRole::Backup;
        return SettingsSavePathRole::None;
    }
};

class StorageAccessScope final {
  public:
    StorageAccessScope(SaveAccess& storage, SettingsSaveOperationKind kind,
                       SettingsSavePathRole path, size_t bytes = 0U,
                       SettingsSavePathRole destination = SettingsSavePathRole::None)
        : m_storage(storage) {
        m_operation.saveGeneration = storage.generation;
        m_operation.kind = kind;
        m_operation.path = path;
        m_operation.destination = destination;
        m_operation.requestedBytes = static_cast<uint32_t>(bytes);
        if (storage.before) storage.before();
        m_startedMs = fxe::Millis();
    }

    ~StorageAccessScope() {
        const int operationError = errno;
        const uint32_t durationMs = fxe::Millis() - m_startedMs;
        if (m_storage.after) m_storage.after();
        m_operation.durationMs = durationMs;
        m_operation.operationError = m_operation.succeeded ? 0 : operationError;
        errno = operationError;
        if (m_storage.observer) m_storage.observer(m_operation);
        errno = operationError;
    }

    void Result(bool succeeded) { m_operation.succeeded = succeeded; }

  private:
    SaveAccess& m_storage;
    SettingsSaveOperation m_operation{};
    uint32_t m_startedMs = 0U;
};

SettingsPathState ProbePathWithAccess(
    const char* path,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Probe, storage.Role(path));
    const auto result = ProbeSettingsPath(path);
    access.Result(result != SettingsPathState::Unknown);
    return result;
}

#ifdef ARDUINO

bool MountLittleFsForSave(
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Mount, SettingsSavePathRole::Filesystem);
    const bool result = TryMountLittleFsErasedPartitionSafe(false);
    access.Result(result);
    return result;
}

int OpenLittleFsForWrite(
    const char* path,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::OpenWrite, storage.Role(path));
    const int result = OpenSettingsWriteFile(path);
    access.Result(result >= 0);
    return result;
}

File OpenLittleFsForRead(
    const char* path,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::OpenRead, storage.Role(path));
    errno = 0;
    File result = LittleFS.open(path, "r");
    access.Result(static_cast<bool>(result));
    return result;
}

bool FlushLittleFsFile(
    const int file,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Sync, storage.staging);
    const bool result = SyncSettingsFile(file);
    access.Result(result);
    return result;
}

bool CloseLittleFsFile(
    const int file,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::CloseWrite, storage.staging);
    const bool result = CloseSettingsFile(file);
    access.Result(result);
    return result;
}

void CloseLittleFsFile(
    File& file,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::CloseRead, storage.staging);
    file.close();
    access.Result(true);
}

int ReadLittleFsFile(
    File& file,
    uint8_t* buffer,
    const size_t size,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Read, storage.staging, size);
    errno = 0;
    const int result = file.read(buffer, size);
    access.Result(result >= 0 && errno == 0);
    return result;
}

bool LittleFsPathExists(
    const char* path,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Exists, storage.Role(path));
    // The pinned Arduino FS wrapper opens file data merely to test presence.
    // Keep metadata on the same direct VFS boundary as settings writes.
    const bool result = ProbeSettingsPath(path) == SettingsPathState::Present;
    access.Result(result);
    return result;
}

bool RemoveLittleFsPath(
    const char* path,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Remove, storage.Role(path));
    const bool result = RemoveSettingsFile(path);
    access.Result(result);
    return result;
}

bool RenameLittleFsPath(
    const char* from,
    const char* to,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Rename, storage.Role(from), 0U, storage.Role(to));
    const bool result = RenameSettingsFile(from, to);
    access.Result(result);
    return result;
}

struct BufferedHashingPrint final : public Print {
    static constexpr size_t WRITE_CHUNK_BYTES = SETTINGS_SAVE_IO_CHUNK_BYTES;

    const int out;
    SaveAccess& storage;
    uint32_t hash = FNV1A32_OFFSET_BASIS;
    size_t bytesWritten = 0;
    size_t bufferedBytes = 0;
    bool shortWrite = false;
    uint8_t buffer[WRITE_CHUNK_BYTES] = {};

    BufferedHashingPrint(
        const int o,
        SaveAccess& access)
        : out(o),
          storage(access) {}

    size_t write(const uint8_t b) override {
        return write(&b, 1U);
    }

    size_t write(const uint8_t* buffer, size_t size) override {
        if (buffer == nullptr || size == 0U || shortWrite) {
            return 0U;
        }

        size_t accepted = 0U;
        while (accepted < size) {
            const size_t available = WRITE_CHUNK_BYTES - bufferedBytes;
            const size_t remaining = size - accepted;
            const size_t copied = remaining < available ? remaining : available;
            std::memcpy(this->buffer + bufferedBytes,
                        buffer + accepted,
                        copied);
            bufferedBytes += copied;
            accepted += copied;

            if (bufferedBytes == WRITE_CHUNK_BYTES && !FlushBuffered()) {
                break;
            }
        }
        return accepted;
    }

    bool FlushBuffered() {
        if (bufferedBytes == 0U) {
            return !shortWrite;
        }

        const size_t requested = bufferedBytes;
        size_t written = 0U;
        {
            StorageAccessScope access(storage, SettingsSaveOperationKind::Write, storage.staging, requested);
            // The descriptor bypasses Arduino File's additional stdio buffer.
            // fsync/metadata may still erase flash; this bounds submitted data,
            // not the duration of an individual LittleFS operation.
            written = WriteSettingsFile(out, buffer, requested);
            access.Result(written == requested);
        }
        if (written > 0U) {
            hash = Fnv1a32UpdateBytes(hash, buffer, written);
            bytesWritten += written;
        }
        bufferedBytes = 0U;
        shortWrite = shortWrite || (written != requested);
        return !shortWrite;
    }
};

bool TryVerifyTmpFileHashLittleFs(const char* tmpPath,
                                  const uint32_t expectedHash,
                                  const size_t expectedBytes,
                                  uint32_t& outActualHash,
                                  size_t& outActualBytes,
                                  SettingsStore::StorageFailureEvidence& outEvidence,
                                  SaveAccess& storage) {
    outActualHash = FNV1A32_OFFSET_BASIS;
    outActualBytes = 0;
    outEvidence = SettingsStore::StorageFailureEvidence::Unconfirmed;

    File f = OpenLittleFsForRead(tmpPath, storage);
    if (!f) {
        if (errno == EIO) outEvidence = SettingsStore::StorageFailureEvidence::Confirmed;
        return false;
    }

    uint8_t buf[SETTINGS_SAVE_IO_CHUNK_BYTES];
    while (true) {
        const int n = ReadLittleFsFile(
            f, buf, sizeof(buf), storage);
        if (n <= 0) {
            const int readError = errno;
            if (readError != 0) {
                CloseLittleFsFile(f, storage);
                if (readError == EIO) outEvidence = SettingsStore::StorageFailureEvidence::Confirmed;
                return false;
            }
            break;
        }
        outActualHash = Fnv1a32UpdateBytes(outActualHash, buf, static_cast<size_t>(n));
        outActualBytes += static_cast<size_t>(n);
    }

    CloseLittleFsFile(f, storage);
    const bool matches = outActualBytes == expectedBytes && outActualHash == expectedHash;
    if (!matches) outEvidence = SettingsStore::StorageFailureEvidence::Confirmed;
    return matches;
}
#else
bool EnsureParentDirsExistWithAccess(
    const char* path,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::CreateParents, storage.Role(path));
    const bool result = EnsureParentDirsExist(path);
    access.Result(result);
    return result;
}

std::FILE* OpenStdioFile(
    const char* path,
    const char* mode,
    SaveAccess& storage) {
    StorageAccessScope access(storage, mode[0] == 'r' ? SettingsSaveOperationKind::OpenRead : SettingsSaveOperationKind::OpenWrite, storage.Role(path));
    errno = 0;
    std::FILE* result = std::fopen(path, mode);
    access.Result(result != nullptr);
    return result;
}

size_t ReadStdioFile(
    std::FILE* file,
    void* buffer,
    const size_t size,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Read, storage.staging, size);
    errno = 0;
    const size_t result = std::fread(buffer, 1, size, file);
    access.Result(std::ferror(file) == 0);
    return result;
}

size_t WriteStdioFile(
    std::FILE* file,
    const uint8_t* buffer,
    const size_t size,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Write, storage.staging, size);
    const size_t result = std::fwrite(buffer, 1, size, file);
    access.Result(result == size);
    return result;
}

bool FlushStdioFile(
    std::FILE* file,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Sync, storage.staging);
    const bool result = std::fflush(file) == 0;
    access.Result(result);
    return result;
}

bool CloseStdioFile(
    std::FILE* file,
    SaveAccess& storage,
    SettingsSaveOperationKind kind = SettingsSaveOperationKind::CloseWrite) {
    StorageAccessScope access(storage, kind, storage.staging);
    const bool result = std::fclose(file) == 0;
    access.Result(result);
    return result;
}

bool StdioPathExists(
    const char* path,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Exists, storage.Role(path));
    struct stat info = {};
    const bool result = ::stat(path, &info) == 0;
    access.Result(result);
    return result;
}

bool RemoveStdioPath(
    const char* path,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Remove, storage.Role(path));
    const bool result = std::remove(path) == 0;
    access.Result(result);
    return result;
}

bool RenameStdioPath(
    const char* from,
    const char* to,
    SaveAccess& storage) {
    StorageAccessScope access(storage, SettingsSaveOperationKind::Rename, storage.Role(from), 0U, storage.Role(to));
    const bool result = std::rename(from, to) == 0;
    access.Result(result);
    return result;
}

struct HashingFileWriter final {
    std::FILE* out = nullptr;
    SaveAccess& storage;
    uint32_t hash = FNV1A32_OFFSET_BASIS;
    size_t bytesWritten = 0;
    bool shortWrite = false;

    HashingFileWriter(
        std::FILE* file,
        SaveAccess& access)
        : out(file),
          storage(access) {}

    size_t write(const uint8_t b) {
        return write(&b, 1);
    }

    size_t write(const uint8_t* buffer, const size_t size) {
        const size_t n = WriteStdioFile(
            out, buffer, size, storage);
        if (n > 0) {
            hash = Fnv1a32UpdateBytes(hash, buffer, n);
            bytesWritten += n;
        }
        shortWrite = shortWrite || (n != size);
        return n;
    }
};

bool TryVerifyTmpFileHashStdio(const char* tmpPath,
                               const uint32_t expectedHash,
                               const size_t expectedBytes,
                               uint32_t& outActualHash,
                               size_t& outActualBytes,
                               SettingsStore::StorageFailureEvidence& outEvidence,
                               SaveAccess& storage) {
    outActualHash = FNV1A32_OFFSET_BASIS;
    outActualBytes = 0;
    outEvidence = SettingsStore::StorageFailureEvidence::Unconfirmed;

    std::FILE* f = OpenStdioFile(tmpPath, "rb", storage);
    if (f == nullptr) {
        if (errno == EIO) outEvidence = SettingsStore::StorageFailureEvidence::Confirmed;
        return false;
    }

    uint8_t buf[SETTINGS_SAVE_IO_CHUNK_BYTES];
    while (true) {
        const size_t n = ReadStdioFile(
            f, buf, sizeof(buf), storage);
        if (std::ferror(f)) {
            const int readError = errno;
            (void)CloseStdioFile(f, storage, SettingsSaveOperationKind::CloseRead);
            if (readError == EIO) outEvidence = SettingsStore::StorageFailureEvidence::Confirmed;
            return false;
        }
        if (n > 0) {
            outActualHash = Fnv1a32UpdateBytes(outActualHash, buf, n);
            outActualBytes += n;
        }
        if (n < sizeof(buf)) {
            break;
        }
    }

    (void)CloseStdioFile(f, storage, SettingsSaveOperationKind::CloseRead);
    const bool matches = outActualBytes == expectedBytes && outActualHash == expectedHash;
    if (!matches) outEvidence = SettingsStore::StorageFailureEvidence::Confirmed;
    return matches;
}
#endif

struct SettingsSaveTotalGuard {
    // Verification alone cannot certify a persisted save total.
    JsonDocument& doc;
    bool hadKey = false;
    uint32_t oldValue = 0;
    bool committed = false;

    explicit SettingsSaveTotalGuard(JsonDocument& d)
        : doc(d) {
        hadKey = doc.containsKey(SETTINGS_SAVE_TOTAL_KEY);
        oldValue = doc[SETTINGS_SAVE_TOTAL_KEY] | 0U;

        uint32_t newValue = oldValue;
        if (newValue != UINT32_MAX) {
            ++newValue;
        }
        doc[SETTINGS_SAVE_TOTAL_KEY] = newValue;
    }

    void Commit() {
        committed = true;
    }

    ~SettingsSaveTotalGuard() {
        if (committed) {
            return;
        }

        if (hadKey) {
            doc[SETTINGS_SAVE_TOTAL_KEY] = oldValue;
        } else {
            (void)doc.remove(SETTINGS_SAVE_TOTAL_KEY);
        }
    }
};

} // namespace

void SettingsStore::PrepareDocumentForPersistence() {
    SettingsScrubStats scrubStats;
    const bool scrubbed = ScrubBadPrintableStrings(m_doc, &scrubStats);
    if (scrubbed) {
        fxe::Warn("settings: scrubbed before save: removed_keys=%u nulled_array=%u",
                  static_cast<unsigned>(scrubStats.removedObjectKeys),
                  static_cast<unsigned>(scrubStats.nulledArrayElements));
    }
    if (m_cfg.scrubFunc) {
        (void)m_cfg.scrubFunc(m_doc, nullptr);
    }
    (void)NormalizeTopLevelObjectKeysUppercase(m_doc);
}

bool SettingsStore::SaveImpl() {
    if (++m_saveOperationGeneration == 0U) ++m_saveOperationGeneration;
    SaveAccess storage{m_beforeStorageAccessCb, m_afterStorageAccessCb,
                       m_saveOperationCb, m_saveOperationGeneration, m_path};
    NotifyBeforePersistence();
    PrepareDocumentForPersistence();

#ifdef ARDUINO
    if (m_disabled) {
        if (m_lastError != SettingsStoreError::Unsupported) {
            SetError(SettingsStoreError::Unsupported, "settings disabled; save skipped");
        }
        return false;
    }

    if (!m_mounted) {
        if (!MountLittleFsForSave(
                storage)) {
            // Conservative: never auto-format on save. Preserve dirty state so we can retry later.
            SetError(SettingsStoreError::MountFailed,
                     "LittleFS mount failed; save skipped");
            return false;
        }
        m_mounted = true;
    }

    char retainedTmpPath[SETTINGS_STORE_PATH_CAPACITY] = {};
    char retryPath[SETTINGS_STORE_PATH_CAPACITY] = {};
    char backupPath[SETTINGS_STORE_PATH_CAPACITY] = {};
    if (!MakeDerivedPath(retainedTmpPath, sizeof(retainedTmpPath), m_path, ".tmp") ||
        !MakeDerivedPath(backupPath, sizeof(backupPath), m_path, ".bak")) {
        SetError(SettingsStoreError::Unsupported, "settings path too long for tmp/bak");
        return false;
    }

    const bool preservePending = ProbePathWithAccess(
        retainedTmpPath, storage) != SettingsPathState::Missing;
    if (preservePending && !MakeDerivedPath(retryPath, sizeof(retryPath), m_path, ".retry")) {
        SetError(SettingsStoreError::Unsupported, "settings path too long for retry scratch");
        return false;
    }
    // A prior pending file may be the only valid recovered document. The
    // worker has a separate store instance, so protect the pathname itself.
    const char* tmpPath = preservePending ? retryPath : retainedTmpPath;
    storage.staging = storage.Role(tmpPath);

    static constexpr uint8_t MAX_VERIFY_ATTEMPTS = 2;
    size_t written = 0;
    uint32_t expectedHash = 0;
    bool verified = false;
    auto verificationEvidence = StorageFailureEvidence::Unconfirmed;
    SettingsSaveTotalGuard saveTotalGuard(m_doc);

    for (uint8_t attempt = 0; attempt < MAX_VERIFY_ATTEMPTS; ++attempt) {
        const int f = OpenLittleFsForWrite(
            tmpPath, storage);
        if (f < 0) {
            SetError(SettingsStoreError::OpenWriteFailed, "open(write tmp) failed");
            return false;
        }

        const size_t required = measureJson(m_doc);
        if (required == 0 || required >= SETTINGS_STORE_MAX_FILE_SIZE_BYTES) {
            CloseLittleFsFile(
                f, storage);
            (void)RemoveLittleFsPath(
                tmpPath, storage);
            SetError(SettingsStoreError::SerializeFailed, "settings JSON too large");
            return false;
        }

        BufferedHashingPrint hashingPrint(
            f, storage);
        written = serializeJson(m_doc, hashingPrint);
        const bool bufferedWriteOk = hashingPrint.FlushBuffered();
        const bool syncOk = FlushLittleFsFile(
            f, storage);
        const bool closeOk = CloseLittleFsFile(
            f, storage);
        if (!syncOk || !closeOk) {
            (void)RemoveLittleFsPath(
                tmpPath, storage);
            SetError(SettingsStoreError::WriteFailed, "sync/close tmp failed",
                     StorageFailureEvidence::Confirmed);
            return false;
        }
        if (!bufferedWriteOk || hashingPrint.shortWrite ||
            hashingPrint.bytesWritten != written || written != required) {
            (void)RemoveLittleFsPath(
                tmpPath, storage);
            const bool ioFailed = !bufferedWriteOk || hashingPrint.shortWrite;
            SetError(ioFailed ? SettingsStoreError::WriteFailed : SettingsStoreError::SerializeFailed,
                     "incomplete settings write/serialization", ioFailed
                         ? StorageFailureEvidence::Confirmed : StorageFailureEvidence::Unconfirmed);
            return false;
        }
        if (written == 0) {
            (void)RemoveLittleFsPath(
                tmpPath, storage);
            SetError(SettingsStoreError::SerializeFailed, "serialize/write produced 0 bytes");
            return false;
        }

        expectedHash = hashingPrint.hash;

        if (!m_cfg.verifyWrites) {
            break;
        }

        uint32_t actualHash = 0;
        size_t actualBytes = 0;
        auto attemptEvidence = StorageFailureEvidence::Unconfirmed;
        if (TryVerifyTmpFileHashLittleFs(
                tmpPath,
                expectedHash,
                written,
                actualHash,
                actualBytes,
                attemptEvidence,
                storage)) {
            verified = true;
            if (attempt > 0) {
                fxe::Warn(
                    "settings verify ok after retry: attempt=%u/%u bytes=%u hash=%08x",
                    static_cast<unsigned>(attempt + 1),
                    static_cast<unsigned>(MAX_VERIFY_ATTEMPTS),
                    static_cast<unsigned>(written),
                    static_cast<unsigned>(expectedHash));
            }
            break;
        }

        if (attemptEvidence == StorageFailureEvidence::Confirmed) {
            verificationEvidence = attemptEvidence;
        }
        fxe::Warn(
            "settings verify failed: attempt=%u/%u expectedBytes=%u actualBytes=%u expectedHash=%08x actualHash=%08x",
            static_cast<unsigned>(attempt + 1),
            static_cast<unsigned>(MAX_VERIFY_ATTEMPTS),
            static_cast<unsigned>(written),
            static_cast<unsigned>(actualBytes),
            static_cast<unsigned>(expectedHash),
            static_cast<unsigned>(actualHash));

        (void)RemoveLittleFsPath(
            tmpPath, storage);
        if (attempt + 1 >= MAX_VERIFY_ATTEMPTS) {
            fxe::Error("settings verify failed after %u attempts; save aborted",
                       static_cast<unsigned>(MAX_VERIFY_ATTEMPTS));
            SetError(SettingsStoreError::VerifyFailed,
                     "verify tmp file failed (hash/size mismatch or read failed)",
                     verificationEvidence);
            return false;
        }
    }

    if (LittleFsPathExists(
            m_path, storage)) {
        if (LittleFsPathExists(
                backupPath, storage)) {
            (void)RemoveLittleFsPath(
                backupPath,
                storage);
        }
        if (!RenameLittleFsPath(
                m_path,
                backupPath,
                storage)) {
            // Preserve verified staging bytes on promotion failure. When
            // staging uses .retry, the retained .tmp remains boot authority.
            SetError(SettingsStoreError::WriteFailed, "rename main->bak failed");
            return false;
        }
    }

    if (!RenameLittleFsPath(
            tmpPath,
            m_path,
            storage)) {
        if (LittleFsPathExists(
                backupPath, storage)) {
            (void)RenameLittleFsPath(
                backupPath,
                m_path,
                storage);
        }
        SetError(SettingsStoreError::WriteFailed, "rename tmp->main failed");
        return false;
    }

    if (preservePending && !RemoveLittleFsPath(
            retainedTmpPath, storage) &&
        ProbePathWithAccess(retainedTmpPath, storage) !=
            SettingsPathState::Missing) {
        SetError(SettingsStoreError::WriteFailed, "new primary saved but old pending removal failed");
        return false;
    }

    saveTotalGuard.Commit();
    m_lastSaveSizeBytes = written;
    ++m_saveCount;
    const uint32_t totalSaves = m_doc[SETTINGS_SAVE_TOTAL_KEY] | 0U;
    fxe::Trace("settings saved: bytes=%u saves_since_boot=%u saves_total=%u%s",
               static_cast<unsigned>(written),
               static_cast<unsigned>(m_saveCount),
               static_cast<unsigned>(totalSaves),
               verified ? " (verified)" : "");
    return true;
#else
    char retainedTmpPath[SETTINGS_STORE_PATH_CAPACITY] = {};
    char retryPath[SETTINGS_STORE_PATH_CAPACITY] = {};
    char backupPath[SETTINGS_STORE_PATH_CAPACITY] = {};
    if (!MakeDerivedPath(retainedTmpPath, sizeof(retainedTmpPath), m_path, ".tmp") ||
        !MakeDerivedPath(backupPath, sizeof(backupPath), m_path, ".bak")) {
        SetError(SettingsStoreError::Unsupported, "settings path too long for tmp/bak");
        return false;
    }

    const bool preservePending = ProbePathWithAccess(
        retainedTmpPath, storage) != SettingsPathState::Missing;
    if (preservePending && !MakeDerivedPath(retryPath, sizeof(retryPath), m_path, ".retry")) {
        SetError(SettingsStoreError::Unsupported, "settings path too long for retry scratch");
        return false;
    }
    const char* tmpPath = preservePending ? retryPath : retainedTmpPath;
    storage.staging = storage.Role(tmpPath);

    static constexpr uint8_t MAX_VERIFY_ATTEMPTS = 2;
    size_t written = 0;
    uint32_t expectedHash = 0;
    bool verified = false;
    auto verificationEvidence = StorageFailureEvidence::Unconfirmed;
    SettingsSaveTotalGuard saveTotalGuard(m_doc);

    for (uint8_t attempt = 0; attempt < MAX_VERIFY_ATTEMPTS; ++attempt) {
        const size_t required = measureJson(m_doc);
        if (required == 0 || required >= SETTINGS_STORE_MAX_FILE_SIZE_BYTES) {
            SetError(SettingsStoreError::SerializeFailed, "settings JSON too large");
            return false;
        }

        if (!EnsureParentDirsExistWithAccess(
                tmpPath, storage)) {
            SetError(SettingsStoreError::OpenWriteFailed, "mkdir failed");
            return false;
        }
        FILE* f = OpenStdioFile(
            tmpPath, "wb", storage);
        if (!f) {
            SetError(SettingsStoreError::OpenWriteFailed, "open(write tmp) failed");
            return false;
        }

        HashingFileWriter writer(
            f, storage);
        written = serializeJson(m_doc, writer);
        const bool flushed = FlushStdioFile(
            f, storage);
        const bool closed = CloseStdioFile(
            f, storage);
        if (writer.shortWrite || writer.bytesWritten != written || written != required ||
            !flushed || !closed) {
            (void)RemoveStdioPath(
                tmpPath, storage);
            const bool ioFailed = writer.shortWrite || !flushed || !closed;
            SetError(ioFailed ? SettingsStoreError::WriteFailed : SettingsStoreError::SerializeFailed,
                     "incomplete settings write/serialization", ioFailed
                         ? StorageFailureEvidence::Confirmed : StorageFailureEvidence::Unconfirmed);
            return false;
        }
        if (written == 0) {
            (void)RemoveStdioPath(
                tmpPath, storage);
            SetError(SettingsStoreError::SerializeFailed,
                     "serialize/write produced 0 bytes");
            return false;
        }

        expectedHash = writer.hash;

        if (!m_cfg.verifyWrites) {
            break;
        }

        uint32_t actualHash = 0;
        size_t actualBytes = 0;
        auto attemptEvidence = StorageFailureEvidence::Unconfirmed;
        if (TryVerifyTmpFileHashStdio(
                tmpPath,
                expectedHash,
                written,
                actualHash,
                actualBytes,
                attemptEvidence,
                storage)) {
            verified = true;
            if (attempt > 0) {
                fxe::Warn(
                    "settings verify ok after retry: attempt=%u/%u bytes=%u hash=%08x",
                    static_cast<unsigned>(attempt + 1),
                    static_cast<unsigned>(MAX_VERIFY_ATTEMPTS),
                    static_cast<unsigned>(written),
                    static_cast<unsigned>(expectedHash));
            }
            break;
        }

        if (attemptEvidence == StorageFailureEvidence::Confirmed) {
            verificationEvidence = attemptEvidence;
        }
        fxe::Warn(
            "settings verify failed: attempt=%u/%u expectedBytes=%u actualBytes=%u expectedHash=%08x actualHash=%08x",
            static_cast<unsigned>(attempt + 1),
            static_cast<unsigned>(MAX_VERIFY_ATTEMPTS),
            static_cast<unsigned>(written),
            static_cast<unsigned>(actualBytes),
            static_cast<unsigned>(expectedHash),
            static_cast<unsigned>(actualHash));

        (void)RemoveStdioPath(
            tmpPath, storage);
        if (attempt + 1 >= MAX_VERIFY_ATTEMPTS) {
            fxe::Error("settings verify failed after %u attempts; save aborted",
                       static_cast<unsigned>(MAX_VERIFY_ATTEMPTS));
            SetError(SettingsStoreError::VerifyFailed,
                     "verify tmp file failed (hash/size mismatch or read failed)",
                     verificationEvidence);
            return false;
        }
    }

    if (StdioPathExists(
            m_path, storage)) {
        (void)RemoveStdioPath(
            backupPath, storage);
        if (!RenameStdioPath(
                m_path,
                backupPath,
                storage)) {
            // Preserve the verified candidate across a failed promotion.
            SetError(SettingsStoreError::WriteFailed, "rename main->bak failed");
            return false;
        }
    }

    if (!RenameStdioPath(
            tmpPath,
            m_path,
            storage)) {
        (void)RenameStdioPath(
            backupPath,
            m_path,
            storage);
        SetError(SettingsStoreError::WriteFailed, "rename tmp->main failed");
        return false;
    }

    if (preservePending && !RemoveStdioPath(
            retainedTmpPath, storage) &&
        ProbePathWithAccess(retainedTmpPath, storage) !=
            SettingsPathState::Missing) {
        SetError(SettingsStoreError::WriteFailed, "new primary saved but old pending removal failed");
        return false;
    }

    saveTotalGuard.Commit();
    m_lastSaveSizeBytes = written;
    ++m_saveCount;
    const uint32_t totalSaves = m_doc[SETTINGS_SAVE_TOTAL_KEY] | 0U;
    fxe::Trace("settings saved: bytes=%u saves_since_boot=%u saves_total=%u%s",
               static_cast<unsigned>(written),
               static_cast<unsigned>(m_saveCount),
               static_cast<unsigned>(totalSaves),
               verified ? " (verified)" : "");
    return true;
#endif
}

} // namespace fxe
