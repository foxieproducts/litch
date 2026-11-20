#include "firmware/current_game_log_storage.hpp"

#ifdef ARDUINO

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include <LittleFS.h>
#include <esp_littlefs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "foxie_embedded/fixed_string.hpp"
#include "runtime/progress_watchdog.hpp"

namespace firmware {
namespace {

using modules::current_game_log::Slot;
using modules::current_game_log::StorageStatus;

constexpr const char* SlotPath(const Slot slot) {
    if (slot == Slot::A) return "/current_game_a.bin";
    if (slot == Slot::B) return "/current_game_b.bin";
    return nullptr;
}

constexpr const char* TemporaryPath(const Slot slot) {
    if (slot == Slot::A) return "/current_game_a.tmp";
    if (slot == Slot::B) return "/current_game_b.tmp";
    return nullptr;
}

bool MountedPath(const char* const path, fxe::FixedString<64U>& out) {
    return path != nullptr && out.Assign("/littlefs") && out.Append(path);
}

bool WriteComplete(File& file,
                   const uint8_t* const bytes,
                   const size_t length) {
    if (length != 0U &&
        (bytes == nullptr || file.write(bytes, length) != length)) {
        return false;
    }
    file.flush();
    return true;
}

} // namespace

CurrentGameLogStorage::FlashLease::FlashLease(
    CurrentGameLogStorage& storage)
    : m_storage(storage) {
    auto& worker = storage.m_worker;
    if (!worker.IsStarted() || xTaskGetCurrentTaskHandle() != worker.m_task) {
        return;
    }
    worker.AcquireFlashAccessWindow();
    m_acquired = true;
}

CurrentGameLogStorage::FlashLease::~FlashLease() {
    if (m_acquired) m_storage.m_worker.ReleaseFlashAccessWindow();
}

modules::current_game_log::StorageOps CurrentGameLogStorage::Ops() {
    return {this, &Stat, &Read, &Replace, &Append, &Truncate,
            &BeginBatch, &EndBatch};
}

bool CurrentGameLogStorage::BeginBatch(void* const context) {
    auto* const self = static_cast<CurrentGameLogStorage*>(context);
    if (self == nullptr || self->m_batchActive ||
        !self->m_worker.IsStarted() ||
        xTaskGetCurrentTaskHandle() != self->m_worker.m_task) {
        return false;
    }
    self->m_batchActive = true;
    return true;
}

void CurrentGameLogStorage::CloseBatchRead() {
    m_batchReadLength = 0U;
    if (m_batchReadFile) m_batchReadFile.close();
}

void CurrentGameLogStorage::CompleteBatchOperation() {
    if (m_batchActive) runtime::FeedProgressWatchdog();
}

void CurrentGameLogStorage::NoteIoFailure(const fxe::SettingsStoreError error) {
    if (errno == EIO) m_worker.NoteSnapshotStorageFailure(error);
}

void CurrentGameLogStorage::EndBatch(void* const context) {
    auto* const self = static_cast<CurrentGameLogStorage*>(context);
    if (self == nullptr || !self->m_batchActive) return;
    const FlashLease lease(*self);
    if (!lease.IsAcquired()) return;
    self->CloseBatchRead();
    self->m_batchActive = false;
}

StorageStatus CurrentGameLogStorage::Stat(
    void* const context,
    const Slot slot,
    uint32_t& outSize) {
    outSize = 0U;
    auto* const self = static_cast<CurrentGameLogStorage*>(context);
    if (self == nullptr) return StorageStatus::IoFailure;
    const FlashLease lease(*self);
    if (!lease.IsAcquired() || !esp_littlefs_mounted("spiffs")) {
        return StorageStatus::IoFailure;
    }
    fxe::FixedString<64U> path;
    if (!MountedPath(SlotPath(slot), path)) return StorageStatus::IoFailure;
    struct stat info {};
    errno = 0;
    if (::stat(path.CStr(), &info) != 0) {
        self->NoteIoFailure(fxe::SettingsStoreError::ReadFailed);
        return errno == ENOENT ? StorageStatus::Missing
                               : StorageStatus::IoFailure;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > UINT32_MAX) {
        return StorageStatus::IoFailure;
    }
    outSize = static_cast<uint32_t>(info.st_size);
    self->CompleteBatchOperation();
    return StorageStatus::Present;
}

bool CurrentGameLogStorage::Read(
    void* const context,
    const Slot slot,
    const uint32_t offset,
    uint8_t* const out,
    const size_t length) {
    auto* const self = static_cast<CurrentGameLogStorage*>(context);
    if (self == nullptr || (out == nullptr && length != 0U)) return false;
    const char* const path = SlotPath(slot);
    if (path == nullptr) return false;
    if (self->m_batchActive && self->m_worker.IsStarted() &&
        xTaskGetCurrentTaskHandle() == self->m_worker.m_task &&
        self->m_batchReadSlot == slot &&
        length != 0U && offset >= self->m_batchReadOffset &&
        offset - self->m_batchReadOffset <= self->m_batchReadLength &&
        length <= self->m_batchReadLength -
            (offset - self->m_batchReadOffset)) {
        std::memcpy(out, self->m_batchReadBytes.data() +
                    (offset - self->m_batchReadOffset), length);
        self->CompleteBatchOperation();
        return true;
    }
    const FlashLease lease(*self);
    if (!lease.IsAcquired()) return false;
    File localFile;
    File* file = nullptr;
    if (self->m_batchActive) {
        if (!self->m_batchReadFile || self->m_batchReadSlot != slot) {
            self->CloseBatchRead();
            errno = 0;
            self->m_batchReadFile = LittleFS.open(path, "r");
            self->m_batchReadSlot = slot;
        }
        file = &self->m_batchReadFile;
    } else {
        errno = 0;
        localFile = LittleFS.open(path, "r");
        file = &localFile;
    }
    if (!*file) {
        self->NoteIoFailure(fxe::SettingsStoreError::ReadFailed);
        return false;
    }
    const size_t size = file->size();
    if (offset > size || length > size - offset) {
        if (!self->m_batchActive) file->close();
        return false;
    }
    errno = 0;
    if (!file->seek(offset, SeekSet)) {
        self->NoteIoFailure(fxe::SettingsStoreError::ReadFailed);
        if (!self->m_batchActive) file->close();
        return false;
    }
    bool complete = true;
    if (self->m_batchActive && length != 0U &&
        length <= self->m_batchReadBytes.size()) {
        self->m_batchReadLength = 0U;
        const size_t count = std::min<size_t>(self->m_batchReadBytes.size(),
                                            size - offset);
        complete = file->read(self->m_batchReadBytes.data(), count) == count;
        if (complete) {
            self->m_batchReadOffset = offset;
            self->m_batchReadLength = count;
            std::memcpy(out, self->m_batchReadBytes.data(), length);
        }
    } else {
        complete = length == 0U || file->read(out, length) == length;
    }
    if (!complete) self->m_worker.NoteSnapshotStorageFailure(fxe::SettingsStoreError::ReadFailed);
    if (!self->m_batchActive) file->close();
    self->CompleteBatchOperation();
    return complete;
}

bool CurrentGameLogStorage::Replace(
    void* const context,
    const Slot slot,
    const uint8_t* const bytes,
    const size_t length) {
    auto* const self = static_cast<CurrentGameLogStorage*>(context);
    if (self == nullptr || (bytes == nullptr && length != 0U)) return false;
    const char* const temporary = TemporaryPath(slot);
    const char* const path = SlotPath(slot);
    if (temporary == nullptr || path == nullptr) return false;
    const FlashLease lease(*self);
    if (!lease.IsAcquired()) return false;
    self->CloseBatchRead();
    errno = 0;
    File file = LittleFS.open(temporary, "w");
    if (!file) {
        self->NoteIoFailure(fxe::SettingsStoreError::WriteFailed);
        return false;
    }
    const bool written = WriteComplete(file, bytes, length);
    errno = 0;
    const size_t writtenSize = file.size();
    const int sizeError = errno;
    file.close();
    if (sizeError == EIO) self->m_worker.NoteSnapshotStorageFailure(fxe::SettingsStoreError::ReadFailed);
    if (!written || writtenSize != length) {
        if (!written) self->m_worker.NoteSnapshotStorageFailure(fxe::SettingsStoreError::WriteFailed);
        // Arduino size() can return cached metadata after a resource/stat
        // refusal. Size alone is not a completed byte readback comparison.
        return false;
    }
    errno = 0;
    const bool promoted = LittleFS.rename(temporary, path);
    if (!promoted) self->NoteIoFailure(fxe::SettingsStoreError::WriteFailed);
    self->CompleteBatchOperation();
    return promoted;
}

bool CurrentGameLogStorage::Append(
    void* const context,
    const Slot slot,
    const uint8_t* const bytes,
    const size_t length) {
    auto* const self = static_cast<CurrentGameLogStorage*>(context);
    if (self == nullptr || bytes == nullptr || length == 0U) return false;
    const char* const path = SlotPath(slot);
    if (path == nullptr) return false;
    const FlashLease lease(*self);
    if (!lease.IsAcquired()) return false;
    self->CloseBatchRead();
    errno = 0;
    File file = LittleFS.open(path, "a");
    if (!file) {
        self->NoteIoFailure(fxe::SettingsStoreError::WriteFailed);
        return false;
    }
    const size_t before = file.size();
    if (before > UINT32_MAX || length > UINT32_MAX - before) {
        file.close();
        return false;
    }
    const bool written = WriteComplete(file, bytes, length);
    errno = 0;
    const size_t after = file.size();
    const int sizeError = errno;
    file.close();
    if (sizeError == EIO) self->m_worker.NoteSnapshotStorageFailure(fxe::SettingsStoreError::ReadFailed);
    const bool complete = written && after == before + length;
    if (!written) self->m_worker.NoteSnapshotStorageFailure(fxe::SettingsStoreError::WriteFailed);
    self->CompleteBatchOperation();
    return complete;
}

bool CurrentGameLogStorage::Truncate(
    void* const context,
    const Slot slot,
    const uint32_t length) {
    auto* const self = static_cast<CurrentGameLogStorage*>(context);
    if (self == nullptr) return false;
    const FlashLease lease(*self);
    if (!lease.IsAcquired()) return false;
    self->CloseBatchRead();
    fxe::FixedString<64U> path;
    if (!MountedPath(SlotPath(slot), path)) return false;
    errno = 0;
    const bool truncated = ::truncate(path.CStr(), static_cast<off_t>(length)) == 0;
    if (!truncated) self->NoteIoFailure(fxe::SettingsStoreError::WriteFailed);
    self->CompleteBatchOperation();
    return truncated;
}

} // namespace firmware

#endif
