#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <ArduinoJson.h>
#include "foxie_embedded/fixed_function.hpp"

namespace fxe {

// Shared bounded save/write and verification-read quantum. Each LittleFS
// quantum is independently bracketed by the storage/render access callbacks.
inline constexpr size_t SETTINGS_SAVE_IO_CHUNK_BYTES = 512U;

enum class JsonLoadResult : uint8_t {
    Missing = 0,
    Empty,
    Ok,
    Error,
    OpenError,
    ReadError,
    NoMemory,
    TooLarge,
};

struct SettingsReadObservation {
    size_t bytes = 0U;
    uint32_t fnv32 = 2166136261U;
    // Optional diagnostic-only handoff after a complete bounded source read.
    fxe::FixedFunction<void(), 32>* chunkHandoff = nullptr;
};

struct SettingsReadAccess {
    fxe::FixedFunction<void(), 32>* before = nullptr;
    fxe::FixedFunction<void(), 32>* after = nullptr;
    std::atomic<bool>* confirmedIoFailure = nullptr;
    SettingsReadObservation* observation = nullptr;
};

enum class SettingsPathState : uint8_t { Missing, Present, Unknown };
SettingsPathState ProbeSettingsPath(const char* path);

struct SettingsScrubStats {
    uint16_t removedObjectKeys = 0;
    uint16_t nulledArrayElements = 0;
};

struct SettingsStoreConfig;
class SettingsRemovedKeys;

enum class PartitionErasure : uint8_t {
    Erased,
    ContainsData,
    ReadError,
};

using PartitionRead = bool (*)(const void* context, size_t offset,
                               uint8_t* out, size_t size);

// Authorize automatic filesystem initialization only after every byte in the
// partition has been read successfully and observed erased. A prefix probe is
// insufficient because a damaged filesystem may retain recoverable data later
// in the partition.
PartitionErasure ClassifyPartitionErasure(
    size_t partitionSize, PartitionRead read, const void* context,
    uint8_t* scratch, size_t scratchSize);

bool MakeDerivedPath(char* out, size_t outCapacity, const char* basePath, const char* suffix);

JsonLoadResult LoadJsonFromPath(const char* path, JsonDocument& outDoc,
                              SettingsReadAccess access = {});

bool DeepCopyJsonDoc(const JsonDocument& src, JsonDocument& dst);

bool CopyUppercaseSettingKey(const char* key, char* out, size_t outCapacity);
const char* FindTopLevelObjectKeyCaseInsensitive(JsonObjectConst object, const char* key);
const char* FindTopLevelObjectKeyCaseInsensitive(const JsonDocument& doc, const char* key);
bool NormalizeTopLevelObjectKeysUppercase(JsonDocument& doc);
bool ProcessSettingsDocument(JsonDocument& document,
                             const SettingsStoreConfig& config,
                             const char* path, SettingsRemovedKeys* removed = nullptr);

bool MergeMissingSettingsFromPath(const char* path,
                                  JsonObject primary, JsonObjectConst eligibleKeys,
                                  const SettingsStoreConfig& config,
                                  JsonDocument& recovery,
                                  JsonLoadResult& outFailure,
                                  SettingsReadAccess access = {});

bool CopyJsonValueOwned(JsonVariant destination, JsonVariantConst source, uint8_t depth);
bool CopyJsonDocumentOwned(JsonDocument& destination,
                           const JsonDocument& source);

bool ScrubBadPrintableStrings(JsonDocument& doc, SettingsScrubStats* outStats = nullptr,
                             SettingsRemovedKeys* removed = nullptr);

#ifdef ARDUINO
inline constexpr const char* SETTINGS_FS_PARTITION_LABEL = "spiffs";
inline constexpr const char* SETTINGS_FS_BASE_PATH = "/littlefs";
inline constexpr uint8_t SETTINGS_FS_MAX_OPEN_FILES = 10U;
// Raw descriptors keep each save chunk below the newlib stdio buffering layer.
int OpenSettingsWriteFile(const char* path);
size_t WriteSettingsFile(int fd, const uint8_t* data, size_t size);
bool SyncSettingsFile(int fd);
bool CloseSettingsFile(int fd);
// Metadata must not open file data through Arduino's FS wrapper.
bool RemoveSettingsFile(const char* path);
bool RenameSettingsFile(const char* from, const char* to);
bool TryMountLittleFsNoAutoFormat();
// Erases the exact board filesystem even when it already mounts successfully.
bool ReformatLittleFsPartition();
bool TryMountLittleFsFormatOnFail();
bool TryMountLittleFsErasedPartitionSafe(bool forceFormat,
    bool allowErasedInitialization = true, bool* erasedStorage = nullptr);
#endif

} // namespace fxe
