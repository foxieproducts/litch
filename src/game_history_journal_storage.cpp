#include "firmware/game_history_journal_storage.hpp"

#include <cerrno>
#include <sys/stat.h>
#include "modules/game_history_journal.hpp"

namespace firmware {
namespace {
// Kept at the real filesystem seam: absence permits initialization, while any
// other failure must preserve the existing journal identity and state slots.
modules::game_history::JournalStorageStatus ProbeJournalFile(
    const char* path, uint32_t& outSize) {
    using modules::game_history::JournalStorageStatus;
    outSize = 0U;
    struct stat info {};
    if (::stat(path, &info) != 0) {
        return errno == ENOENT ? JournalStorageStatus::Missing
                              : JournalStorageStatus::IoFailure;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > UINT32_MAX) {
        return JournalStorageStatus::IoFailure;
    }
    outSize = static_cast<uint32_t>(info.st_size);
    return JournalStorageStatus::Present;
}
} // namespace
} // namespace firmware

#ifdef ARDUINO

#include <LittleFS.h>
#include <esp_littlefs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>

namespace firmware {
namespace {

using modules::game_history::JournalArtifact;
using modules::game_history::JournalStorageStatus;

constexpr const char* ArtifactPath(const JournalArtifact artifact) {
    switch (artifact) {
        case JournalArtifact::Journal:
            return "/game_history.journal";
        case JournalArtifact::Compaction:
            return "/game_history.compact";
        case JournalArtifact::StateA:
            return "/game_history.state_a";
        case JournalArtifact::StateB:
            return "/game_history.state_b";
    }
    return nullptr;
}

constexpr const char* TemporaryPath(const JournalArtifact artifact) {
    switch (artifact) {
        case JournalArtifact::Journal:
            return "/game_history.journal.tmp";
        case JournalArtifact::Compaction:
            return "/game_history.compact.tmp";
        case JournalArtifact::StateA:
            return "/game_history.state_a.tmp";
        case JournalArtifact::StateB:
            return "/game_history.state_b.tmp";
    }
    return nullptr;
}

bool WriteComplete(File& file,
                   const uint8_t* const bytes,
                   const size_t length) {
    if (length == 0U) {
        file.flush();
        return true;
    }
    if (bytes == nullptr || file.write(bytes, length) != length) {
        return false;
    }
    file.flush();
    return true;
}

} // namespace

GameHistoryJournalStorage::FlashLease::FlashLease(SettingsPersistenceWorker& worker)
    : m_worker(worker) {
    if (!worker.IsStarted() || xTaskGetCurrentTaskHandle() != worker.m_task) return;
    worker.AcquireFlashAccessWindow();
    m_acquired = true;
}

GameHistoryJournalStorage::FlashLease::~FlashLease() {
    if (m_acquired) m_worker.ReleaseFlashAccessWindow();
}

modules::game_history::GameHistoryJournalStorageOps
GameHistoryJournalStorage::Ops() {
    return {this, &Stat, &Read, &Replace, &Append, &Promote};
}

JournalStorageStatus GameHistoryJournalStorage::Stat(
    void* const context,
    const JournalArtifact artifact,
    uint32_t& outSize) {
    outSize = 0U;
    auto* const self = static_cast<GameHistoryJournalStorage*>(context);
    const char* const path = ArtifactPath(artifact);
    if (self == nullptr || path == nullptr) {
        return JournalStorageStatus::IoFailure;
    }
    const FlashLease lease(self->m_worker);
    if (!lease.IsAcquired()) return JournalStorageStatus::IoFailure;
    // SettingsStore owns this mount at /littlefs with partition label spiffs.
    // An unmounted filesystem must never look like an empty new journal.
    if (!esp_littlefs_mounted("spiffs")) return JournalStorageStatus::IoFailure;
    fxe::FixedString<64U> mounted;
    if (!mounted.Assign("/littlefs") || !mounted.Append(path)) {
        return JournalStorageStatus::IoFailure;
    }
    return ProbeJournalFile(mounted.CStr(), outSize);
}

bool GameHistoryJournalStorage::Read(
    void* const context,
    const JournalArtifact artifact,
    const uint32_t offset,
    uint8_t* const out,
    const size_t length) {
    auto* const self = static_cast<GameHistoryJournalStorage*>(context);
    const char* const path = ArtifactPath(artifact);
    if (self == nullptr || path == nullptr ||
        (out == nullptr && length != 0U)) {
        return false;
    }
    const FlashLease lease(self->m_worker);
    if (!lease.IsAcquired()) return false;
    File file = LittleFS.open(path, "r");
    if (!file || offset > file.size() ||
        length > file.size() - offset || !file.seek(offset, SeekSet)) {
        if (file) file.close();
        return false;
    }
    const bool complete =
        length == 0U || file.read(out, length) == length;
    file.close();
    return complete;
}

bool GameHistoryJournalStorage::Replace(
    void* const context,
    const JournalArtifact artifact,
    const uint8_t* const bytes,
    const size_t length) {
    auto* const self = static_cast<GameHistoryJournalStorage*>(context);
    const char* const path = ArtifactPath(artifact);
    const char* const temporary = TemporaryPath(artifact);
    if (self == nullptr || path == nullptr || temporary == nullptr ||
        (bytes == nullptr && length != 0U)) {
        return false;
    }
    const FlashLease lease(self->m_worker);
    if (!lease.IsAcquired()) return false;
    File file = LittleFS.open(temporary, "w");
    if (!file) return false;
    const bool written = WriteComplete(file, bytes, length);
    const size_t writtenSize = file.size();
    file.close();
    if (!written || writtenSize != length) {
        return false;
    }
    // LittleFS rename is the durability boundary: it atomically leaves either
    // the prior complete destination or this flushed temporary artifact.
    return LittleFS.rename(temporary, path);
}

bool GameHistoryJournalStorage::Append(
    void* const context,
    const JournalArtifact artifact,
    const uint8_t* const bytes,
    const size_t length) {
    auto* const self = static_cast<GameHistoryJournalStorage*>(context);
    const char* const path = ArtifactPath(artifact);
    if (self == nullptr || path == nullptr || bytes == nullptr ||
        length == 0U) {
        return false;
    }
    const FlashLease lease(self->m_worker);
    if (!lease.IsAcquired()) return false;
    File file = LittleFS.open(path, "a");
    if (!file) return false;
    const size_t before = file.size();
    const bool written = WriteComplete(file, bytes, length);
    const size_t after = file.size();
    file.close();
    return written && before <= UINT32_MAX &&
           length <= UINT32_MAX - before && after == before + length;
}

bool GameHistoryJournalStorage::Promote(
    void* const context,
    const JournalArtifact from,
    const JournalArtifact to) {
    auto* const self = static_cast<GameHistoryJournalStorage*>(context);
    const char* const fromPath = ArtifactPath(from);
    const char* const toPath = ArtifactPath(to);
    if (self == nullptr || fromPath == nullptr || toPath == nullptr ||
        from == to) {
        return false;
    }
    const FlashLease lease(self->m_worker);
    if (!lease.IsAcquired()) return false;
    return LittleFS.exists(fromPath) && LittleFS.rename(fromPath, toPath);
}

} // namespace firmware

#endif
