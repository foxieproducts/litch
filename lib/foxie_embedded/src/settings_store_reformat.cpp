#include "foxie_embedded/settings_store.hpp"

#include "settings_store_internal.hpp"

#ifdef ARDUINO
#include <cerrno>
#include <LittleFS.h>
#include <esp_littlefs.h>
#include "foxie_embedded/logger.hpp"
#endif

namespace fxe {
namespace {
#ifdef ARDUINO
class SettingsIoAccessScope final {
  public:
    SettingsIoAccessScope(SettingsStore::StorageAccessCallback& before,
                          SettingsStore::StorageAccessCallback& after)
        : m_after(after) {
        if (before) before();
    }
    ~SettingsIoAccessScope() { if (m_after) m_after(); }
  private:
    SettingsStore::StorageAccessCallback& m_after;
};

struct SettingsRecoveryHashWriter final : Print {
    static constexpr uint32_t OFFSET_BASIS = 2166136261U;
    uint32_t hash = OFFSET_BASIS;
    static uint32_t Update(uint32_t hash, const uint8_t* bytes, size_t size) {
        for (size_t index = 0U; index < size; ++index) {
            hash = (hash ^ bytes[index]) * 16777619U;
        }
        return hash;
    }
    size_t write(uint8_t byte) override { return write(&byte, 1U); }
    size_t write(const uint8_t* bytes, size_t size) override {
        hash = Update(hash, bytes, size);
        return size;
    }
};
#endif

} // namespace

#ifdef ARDUINO
bool ReformatLittleFsPartition() {
    // Always erase the exact board filesystem, including a healthy mount.
    LittleFS.end();
    disableCore0WDT();
    const esp_err_t error = esp_littlefs_format(SETTINGS_FS_PARTITION_LABEL);
    enableCore0WDT();
    if (error != ESP_OK) {
        fxe::Error("settings: LittleFS format failed: %d", static_cast<int>(error));
        return false;
    }
    return LittleFS.begin(false, SETTINGS_FS_BASE_PATH,
                          SETTINGS_FS_MAX_OPEN_FILES, SETTINGS_FS_PARTITION_LABEL);
}
#endif

bool SettingsStore::ReformatFilesystemAndSaveNowBlocking() {
#ifdef ARDUINO
    if (m_doc.overflowed() || !m_doc.is<JsonObject>() ||
        measureJson(m_doc) >= SETTINGS_STORE_MAX_FILE_SIZE_BYTES) {
        SetError(SettingsStoreError::SerializeFailed,
                 "recovery snapshot is invalid or too large");
        return false;
    }
    NotifyBeforePersistence();
    if (!WaitForDirectStorageAccess()) return false;
    bool formatted = false;
    {
        SettingsIoAccessScope access(m_beforeStorageAccessCb, m_afterStorageAccessCb);
        m_mounted = false;
        formatted = ReformatLittleFsPartition();
    }
    if (!formatted) {
        m_disabled.store(true, std::memory_order_release);
        SetError(SettingsStoreError::FormatFailed,
                 "explicit filesystem reformat/remount failed");
        return false;
    }
    m_mounted = true;
    // This is the worker's private writer, never the live RAM source. Force
    // verification regardless of the ordinary save configuration.
    m_disabled.store(false, std::memory_order_release);
    const bool priorVerifyWrites = m_cfg.verifyWrites;
    m_cfg.verifyWrites = true;
    const bool saved = SaveNowBlocking();
    m_cfg.verifyWrites = priorVerifyWrites;
    if (!saved) return false;

    SettingsRecoveryHashWriter expected;
    const size_t expectedBytes = serializeJson(m_doc, expected);
    if (expectedBytes == 0U) {
        SetError(SettingsStoreError::SerializeFailed, "recovery snapshot serialization unavailable");
        return false;
    }
    File current;
    auto evidence = StorageFailureEvidence::Unconfirmed;
    {
        SettingsIoAccessScope access(m_beforeStorageAccessCb, m_afterStorageAccessCb);
        errno = 0;
        current = LittleFS.open(m_path, "r");
        if (!current && errno == EIO) evidence = StorageFailureEvidence::Confirmed;
    }
    bool readable = current && expectedBytes != 0U;
    uint32_t actualHash = SettingsRecoveryHashWriter::OFFSET_BASIS;
    size_t actualBytes = 0U;
    uint8_t buffer[SETTINGS_SAVE_IO_CHUNK_BYTES] = {};
    // One byte beyond the expected document detects excess content without an
    // unbounded read. An open/path refusal is not a readback comparison.
    while (readable && actualBytes <= expectedBytes) {
        const size_t remaining = expectedBytes + 1U - actualBytes;
        const size_t requested = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t received = 0U;
        int readError = 0;
        {
            SettingsIoAccessScope access(m_beforeStorageAccessCb, m_afterStorageAccessCb);
            errno = 0;
            received = current.read(buffer, requested);
            readError = errno;
        }
        if (readError != 0 || received > requested) {
            if (readError == EIO) evidence = StorageFailureEvidence::Confirmed;
            readable = false;
            break;
        }
        if (received == 0U) break;
        actualHash = SettingsRecoveryHashWriter::Update(actualHash, buffer, received);
        actualBytes += received;
    }
    {
        SettingsIoAccessScope access(m_beforeStorageAccessCb, m_afterStorageAccessCb);
        current.close();
    }
    if (!readable || actualBytes != expectedBytes || actualHash != expected.hash) {
        if (readable) evidence = StorageFailureEvidence::Confirmed;
        SetError(SettingsStoreError::VerifyFailed,
                 "formatted filesystem primary readback unavailable or mismatched", evidence);
        return false;
    }
    return true;
#else
    SetError(SettingsStoreError::Unsupported,
             "full filesystem reformat requires a board storage owner");
    return false;
#endif
}

} // namespace fxe
