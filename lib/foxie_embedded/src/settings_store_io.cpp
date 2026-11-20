#include "settings_store_internal.hpp"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>

#include "foxie_embedded/format.hpp"
#include "foxie_embedded/settings_store.hpp"

#include "foxie_embedded/logger.hpp"

#ifdef ARDUINO
#include <LittleFS.h>

#include <esp_attr.h>
#include <esp_littlefs.h>
#include <esp_partition.h>

#include <fcntl.h>
#include <sys/reent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {
ssize_t esp_vfs_read(struct _reent* r, int fd, void* dst, size_t size);
int esp_vfs_open(struct _reent* r, const char* path, int flags, int mode);
int esp_vfs_close(struct _reent* r, int fd);
int esp_vfs_fstat(struct _reent* r, int fd, struct stat* st);
}
#endif

namespace fxe {

namespace {

static constexpr size_t SETTINGS_STREAM_BUFFER_CAPACITY = 256;

template <typename TSource>
class BoundedJsonReader final {
  public:
    BoundedJsonReader(TSource& source, const size_t fileSize,
                      SettingsReadObservation* observation = nullptr)
        : m_source(source), m_observation(observation), m_remaining(fileSize) {}

    int read() {
        if (!EnsureAvailable()) {
            return -1;
        }
        return static_cast<unsigned char>(m_buffer[m_pos++]);
    }

    size_t readBytes(char* out, const size_t length) {
        size_t copied = 0;
        while (copied < length && EnsureAvailable()) {
            const size_t available = m_end - m_pos;
            const size_t requested = length - copied;
            const size_t amount = (available < requested) ? available : requested;
            std::memcpy(out + copied, m_buffer + m_pos, amount);
            m_pos += amount;
            copied += amount;
        }
        return copied;
    }

    bool ConsumeRemaining() {
        while (read() >= 0) {
        }
        return !m_readFailed && m_remaining == 0;
    }

  private:
    bool EnsureAvailable() {
        if (m_pos < m_end) {
            return true;
        }
        if (m_remaining == 0 || m_readFailed) {
            return false;
        }

        const size_t requested = (m_remaining < sizeof(m_buffer))
                                     ? m_remaining
                                     : sizeof(m_buffer);
        const size_t received = m_source.Read(m_buffer, requested);
        if (received == 0 || received > requested) {
            m_readFailed = true;
            return false;
        }

        if (m_observation != nullptr) {
            m_observation->bytes += received;
            for (size_t index = 0U; index < received; ++index) {
                m_observation->fnv32 ^= static_cast<uint8_t>(m_buffer[index]);
                m_observation->fnv32 *= 16777619U;
            }
            if (m_observation->chunkHandoff != nullptr && *m_observation->chunkHandoff) {
                (*m_observation->chunkHandoff)();
            }
        }
        m_remaining -= received;
        m_pos = 0;
        m_end = received;
        return true;
    }

    TSource& m_source;
    SettingsReadObservation* m_observation = nullptr;
    size_t m_remaining = 0;
    size_t m_pos = 0;
    size_t m_end = 0;
    bool m_readFailed = false;
    char m_buffer[SETTINGS_STREAM_BUFFER_CAPACITY] = {};
};

#ifdef ARDUINO

class FdReadSource final {
  public:
    FdReadSource(struct _reent* reent, const int fd)
        : m_reent(reent), m_fd(fd) {}

    size_t Read(char* out, const size_t requested) {
        const ssize_t n = esp_vfs_read(m_reent, m_fd, out, requested);
        return (n > 0) ? static_cast<size_t>(n) : 0;
    }

  private:
    struct _reent* m_reent = nullptr;
    int m_fd = -1;
};

bool MakeMountedPath(char* out, const size_t outCap, const char* path) {
    if (out == nullptr || outCap == 0 || path == nullptr || path[0] == '\0') {
        return false;
    }

    const bool pathHasLeadingSlash = path[0] == '/';
    size_t pos = 0;
    bool truncated = false;
    fxe::format::AppendStr(out, outCap, pos, SETTINGS_FS_BASE_PATH, truncated);
    fxe::format::AppendChar(out, outCap, pos, '/', truncated);
    fxe::format::AppendStr(out, outCap, pos, pathHasLeadingSlash ? (path + 1) : path, truncated);
    return !truncated;
}


bool ReadPartition(const void* const context, const size_t offset,
                   uint8_t* const out, const size_t size) {
    if (context == nullptr) return false;
    const auto& partition = *static_cast<const esp_partition_t*>(context);
    return esp_partition_read(&partition, offset, out, size) == ESP_OK;
}

bool IsFullyErasedPartition(const esp_partition_t& partition) {
    // Mount failure is rare and boot-only. A bounded buffer keeps task-stack
    // use small while the complete 1.6 MiB settings partition is classified.
    uint8_t scratch[512U] = {};
    return ClassifyPartitionErasure(
               partition.size, ReadPartition, &partition, scratch,
               sizeof(scratch)) == PartitionErasure::Erased;
}

#else

class FileReadSource final {
  public:
    explicit FileReadSource(std::FILE* file)
        : m_file(file) {}

    size_t Read(char* out, const size_t requested) {
        return std::fread(out, 1, requested, m_file);
    }

  private:
    std::FILE* m_file = nullptr;
};

#endif

} // namespace

#ifdef ARDUINO
int OpenSettingsWriteFile(const char* path) {
    char mountedPath[96] = {};
    if (!MakeMountedPath(mountedPath, sizeof(mountedPath), path)) {
        return -1;
    }
    return ::open(mountedPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
}

size_t WriteSettingsFile(const int fd, const uint8_t* data, const size_t size) {
    const ssize_t written = ::write(fd, data, size);
    return written > 0 ? static_cast<size_t>(written) : 0U;
}

bool SyncSettingsFile(const int fd) {
    return ::fsync(fd) == 0;
}

bool CloseSettingsFile(const int fd) {
    return ::close(fd) == 0;
}

bool RemoveSettingsFile(const char* path) {
    char mountedPath[96] = {};
    if (!MakeMountedPath(mountedPath, sizeof(mountedPath), path)) return false;
    return ::unlink(mountedPath) == 0;
}

bool RenameSettingsFile(const char* from, const char* to) {
    char mountedFrom[96] = {}, mountedTo[96] = {};
    if (!MakeMountedPath(mountedFrom, sizeof(mountedFrom), from) ||
        !MakeMountedPath(mountedTo, sizeof(mountedTo), to)) return false;
    return std::rename(mountedFrom, mountedTo) == 0;
}
#endif

SettingsPathState ProbeSettingsPath(const char* path) {
    if (path == nullptr || path[0] == '\0') return SettingsPathState::Unknown;
#ifdef ARDUINO
    char mountedPath[96] = {};
    if (!MakeMountedPath(mountedPath, sizeof(mountedPath), path)) {
        return SettingsPathState::Unknown;
    }
    path = mountedPath;
#endif
    struct stat state = {};
    if (::stat(path, &state) == 0) return SettingsPathState::Present;
    return errno == ENOENT ? SettingsPathState::Missing : SettingsPathState::Unknown;
}

static JsonLoadResult LoadJsonOnce(const char* path, JsonDocument& outDoc,
                                  int& systemError, SettingsReadObservation* observation) {
    systemError = 0;
    errno = 0;
#ifdef ARDUINO
    if (path == nullptr || path[0] == '\0') {
        return JsonLoadResult::Error;
    }

    // Important: avoid Arduino's File/newlib stdio buffering path here.
    char mountedPath[96] = {};
    if (!MakeMountedPath(mountedPath, sizeof(mountedPath), path)) {
        return JsonLoadResult::Error;
    }

    struct _reent* reent = __getreent();
    const int fd = esp_vfs_open(reent, mountedPath, O_RDONLY, 0);
    if (fd < 0) {
        systemError = reent->_errno;
        return systemError == ENOENT ? JsonLoadResult::Missing : JsonLoadResult::OpenError;
    }

    struct stat st = {};
    if (esp_vfs_fstat(reent, fd, &st) != 0) {
        systemError = reent->_errno;
        (void)esp_vfs_close(reent, fd);
        fxe::Warn("settings: fstat failed: path=%s", path);
        return JsonLoadResult::ReadError;
    }
    if (st.st_size == 0) {
        (void)esp_vfs_close(reent, fd);
        return JsonLoadResult::Empty;
    }
    if (st.st_size < 0 || static_cast<size_t>(st.st_size) >= fxe::SETTINGS_STORE_MAX_FILE_SIZE_BYTES) {
        (void)esp_vfs_close(reent, fd);
        fxe::Warn("settings: file too large: path=%s size=%d cap=%u",
                  path,
                  static_cast<int>(st.st_size),
                  static_cast<unsigned>(fxe::SETTINGS_STORE_MAX_FILE_SIZE_BYTES));
        return JsonLoadResult::TooLarge;
    }

    const size_t expected = static_cast<size_t>(st.st_size);
    FdReadSource source(reent, fd);
    BoundedJsonReader<FdReadSource> reader(source, expected, observation);
    const DeserializationError err = deserializeJson(outDoc, reader);
    const bool fullyRead = reader.ConsumeRemaining();
    if (!fullyRead) systemError = reent->_errno;

    (void)esp_vfs_close(reent, fd);

    if (!fullyRead) {
        fxe::Warn("settings: read failed: path=%s expected=%u",
                  path,
                  static_cast<unsigned>(expected));
        return JsonLoadResult::ReadError;
    }
    if (err) {
        fxe::Warn("settings: deserialize failed: path=%s bytes=%u err=%s",
                  path,
                  static_cast<unsigned>(expected),
                  err.c_str());
        return err == DeserializationError::NoMemory ? JsonLoadResult::NoMemory : JsonLoadResult::Error;
    }
    return JsonLoadResult::Ok;
#else
    if (path == nullptr || path[0] == '\0') {
        return JsonLoadResult::Error;
    }

    FILE* f = std::fopen(path, "rb");
    if (!f) {
        systemError = errno;
        return systemError == ENOENT ? JsonLoadResult::Missing : JsonLoadResult::OpenError;
    }

    struct stat fileState = {};
    if (::fstat(::fileno(f), &fileState) != 0) {
        systemError = errno;
        (void)std::fclose(f);
        return JsonLoadResult::ReadError;
    }
    // A directory can be seekable but is not an oversized settings document.
    if (S_ISDIR(fileState.st_mode)) {
        systemError = EISDIR;
        (void)std::fclose(f);
        return JsonLoadResult::ReadError;
    }

    if (std::fseek(f, 0, SEEK_END) != 0) {
        systemError = errno;
        (void)std::fclose(f);
        return JsonLoadResult::ReadError;
    }
    const long end = std::ftell(f);
    if (end < 0 || std::fseek(f, 0, SEEK_SET) != 0) {
        systemError = errno;
        (void)std::fclose(f);
        return JsonLoadResult::ReadError;
    }
    if (end == 0) {
        (void)std::fclose(f);
        return JsonLoadResult::Empty;
    }
    const size_t expected = static_cast<size_t>(end);
    if (expected >= fxe::SETTINGS_STORE_MAX_FILE_SIZE_BYTES) {
        (void)std::fclose(f);
        fxe::Warn("settings: file too large: path=%s size=%u cap=%u",
                  path,
                  static_cast<unsigned>(expected),
                  static_cast<unsigned>(fxe::SETTINGS_STORE_MAX_FILE_SIZE_BYTES));
        return JsonLoadResult::TooLarge;
    }

    FileReadSource source(f);
    BoundedJsonReader<FileReadSource> reader(source, expected, observation);
    const DeserializationError err = deserializeJson(outDoc, reader);
    const bool fullyRead = reader.ConsumeRemaining();
    if (!fullyRead) systemError = errno;
    (void)std::fclose(f);

    if (!fullyRead) {
        fxe::Warn("settings: read failed: path=%s expected=%u",
                  path,
                  static_cast<unsigned>(expected));
        return JsonLoadResult::ReadError;
    }
    if (err) {
        fxe::Warn("settings: deserialize failed: path=%s bytes=%u err=%s",
                  path,
                  static_cast<unsigned>(expected),
                  err.c_str());
        return err == DeserializationError::NoMemory ? JsonLoadResult::NoMemory : JsonLoadResult::Error;
    }
    return JsonLoadResult::Ok;
#endif
}

JsonLoadResult LoadJsonFromPath(const char* path, JsonDocument& outDoc,
                              const SettingsReadAccess access) {
    constexpr uint8_t MAX_READ_ATTEMPTS = 3U;
    JsonLoadResult result = JsonLoadResult::Error;
    for (uint8_t attempt = 0U; attempt < MAX_READ_ATTEMPTS; ++attempt) {
        outDoc.clear();
        if (access.before != nullptr && *access.before) (*access.before)();
        int systemError = 0;
        if (access.observation != nullptr) {
            access.observation->bytes = 0U;
            access.observation->fnv32 = 2166136261U;
        }
        result = LoadJsonOnce(path, outDoc, systemError, access.observation);
        if ((result == JsonLoadResult::OpenError || result == JsonLoadResult::ReadError) &&
            systemError == EIO && access.confirmedIoFailure != nullptr) {
            access.confirmedIoFailure->store(true, std::memory_order_release);
        }
        if (access.after != nullptr && *access.after) (*access.after)();
        if (result != JsonLoadResult::OpenError && result != JsonLoadResult::ReadError) {
            return result;
        }
        fxe::Warn("settings: load path=%s stage=%s errno=%d attempt=%u/%u",
                  path, result == JsonLoadResult::OpenError ? "open" : "read",
                  systemError, unsigned(attempt + 1U), unsigned(MAX_READ_ATTEMPTS));
    }
    return result;
}

bool SettingsStore::ReadPrimaryForDiagnostics(size_t& outBytes, uint32_t& outFnv32) {
    SettingsReadObservation observed{};
    StorageAccessCallback handoff = [this]() {
        if (m_beforeStorageAccessCb && m_afterStorageAccessCb) {
            m_afterStorageAccessCb();
            m_beforeStorageAccessCb();
        }
    };
    observed.chunkHandoff = &handoff;
    const SettingsReadAccess access{
        &m_beforeStorageAccessCb, &m_afterStorageAccessCb, nullptr, &observed};
    const auto result = LoadJsonFromPath(m_path, m_doc, access);
    outBytes = observed.bytes;
    outFnv32 = observed.fnv32;
    return result == JsonLoadResult::Ok && m_doc.is<JsonObject>() && !m_doc.overflowed();
}

#ifdef ARDUINO

bool TryMountLittleFsNoAutoFormat() {
    return LittleFS.begin(false,
                          SETTINGS_FS_BASE_PATH,
                          SETTINGS_FS_MAX_OPEN_FILES,
                          SETTINGS_FS_PARTITION_LABEL);
}

bool TryMountLittleFsFormatOnFail() {
    return LittleFS.begin(true,
                          SETTINGS_FS_BASE_PATH,
                          SETTINGS_FS_MAX_OPEN_FILES,
                          SETTINGS_FS_PARTITION_LABEL);
}


bool TryMountLittleFsErasedPartitionSafe(const bool forceFormat,
    const bool allowErasedInitialization, bool* const erasedStorage) {
    if (erasedStorage != nullptr) *erasedStorage = false;
    if (forceFormat) return ReformatLittleFsPartition();

    if (TryMountLittleFsNoAutoFormat()) {
        return true;
    }

    // After a flash erase, the partition can be all 0xFF and LittleFS won't mount.
    // In that case, formatting is required to initialize the filesystem.
    bool partitionAppearsErased = false;
    if (!forceFormat) {
        const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                               ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                                               SETTINGS_FS_PARTITION_LABEL);
        partitionAppearsErased =
            (part != nullptr) && IsFullyErasedPartition(*part);
    }

    if (!forceFormat && !partitionAppearsErased) {
        // Conservative: do not auto-format on generic mount failures.
        return false;
    }

    if (erasedStorage != nullptr) *erasedStorage = partitionAppearsErased;
    if (!forceFormat && !allowErasedInitialization) return false;

    fxe::Warn("settings: LittleFS formatting due to %s",
              forceFormat ? "crash loop" : "erased partition");

    if (TryMountLittleFsFormatOnFail()) {
        return true;
    }

    return false;
}

#endif

} // namespace fxe
