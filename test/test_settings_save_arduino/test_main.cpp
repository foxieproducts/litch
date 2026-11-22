#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "foxie_embedded/logger.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "firmware/settings_physical_access.hpp"
#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "modules/litchcraft_wire_codec.hpp"
#include "modules/litchcraft_frame_stream.hpp"
#include "freertos/task.h"
#include <esp_partition.h>

namespace physical_sdk {
esp_partition_t settingsPartition{}, otherPartition{};
int taskToken = 0;
void* task = &taskToken;
const esp_partition_t* partition = nullptr;
size_t offset = 0U, bytes = 0U;
const void* buffer = nullptr;
unsigned calls = 0U, clockReads = 0U, deniedRenderAttempts = 0U;
uint32_t delayMs = 0U;
uint32_t delayUs = 0U;
uint64_t elapsedUs = 0U;
unsigned microClockReads = 0U;
uint32_t requestedDelayTicks = 0U;
int result = 0, resultError = 0, inputError = 0;
unsigned syncMode = 0U;
hardware::RmtFlashArbiter* activeArbiter = nullptr;
void (*sdkYield)() = nullptr;
void (*flashWait)() = nullptr;
void WaitForFlash(const uint32_t ticks) {
    requestedDelayTicks = ticks;
    if (flashWait != nullptr) flashWait();
    else std::this_thread::yield();
}
TickType_t CurrentTick() { return fxe::ManualMillis::NowMs(); }
TaskHandle_t CurrentTask() { errno = EBUSY; return task; }
int Access(const esp_partition_t* p, size_t at, const void* data, size_t size) {
    if (activeArbiter != nullptr && activeArbiter->TryBeginRender()) {
        std::fprintf(stderr, "FAIL physical SDK access overlapped RMT\n");
        std::exit(1);
    }
    if (activeArbiter != nullptr) ++deniedRenderAttempts;
    inputError = errno;
    partition = p; offset = at; buffer = data; bytes = size; ++calls;
    fxe::ManualMillis::AdvanceMs(delayMs);
    elapsedUs += static_cast<uint64_t>(delayMs) * 1000U + delayUs;
    if (sdkYield != nullptr) sdkYield();
    errno = resultError;
    return result;
}
} // namespace physical_sdk

extern "C" int64_t esp_timer_get_time() {
    ++physical_sdk::microClockReads;
    errno = EAGAIN;
    return static_cast<int64_t>(physical_sdk::elapsedUs);
}

extern "C" esp_err_t __real_esp_partition_read(
    const esp_partition_t* p, size_t at, void* data, size_t size) {
    return physical_sdk::Access(p, at, data, size);
}
extern "C" esp_err_t __real_esp_partition_write(
    const esp_partition_t* p, size_t at, const void* data, size_t size) {
    return physical_sdk::Access(p, at, data, size);
}
extern "C" esp_err_t __real_esp_partition_erase_range(
    const esp_partition_t* p, size_t at, size_t size) {
    return physical_sdk::Access(p, at, nullptr, size);
}

#define ARDUINO 1
#define LITCH_ESP32S3 1
#define xTaskGetCurrentTaskHandle physical_sdk::CurrentTask
#define vTaskDelay physical_sdk::WaitForFlash
#define xTaskGetTickCount physical_sdk::CurrentTick
#include "../../src/settings_physical_access.cpp"
#undef xTaskGetCurrentTaskHandle
#undef vTaskDelay
#undef xTaskGetTickCount
#undef LITCH_ESP32S3
#undef ARDUINO

// Compile the production Arduino save branch alongside the native aggregate.
// Only the SDK filesystem leaves and mount are controlled; the serializer,
// verification, promotion, dirty/error handling and arbiter are production code.
#define SettingsStore ArduinoSaveSettingsStore
#define CachedSetting ArduinoSaveCachedSetting
#include "../../lib/foxie_embedded/src/settings_store.cpp"
#define File SettingsSaveFile
#define Print SettingsSavePrint
#define LittleFS SettingsSaveLittleFS
#define BatchLittleFS SettingsSaveFilesystem
#define batch_sdk settings_save_sdk
#include "../test_current_game_log_render_lease/sdk/LittleFS.h"

// Pinned Arduino VFSImpl::exists opens/closes a VFSFileImpl; rename calls
// exists, and remove opens/checks/closes before unlink. Model those SDK leaves
// only for the metadata regression; ordinary fixture behavior stays unchanged.
struct SettingsSaveMetadataFilesystem : SettingsSaveFilesystem {
    bool modelMetadataOpens = false;
    bool exists(const char* path) {
        if (!modelMetadataOpens) return SettingsSaveFilesystem::exists(path);
        settings_save_sdk::Touch("exists");
        auto file = open(path, "r");
        const bool present = static_cast<bool>(file);
        if (file) file.close();
        return present;
    }
    bool rename(const char* from, const char* to) {
        if (modelMetadataOpens && !exists(from)) return false;
        return SettingsSaveFilesystem::rename(from, to);
    }
    bool remove(const char* path) {
        if (modelMetadataOpens) {
            auto file = open(path, "r");
            const bool present = static_cast<bool>(file);
            if (file) file.close();
            if (!present || std::filesystem::is_directory(settings_save_sdk::Path(path))) return false;
        }
        return SettingsSaveFilesystem::remove(path);
    }
};
SettingsSaveMetadataFilesystem settingsSaveMetadataFilesystem;
#undef LittleFS
#define LittleFS settingsSaveMetadataFilesystem

namespace {
hardware::RmtFlashArbiter* saveArbiter = nullptr;
bool failSync = false;
bool failClose = false;
bool shortWrite = false;
bool refuseVerificationOpen = false;
size_t maxWrite = 0U;
unsigned rawWrites = 0U;
const char* delayedOperation = "";
uint32_t operationDelayMs = 0U;

void Need(const bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL settings Arduino save: %s\n", message);
        std::exit(1);
    }
}

void ObserveAccess(const char* operation) {
    Need(saveArbiter == nullptr || !saveArbiter->TryBeginRender(), "controlled exclusive writer scope");
    if (refuseVerificationOpen && std::strcmp(operation, "open") == 0) {
        const auto temporary = settings_save_sdk::root / "settings.json.tmp";
        const auto held = settings_save_sdk::root / "settings.json.held";
        Need(std::rename(temporary.c_str(), held.c_str()) == 0,
             "controlled verification-open refusal preserves staging bytes");
    }
    fxe::ManualMillis::AdvanceMs(1U);
    if (std::strcmp(operation, delayedOperation) == 0) {
        fxe::ManualMillis::AdvanceMs(operationDelayMs);
    }
}

void ProgramBytes(const size_t size) {
    // Controlled per-512-byte cost, NOT measured ESP32 timing. The same leaf
    // cost applies to raw writes and the adapter's configured stdio buffer.
    Need(saveArbiter == nullptr || !saveArbiter->TryBeginRender(), "controlled exclusive data scope");
    fxe::ManualMillis::AdvanceMs(static_cast<unsigned>((size + 511U) / 512U) * 8U);
}
} // namespace

namespace fxe {
int OpenSettingsWriteFile(const char* path) {
    settings_save_sdk::Touch("raw_open");
    return ::open(settings_save_sdk::Path(path).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
}
size_t WriteSettingsFile(const int fd, const uint8_t* data, const size_t size) {
    settings_save_sdk::Touch("raw_write");
    maxWrite = std::max(maxWrite, size);
    ++rawWrites;
    ProgramBytes(size);
    const size_t requested = shortWrite && size > 0U ? size - 1U : size;
    const ssize_t written = ::write(fd, data, requested);
    return written > 0 ? static_cast<size_t>(written) : 0U;
}
bool SyncSettingsFile(const int fd) {
    settings_save_sdk::Touch("raw_sync");
    if (physical_sdk::syncMode == 1U) {
        for (unsigned i = 0U; i < 8U; ++i) {
            physical_sdk::delayMs = 26U;
            (void)__wrap_esp_partition_write(&physical_sdk::settingsPartition, i * 128U, nullptr, 128U);
        }
        fxe::ManualMillis::AdvanceMs(20U);
    } else if (physical_sdk::syncMode == 2U) {
        physical_sdk::delayMs = 228U;
        (void)__wrap_esp_partition_erase_range(&physical_sdk::settingsPartition, 4096U, 4096U);
    } else if (physical_sdk::syncMode == 3U) {
        fxe::ManualMillis::AdvanceMs(228U);
    } else if (physical_sdk::syncMode == 4U) {
        // Actual V3.3 metadata witness: 573 reads / 93440 B / 52 ms,
        // one 128 B write / 2 ms, no erase, 65 ms whole operation. Keep the
        // measured call sizes/counts and bounded costs; timing is controlled.
        for (unsigned i = 0U; i < 573U; ++i) {
            physical_sdk::delayMs = i % 11U == 10U ? 1U : 0U;
            (void)__wrap_esp_partition_read(&physical_sdk::settingsPartition,
                                          0U, nullptr, i < 157U ? 256U : 128U);
            if (i % 53U == 52U) fxe::ManualMillis::AdvanceMs(1U);
        }
        physical_sdk::delayMs = 2U;
        (void)__wrap_esp_partition_write(&physical_sdk::settingsPartition,
                                       0U, nullptr, 128U);
    }
    const bool synced = ::fsync(fd) == 0;
    if (failSync) errno = EIO;
    return synced && !failSync;
}
bool CloseSettingsFile(const int fd) {
    settings_save_sdk::Touch("raw_close");
    return ::close(fd) == 0 && !failClose;
}
bool RemoveSettingsFile(const char* path) {
    settings_save_sdk::Touch("raw_remove");
    return ::unlink(settings_save_sdk::Path(path).c_str()) == 0;
}
bool RenameSettingsFile(const char* from, const char* to) {
    settings_save_sdk::Touch("raw_rename");
    return std::rename(settings_save_sdk::Path(from).c_str(), settings_save_sdk::Path(to).c_str()) == 0;
}
bool SettingsSaveMount(bool) {
    settings_save_sdk::Touch("mount");
    return true;
}
} // namespace fxe

#define TryMountLittleFsErasedPartitionSafe SettingsSaveMount
#define ARDUINO 1
#ifndef SETTINGS_SAVE_IMPLEMENTATION
#define SETTINGS_SAVE_IMPLEMENTATION "../../lib/foxie_embedded/src/settings_store_save.cpp"
#endif
#include SETTINGS_SAVE_IMPLEMENTATION
#undef LITCH_NATIVE
// Control only the POSIX VFS leaves used by the production Arduino adapter.
// Namespace lookup keeps the SDK File::open/close implementation unchanged.
namespace firmware {
enum class BankPrepareFault { None, Open, Sync, Close, ImplicitSync, SizeRead };
BankPrepareFault bankPrepareFault = BankPrepareFault::None;
unsigned bankPrepareOpens = 0U;
int bankPrepareFd = -1, rootWriterFd = -1;
std::string bankPreparePath;
unsigned bankPrepareSizeReads = 0U;
BankPrepareFault rootWriterFault = BankPrepareFault::None;
enum class FrameReadFault { None, Open, SeekEnd, Size, SeekRead, Read };
FrameReadFault frameReadFault = FrameReadFault::None;
int frameReadError = EIO;
unsigned frameReadHandles = 0U;
bool FailFrameRead(const FrameReadFault operation) {
    if (frameReadFault != operation) return false;
    frameReadFault = FrameReadFault::None;
    errno = frameReadError;
    return true;
}
int open(const char* path, const int flags, const int mode) {
    const bool prepare = (flags & O_TRUNC) != 0;
    settings_save_sdk::Touch(prepare ? "bank_prepare_open" : "frame_writer_open");
    if (prepare) {
        ++bankPrepareOpens;
        if (bankPrepareFault == BankPrepareFault::Open) { errno = EACCES; return -1; }
    }
    const bool implicitFailure = prepare && bankPrepareFault == BankPrepareFault::ImplicitSync;
    const int fd = ::open(settings_save_sdk::Path(path).c_str(),
                          implicitFailure ? flags & ~O_TRUNC : flags, mode);
    if (prepare) {
        bankPrepareFd = fd;
        bankPreparePath = settings_save_sdk::Path(path);
    }
    else if (std::strstr(path, ".r0") != nullptr || std::strstr(path, ".r1") != nullptr) {
        rootWriterFd = fd;
    }
    // Installed esp_littlefs can return fd after the implicit truncation sync
    // failed. Its ERRED handle then reports successful explicit sync/close.
    if (implicitFailure && fd >= 0) errno = EIO;
    return fd;
}
int fsync(const int fd) {
    settings_save_sdk::Touch(fd == bankPrepareFd ? "bank_prepare_sync" : "frame_sync");
    const int result = ::fsync(fd);
    if ((fd == bankPrepareFd && bankPrepareFault == BankPrepareFault::Sync) ||
        (fd == rootWriterFd && rootWriterFault == BankPrepareFault::Sync)) {
        errno = EIO; return -1;
    }
    return result;
}
int close(const int fd) {
    settings_save_sdk::Touch(fd == bankPrepareFd ? "bank_prepare_close" : "frame_writer_close");
    const bool fail = (fd == bankPrepareFd && bankPrepareFault == BankPrepareFault::Close) ||
                     (fd == rootWriterFd && rootWriterFault == BankPrepareFault::Close);
    const int result = ::close(fd);
    if (fd == bankPrepareFd) bankPrepareFd = -1;
    if (fd == rootWriterFd) rootWriterFd = -1;
    if (fail) { errno = EIO; return -1; }
    return result;
}
int fstat(const int fd, struct stat* out) {
    settings_save_sdk::Touch("frame_writer_size");
    if (fd == bankPrepareFd) {
        ++bankPrepareSizeReads;
        if (bankPrepareFault == BankPrepareFault::SizeRead) { errno = EIO; return -1; }
        // Pinned VFS fstat uses lfs_stat(path), not lfs_file_size(handle).
        return ::stat(bankPreparePath.c_str(), out);
    }
    return ::fstat(fd, out);
}
int unlink(const char* path) {
    settings_save_sdk::Touch("frame_remove");
    return ::unlink(settings_save_sdk::Path(path).c_str());
}
off_t lseek(const int fd, const off_t offset, const int whence) {
    settings_save_sdk::Touch("frame_writer_seek");
    return ::lseek(fd, offset, whence);
}
ssize_t write(const int fd, const void* bytes, const size_t size) {
    settings_save_sdk::Touch("frame_writer_write");
    return ::write(fd, bytes, size);
}
// Same POSIX/stdio leaves as production, scoped to the included Arduino file
// owner. Path mapping, access observation and faults remain SDK boundaries.
std::FILE* fopen(const char* path, const char* mode) {
    settings_save_sdk::Touch("frame_read_open");
    Need(std::strncmp(path, "/littlefs/", 10U) == 0, "Arduino stdio reader uses the mounted VFS path");
    if (FailFrameRead(FrameReadFault::Open)) return nullptr;
    auto* file = std::fopen(settings_save_sdk::Path(path).c_str(), mode);
    if (file != nullptr) ++frameReadHandles;
    return file;
}
int fseek(std::FILE* file, const long offset, const int whence) {
    settings_save_sdk::Touch("frame_read_seek");
    if (FailFrameRead(whence == SEEK_END ? FrameReadFault::SeekEnd : FrameReadFault::SeekRead)) return -1;
    return std::fseek(file, offset, whence);
}
long ftell(std::FILE* file) {
    settings_save_sdk::Touch("frame_read_size");
    if (FailFrameRead(FrameReadFault::Size)) return -1L;
    return std::ftell(file);
}
size_t fread(void* out, const size_t size, const size_t count, std::FILE* file) {
    settings_save_sdk::Touch("frame_read");
    if (FailFrameRead(FrameReadFault::Read)) return 0U;
    const auto received = std::fread(out, size, count, file);
    settings_save_sdk::readBytes += received * size;
    return received;
}
int fclose(std::FILE* file) {
    settings_save_sdk::Touch("frame_read_close");
    Need(frameReadHandles != 0U, "retained and transient reader ownership stays balanced");
    --frameReadHandles;
    return std::fclose(file);
}
} // namespace firmware
#define LitchcraftFrameFile ArduinoLeaseFrameFile
#include "../../src/litchcraft_frame_file.cpp"
#include "../../src/litchcraft_frame_file_layout.cpp"
#undef LitchcraftFrameFile
#define LITCH_NATIVE 1
#undef ARDUINO
#undef TryMountLittleFsErasedPartitionSafe
#undef batch_sdk
#undef BatchLittleFS
#undef LittleFS
#undef Print
#undef File
#undef SettingsStore
#undef CachedSetting

namespace {
std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

enum class MetadataCase { OpenCost, PromotionRecovery, PromotionWithoutVerification };

void RunMetadataOpenCase(const MetadataCase scenario) {
    char directory[] = "/tmp/litch-save-metadata-opens-XXXXXX";
    Need(::mkdtemp(directory) != nullptr, "create metadata-open regression fixture");
    settings_save_sdk::root = directory;
    settings_save_sdk::access = ObserveAccess;
    settingsSaveMetadataFilesystem.modelMetadataOpens = true;
    const std::string path = (settings_save_sdk::root / "settings.json").string();
    const std::string primary = "{\"BRIGHTNESS\":2}";
    std::ofstream(path) << primary;
    std::ofstream(path + ".bak") << "{\"BRIGHTNESS\":1}";
    fxe::SettingsStoreConfig config; config.path = path.c_str();
    config.verifyWrites = scenario != MetadataCase::PromotionWithoutVerification;
    fxe::ArduinoSaveSettingsStore store(config);
    hardware::RmtFlashArbiter arbiter;
    saveArbiter = &arbiter;
    struct MetadataWitness {
        unsigned opens = 0U, operations = 0U, verificationOpens = 0U;
        unsigned observedOpens = settings_save_sdk::counts["open"];
        fxe::SettingsSaveOperation failedRename{};
    } witness;
    store.SetStorageAccessCallbacks([&] {
        Need(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()), "metadata save acquires flash");
    }, [&] {
        arbiter.EndFlash();
        Need(arbiter.TryBeginRender(), "metadata save yields after each operation");
        arbiter.EndRender();
        errno = EBUSY; // Operation diagnostics must retain the filesystem error.
    });
    store.SetSaveOperationCallback([&](const fxe::SettingsSaveOperation& op) {
        const unsigned opened = settings_save_sdk::counts["open"] - witness.observedOpens;
        witness.observedOpens = settings_save_sdk::counts["open"];
        using Kind = fxe::SettingsSaveOperationKind;
        if (op.kind == Kind::Exists || op.kind == Kind::Remove || op.kind == Kind::Rename) {
            ++witness.operations;
            witness.opens += opened;
        }
        if (op.kind == Kind::OpenRead) witness.verificationOpens += opened;
        if (op.kind == Kind::Rename && !op.succeeded) witness.failedRename = op;
    });
    store.Doc()["BRIGHTNESS"] = 7U;
    Need(store.SaveNowBlocking(), "metadata optimization retains verified promotion");
    Need(ReadBytes(path + ".bak") == primary, "metadata optimization retains the previous primary");
    const auto promoted = ReadBytes(path);
    const auto savedTotal = store.GetPersistedSaveTotal();
    if (scenario == MetadataCase::OpenCost) {
        std::printf("SAVE_METADATA_OPENS metadata=%u data_opens=%u verification_opens=%u\n",
                    witness.operations, witness.opens, witness.verificationOpens);
        Need(witness.operations >= 5U && witness.verificationOpens == 1U,
             "metadata operations and mandatory verification open were exercised");
        Need(witness.opens == 0U, "metadata-only operations must not open file data");
        settingsSaveMetadataFilesystem.modelMetadataOpens = false;
        settings_save_sdk::access = nullptr;
        std::filesystem::remove_all(directory);
        return;
    }

    // A real nonempty backup directory prevents promotion. No controlled
    // syscall result may hide loss of the primary or completed staging bytes.
    std::filesystem::remove(path + ".bak");
    std::filesystem::create_directory(path + ".bak");
    std::ofstream(path + ".bak/keep") << "retained directory";
    store.Doc()["BRIGHTNESS"] = 9U;
    store.MarkDirty();
    Need(!store.SaveNowBlocking(), "real rename refusal rejects promotion");
    std::printf("SAVE_PROMOTION_REFUSAL verify=%u primary=%u backup=%u staging=%u total_before=%u total_after=%u\n",
        unsigned(config.verifyWrites),
        unsigned(ReadBytes(path) == promoted),
        unsigned(ReadBytes(path + ".bak/keep") == "retained directory"),
        unsigned(std::filesystem::exists(path + ".tmp")),
        savedTotal, store.GetPersistedSaveTotal());
    Need(ReadBytes(path) == promoted &&
             ReadBytes(path + ".bak/keep") == "retained directory" &&
             std::filesystem::exists(path + ".tmp"),
         "failed metadata promotion preserves primary, backup directory and completed staging");
    Need(store.GetPersistedSaveTotal() == savedTotal,
         "rejected promotion cannot advance the persisted save count");
    Need(witness.failedRename.kind == fxe::SettingsSaveOperationKind::Rename &&
             witness.failedRename.operationError != 0 && witness.failedRename.operationError != EBUSY &&
             !store.HasConfirmedStorageFailure(),
         "rename refusal retains its actual errno without manufacturing flash failure");
    std::filesystem::remove_all(path + ".bak");
    Need(store.SaveNowBlocking(), "retained staging recovers through the ordinary save path");
    DynamicJsonDocument saved(1024U);
    Need(!deserializeJson(saved, ReadBytes(path)) && saved["BRIGHTNESS"].as<unsigned>() == 9U &&
             ReadBytes(path + ".bak") == promoted && !std::filesystem::exists(path + ".tmp"),
         "retry promotes latest bytes, preserves backup and retires the old staging file");
    Need(witness.operations >= 5U && witness.verificationOpens == (config.verifyWrites ? 3U : 0U) &&
             store.GetPersistedSaveTotal() == savedTotal + 1U,
         "real metadata operations and all mandatory verification opens were exercised");
    settingsSaveMetadataFilesystem.modelMetadataOpens = false;
    settings_save_sdk::access = nullptr;
    std::filesystem::remove_all(directory);
}

void RunCase(const bool verifyWrites) {
    char directory[] = "/tmp/litch-settings-arduino-XXXXXX";
    Need(::mkdtemp(directory) != nullptr, "create isolated temporary directory");
    settings_save_sdk::root = directory;
    settings_save_sdk::stdioBufferCapacity = 4096U;
    settings_save_sdk::programBytes = ProgramBytes;
    const auto path = settings_save_sdk::root / "settings.json";
    const std::string pathString = path.string();
    fxe::SettingsStoreConfig config;
    config.path = pathString.c_str();
    config.verifyWrites = verifyWrites;
    fxe::ArduinoSaveSettingsStore store(config);
    hardware::RmtFlashArbiter arbiter;
    saveArbiter = &arbiter;
    settings_save_sdk::access = ObserveAccess;
    store.SetStorageAccessCallbacks(
        [&] { Need(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()), "acquire flash"); },
        [&] {
            arbiter.EndFlash();
            Need(arbiter.TryBeginRender(), "render handoff after every operation");
            arbiter.EndRender();
        });
    store.Doc()["BRIGHTNESS"] = 3U;
    // Exercise several chunks and the final partial chunk without reducing the
    // document's content or precision to make the latency assertion pass.
    store.Doc()["PAYLOAD"] = std::string(4100U, 'x');
    Need(store.SaveNowBlocking(), "initial durable save");
    std::printf("Arduino save verify=%d controlled max lease=%u ms\n", verifyWrites,
                arbiter.GetFlashDiagnostics().maxHoldMs);
    Need(arbiter.GetFlashDiagnostics().maxHoldMs <= 9U,
         "a stdio flush must not batch multiple data chunks under one lease");
    Need(rawWrites > 1U && maxWrite == 512U, "bounded direct writes were exercised");
    DynamicJsonDocument saved(9216U);
    const std::string first = ReadBytes(path);
    Need(!deserializeJson(saved, first), "saved JSON parses");
    Need(saved["PAYLOAD"].as<std::string>() == std::string(4100U, 'x'), "complete payload persists");
    Need(saved["BRIGHTNESS"].as<unsigned>() == 3U, "exact preference persists");
    const uint32_t firstSaveTotal = store.GetPersistedSaveTotal();

    for (unsigned failure = 0U; failure < 3U; ++failure) {
        store.Doc()["BRIGHTNESS"] = 7U;
        store.MarkDirty();
        failSync = failure == 0U;
        failClose = failure == 1U;
        shortWrite = failure == 2U;
        Need(!store.SaveNowBlocking(), "write/sync/close failure must reject save");
        Need(store.GetLastError() == fxe::SettingsStoreError::WriteFailed, "failed durability stays visible");
        Need(store.HasConfirmedStorageFailure(), "actual short-write/sync/close failures certify operation evidence");
        Need(ReadBytes(path) == first, "failed staging cannot replace prior primary");
        Need(store.GetPersistedSaveTotal() == firstSaveTotal,
             "failed durability cannot advance the persisted save count");
        failSync = failClose = shortWrite = false;
    }

    // A previous pending file may be the only recovery authority. A failed
    // retry must preserve it; a later successful save still promotes exactly.
    const auto pending = std::filesystem::path(pathString + ".tmp");
    std::ofstream(pending) << "{\"BRIGHTNESS\":2}";
    failSync = true;
    Need(!store.SaveNowBlocking(), "reject failed retry scratch");
    Need(ReadBytes(pending) == "{\"BRIGHTNESS\":2}", "retained recovery tmp survives failure");
    failSync = false;
    Need(store.SaveNowBlocking(), "retry completes");
    Need(!store.HasConfirmedStorageFailure(), "verified retry clears confirmed operation evidence");
    saved.clear();
    Need(!deserializeJson(saved, ReadBytes(path)), "retry output parses");
    Need(saved["BRIGHTNESS"].as<unsigned>() == 7U, "latest exact value after retry");
    Need(store.GetPersistedSaveTotal() == firstSaveTotal + 1U,
         "only successful retry advances persisted save count");
    Need(ReadBytes(pathString + ".bak") == first, "backup retains previous verified primary");
    Need(!std::filesystem::exists(pending), "obsolete recovery tmp removed after success");
    if (verifyWrites) {
        refuseVerificationOpen = true;
        Need(!store.SaveNowBlocking() && store.GetLastError() == fxe::SettingsStoreError::VerifyFailed,
             "verification open ENOENT remains a truthful failed save");
        Need(!store.HasConfirmedStorageFailure(),
             "verification open refusal without EIO cannot certify flash failure");
        refuseVerificationOpen = false;
        Need(store.SaveNowBlocking(), "verification-open refusal recovers through existing save");
    }
    settings_save_sdk::access = nullptr;
    settings_save_sdk::programBytes = nullptr;
    settings_save_sdk::stdioBufferCapacity = 0U;
    std::filesystem::remove_all(directory);
}
void RunOperationWitnessCase() {
    char directory[] = "/tmp/litch-save-witness-XXXXXX";
    Need(::mkdtemp(directory) != nullptr, "create operation witness fixture");
    settings_save_sdk::root = directory;
    settings_save_sdk::access = ObserveAccess;
    settings_save_sdk::programBytes = ProgramBytes;
    const std::string path = (settings_save_sdk::root / "settings.json").string();
    fxe::SettingsStoreConfig config;
    config.path = path.c_str();
    fxe::ArduinoSaveSettingsStore store(config);
    hardware::RmtFlashArbiter arbiter;
    saveArbiter = &arbiter;
    struct Witness {
        fxe::SettingsSaveOperation last{}, worst{}, sync{}, write{}, read{}, openFailure{};
        unsigned count = 0U;
    } witness;
    store.SetStorageAccessCallbacks([&] {
        fxe::ManualMillis::AdvanceMs(83U); // Waiting for a completed render.
        Need(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()), "witness acquires lease");
    }, [&] {
        arbiter.EndFlash();
        fxe::ManualMillis::AdvanceMs(41U); // Post-release paging/hand-off work.
        errno = EBUSY;
    });
    store.SetSaveOperationCallback([&](const fxe::SettingsSaveOperation& operation) {
        Need(arbiter.TryBeginRender(), "observer runs after flash lease release");
        arbiter.EndRender();
        if (witness.last.saveGeneration != operation.saveGeneration ||
            operation.durationMs > witness.worst.durationMs) witness.worst = operation;
        witness.last = operation;
        ++witness.count;
        if (operation.kind == fxe::SettingsSaveOperationKind::Write) witness.write = operation;
        if (operation.kind == fxe::SettingsSaveOperationKind::Read) witness.read = operation;
        if (operation.kind == fxe::SettingsSaveOperationKind::Sync) {
            witness.sync = operation;
            if (!operation.succeeded) Need(errno == EIO, "release cannot clobber original sync errno");
        }
        if (operation.kind == fxe::SettingsSaveOperationKind::OpenRead && !operation.succeeded) {
            witness.openFailure = operation;
            Need(errno == ENOENT, "observer sees original open errno after release");
        }
        errno = EIO; // Observer cannot manufacture storage failure evidence.
    });
    store.Doc()["BRIGHTNESS"] = 3U;
    delayedOperation = "mount"; operationDelayMs = 1999U;
    Need(store.SaveNowBlocking(), "slow startup witness still saves");
    Need(witness.worst.kind == fxe::SettingsSaveOperationKind::Mount && witness.worst.durationMs == 2000U,
         "slow startup mount is attributed without acquisition or release wait");
    const uint32_t startupGeneration = witness.last.saveGeneration;
    delayedOperation = "raw_sync"; operationDelayMs = 365U;
    Need(store.SaveNowBlocking(), "ordinary save after slow startup succeeds");
    Need(witness.last.saveGeneration != startupGeneration &&
         witness.worst.kind == fxe::SettingsSaveOperationKind::Sync && witness.worst.durationMs == 366U &&
         witness.worst.path == fxe::SettingsSavePathRole::Pending && witness.worst.succeeded,
         "next save exposes its 366 ms sync rather than a prior 2 second mount");
    Need(witness.last.kind == fxe::SettingsSaveOperationKind::Rename &&
         witness.last.path == fxe::SettingsSavePathRole::Pending &&
         witness.last.destination == fxe::SettingsSavePathRole::Primary,
         "promotion witness identifies source and destination without filenames");
    Need(witness.sync.requestedBytes == 0U && witness.write.requestedBytes == 40U &&
         witness.read.requestedBytes == 512U && witness.count > 0U,
         "witness reports actual write and verification read requests, zero bytes for metadata");
    failSync = true;
    Need(!store.SaveNowBlocking() && store.GetLastError() == fxe::SettingsStoreError::WriteFailed &&
         store.HasConfirmedStorageFailure(), "timing preserves actual sync failure result and evidence");
    Need(!witness.sync.succeeded && witness.sync.operationError == EIO && witness.sync.durationMs == 366U,
         "failed slow sync retains its original operation errno and duration");
    failSync = false; operationDelayMs = 0U;
    refuseVerificationOpen = true;
    Need(!store.SaveNowBlocking() && store.GetLastError() == fxe::SettingsStoreError::VerifyFailed &&
         witness.openFailure.operationError == ENOENT,
         "observer errno cannot overwrite verification-open failure");
    // Confirmed sync evidence is sticky until a successful save, as before.
    refuseVerificationOpen = false;
    Need(store.SaveNowBlocking() && !store.HasConfirmedStorageFailure(), "ordinary successful retry clears evidence");
    std::printf("SAVE_OPERATION_WITNESS observed=%u startup_ms=2000 ordinary_sync_ms=366 acquire_wait_ms=83 release_work_ms=41 record_bytes=%zu\n",
                witness.count, sizeof(fxe::SettingsSaveOperation));
    delayedOperation = "";
    settings_save_sdk::access = nullptr;
    settings_save_sdk::programBytes = nullptr;
    std::filesystem::remove_all(directory);
}

void RunPhysicalWitnessCase() {
    char directory[] = "/tmp/litch-save-physical-XXXXXX";
    Need(::mkdtemp(directory) != nullptr, "create physical witness fixture");
    settings_save_sdk::root = directory;
    settings_save_sdk::access = ObserveAccess;
    const std::string path = (settings_save_sdk::root / "settings.json").string();
    fxe::SettingsStoreConfig config; config.path = path.c_str();
    fxe::ArduinoSaveSettingsStore store(config);
    hardware::RmtFlashArbiter arbiter;
    saveArbiter = nullptr;
    firmware::SettingsPhysicalAccess physical;
    physical.Bind(physical_sdk::task, &physical_sdk::settingsPartition);
    physical.BindFlashArbiter(arbiter);
    firmware::RegisterSettingsPhysicalAccess(&physical);
    store.SetStorageAccessCallbacks([&] {
        physical_sdk::delayMs = 83U;
        (void)__wrap_esp_partition_read(&physical_sdk::settingsPartition, 0U, nullptr, 128U);
        physical.BeginFlashLease();
        physical.BeginOperation();
    }, [&] {
        physical.EndOperation();
        physical.EndFlashLease();
        physical.BeginFlashLease();
        physical_sdk::delayMs = 41U;
        (void)__wrap_esp_partition_read(&physical_sdk::settingsPartition, 0U, nullptr, 128U);
        physical.EndFlashLease();
    });
    fxe::SettingsSaveOperation sync;
    firmware::SettingsPhysicalAccessSample sample;
    store.SetSaveOperationCallback([&](const fxe::SettingsSaveOperation& operation) {
        if (operation.kind == fxe::SettingsSaveOperationKind::Sync) {
            sync = operation;
            sample = physical.Sample();
        }
    });
    store.Doc()["BRIGHTNESS"] = 3U;
    uint32_t priorGeneration = 0U;
    for (unsigned mode = 1U; mode <= 3U; ++mode) {
        physical_sdk::syncMode = mode;
        Need(store.SaveNowBlocking(), "controlled slow physical sync retains durable save");
        Need(sync.durationMs == 229U && sync.saveGeneration != priorGeneration && sample.available,
             "same long sync remains attributed to its actual save without grant or paging cost");
        Need(sample.access[0U].calls == 0U, "grant and post-release paging reads are excluded");
        Need(sample.access[1U].calls == (mode == 1U ? 8U : 0U) &&
             sample.access[1U].bytes == (mode == 1U ? 1024U : 0U) &&
             sample.access[1U].totalMs == (mode == 1U ? 208U : 0U) &&
             sample.access[1U].maxMs == (mode == 1U ? 26U : 0U),
             "witness distinguishes many short writes from time outside physical callbacks");
        Need(sample.access[2U].calls == (mode == 2U ? 1U : 0U) &&
             sample.access[2U].bytes == (mode == 2U ? 4096U : 0U) &&
             sample.access[2U].totalMs == (mode == 2U ? 228U : 0U) &&
             sample.access[2U].maxMs == (mode == 2U ? 228U : 0U),
             "witness distinguishes one long erase from the same whole-operation duration");
        std::printf("SAVE_PHYSICAL_WITNESS save=%u sync_ms=%u writes=%u/%u/%u/%u erases=%u/%u/%u/%u\n",
                    sync.saveGeneration, sync.durationMs,
                    sample.access[1U].calls, sample.access[1U].bytes,
                    sample.access[1U].totalMs, sample.access[1U].maxMs,
                    sample.access[2U].calls, sample.access[2U].bytes,
                    sample.access[2U].totalMs, sample.access[2U].maxMs);
        priorGeneration = sync.saveGeneration;
    }
    physical_sdk::syncMode = 0U;
    firmware::RegisterSettingsPhysicalAccess(nullptr);
    settings_save_sdk::access = nullptr;
    std::filesystem::remove_all(directory);
}

uint32_t PhysicalWitnessClock() {
    ++physical_sdk::clockReads;
    errno = EAGAIN;
    return fxe::ManualMillis::NowMs();
}

void RunPhysicalWrapperFilters() {
    firmware::SettingsPhysicalAccess physical;
    Need(!physical.Sample().available, "unbound physical diagnostics are unavailable");
    hardware::RmtFlashArbiter arbiter;
    physical.Bind(physical_sdk::task, &physical_sdk::settingsPartition);
    physical.BindFlashArbiter(arbiter);
    firmware::RegisterSettingsPhysicalAccess(&physical);
    fxe::ScopedMillisProviderOverride clock(PhysicalWitnessClock);
    const auto invoke = [&](const esp_partition_t* partition) {
        physical_sdk::clockReads = 0U; physical_sdk::delayMs = 7U;
        physical_sdk::result = 1234; physical_sdk::resultError = EIO;
        errno = ENOENT;
        const auto calls = physical_sdk::calls;
        const int result = __wrap_esp_partition_read(partition, 19U, &physical, 3U);
        Need(result == 1234 && errno == EIO && physical_sdk::inputError == ENOENT &&
             physical_sdk::calls == calls + 1U && physical_sdk::partition == partition &&
             physical_sdk::offset == 19U && physical_sdk::buffer == &physical && physical_sdk::bytes == 3U,
             "eligibility and clock probes preserve SDK arguments, result and errno in both directions");
    };
    invoke(&physical_sdk::settingsPartition);
    Need(physical_sdk::clockReads == 0U, "inactive operation does not sample clock");
    physical.BeginOperation();
    invoke(&physical_sdk::settingsPartition);
    Need(physical_sdk::clockReads == 0U && physical.Sample().access[0U].calls == 0U,
         "sampling scope alone cannot activate SDK handoffs outside a flash lease");
    physical.BeginFlashLease();
    invoke(nullptr);
    invoke(&physical_sdk::otherPartition);
    Need(physical.Sample().available && physical.Sample().access[0U].calls == 0U &&
         physical_sdk::clockReads == 0U, "exact partition identity rejects null and other partitions");
    physical_sdk::task = &physical;
    invoke(&physical_sdk::settingsPartition);
    Need(physical_sdk::clockReads == 0U && physical.Sample().access[0U].calls == 0U,
         "another task does not inspect or record worker operation state");
    physical_sdk::task = &physical_sdk::taskToken;
    invoke(&physical_sdk::settingsPartition);
    Need(physical_sdk::clockReads == 4U && physical.Sample().access[0U].calls == 1U &&
         physical.Sample().access[0U].bytes == 3U && physical.Sample().access[0U].maxMs == 7U,
         "matching failed SDK call is timed without changing its failure");
    hardware::RmtFlashArbiter unchangedArbiter;
    Need(unchangedArbiter.TryBeginFlash(0U), "wrapper fixture owns flash exclusion");
    errno = ENOENT;
    const int writeResult = __wrap_esp_partition_write(
        &physical_sdk::settingsPartition, 99U, &physical, 17U);
    Need(writeResult == 1234 && errno == EIO && physical_sdk::inputError == ENOENT &&
         physical_sdk::offset == 99U && physical_sdk::buffer == &physical && physical_sdk::bytes == 17U,
         "write forwards the exact const buffer, offset, length and errors");
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 3U);
    errno = ENOENT;
    const int eraseResult = __wrap_esp_partition_erase_range(&physical_sdk::settingsPartition, 4096U, 4096U);
    Need(eraseResult == 1234 && errno == EIO && physical_sdk::inputError == ENOENT &&
         physical_sdk::offset == 4096U && physical_sdk::bytes == 4096U &&
         physical.Sample().access[2U].totalMs == 7U && !unchangedArbiter.TryBeginRender(),
         "erase preserves arguments, unsigned clock wrap and the existing flash lease");
    unchangedArbiter.EndFlash();
    physical.EndOperation();
    const auto retainedSample = physical.Sample();
    invoke(&physical_sdk::settingsPartition);
    Need(physical_sdk::clockReads == 2U &&
             physical.Sample().access[0U].calls == retainedSample.access[0U].calls,
         "unmeasured worker lease records only hardware ownership timing, preserving completed sample");
    physical.EndFlashLease();
    firmware::RegisterSettingsPhysicalAccess(nullptr);
    invoke(&physical_sdk::settingsPartition);
    Need(physical_sdk::clockReads == 0U, "pre-startup or disabled registration only forwards");
    firmware::SettingsPhysicalAccessCounter bounded;
    bounded.calls = UINT32_MAX; bounded.bytes = UINT32_MAX - 1U;
    bounded.totalMs = UINT32_MAX - 1U;
    bounded.Record(SIZE_MAX, UINT32_MAX);
    bounded.Record(3U, 4U);
    Need(bounded.calls == UINT32_MAX && bounded.bytes == UINT32_MAX &&
         bounded.totalMs == UINT32_MAX && bounded.maxMs == UINT32_MAX,
         "physical counters and requested bytes saturate without wrap");
    physical_sdk::result = physical_sdk::resultError = 0;
}

void RunPhysicalTransactionTiming() {
    firmware::SettingsPhysicalAccess physical;
    hardware::RmtFlashArbiter arbiter;
    physical.Bind(physical_sdk::task, &physical_sdk::settingsPartition);
    physical.BindFlashArbiter(arbiter);
    firmware::RegisterSettingsPhysicalAccess(&physical);
    physical.BeginFlashLease();
    physical.BeginTransaction();
    physical_sdk::delayMs = 0U;
    physical_sdk::delayUs = 375U;
    physical_sdk::result = 1234;
    physical_sdk::resultError = EIO;
    physical_sdk::microClockReads = 0U;
    errno = ENOENT;
    const auto result = __wrap_esp_partition_read(
        &physical_sdk::settingsPartition, 19U, &physical, 128U);
    Need(result == 1234 && errno == EIO && physical_sdk::inputError == ENOENT,
         "transaction microtimer preserves failed driver result and errno");
    physical_sdk::delayUs = 625U;
    (void)__wrap_esp_partition_read(&physical_sdk::settingsPartition, 0U, nullptr, 256U);
    const auto work = physical.TransactionSample().work.physical[0U];
    Need(work.calls == 2U && work.bytes == 384U && work.totalUs == 1000U && work.maxUs == 625U,
         "submillisecond reads accumulate before rounding rather than disappearing");
    physical_sdk::task = &physical;
    (void)__wrap_esp_partition_read(&physical_sdk::settingsPartition, 0U, nullptr, 128U);
    physical_sdk::task = &physical_sdk::taskToken;
    (void)__wrap_esp_partition_read(&physical_sdk::otherPartition, 0U, nullptr, 128U);
    Need(physical_sdk::microClockReads == 4U &&
             physical.TransactionSample().work.physical[0U].calls == 2U,
         "transaction timing excludes other tasks and partitions without probing their clock");
    physical.SetTransactionPaging(true);
    physical_sdk::delayUs = 800U;
    (void)__wrap_esp_partition_write(&physical_sdk::settingsPartition, 0U, nullptr, 64U);
    physical_sdk::delayUs = 1500U;
    (void)__wrap_esp_partition_erase_range(&physical_sdk::settingsPartition, 4096U, 4096U);
    physical.SetTransactionPaging(false);
    const auto sample = physical.TransactionSample();
    Need(sample.available && sample.work.physical[1U].calls == 0U &&
             sample.work.physical[2U].calls == 0U &&
             sample.paging.physical[1U].totalUs == 800U &&
             sample.paging.physical[2U].totalUs == 1500U,
         "paging driver work has a separate partition and cannot double-count settings work");
    physical.EndTransaction();
    physical_sdk::microClockReads = 0U;
    (void)__wrap_esp_partition_read(&physical_sdk::settingsPartition, 0U, nullptr, 128U);
    Need(physical_sdk::microClockReads == 0U &&
             physical.TransactionSample().work.physical[0U].calls == 2U,
         "completed measurements remain unchanged without an idle physical-work timer");
    physical.BeginTransaction();
    Need(physical.TransactionSample().work.physical[0U].calls == 0U &&
             physical.TransactionSample().paging.physical[1U].calls == 0U,
         "new request starts with independent work and paging measurements");
    physical.EndTransaction();
    physical.EndFlashLease();
    firmware::RegisterSettingsPhysicalAccess(nullptr);
    physical_sdk::delayUs = 0U;
    physical_sdk::result = physical_sdk::resultError = 0;
}

// Actual Arduino FrameFile/FrameStore and wrapped partition entry with the
// real hardware arbiter. Only SDK filesystem leaves and costs are controlled.
void RunPhysicalAcquireTimeout() {
    hardware::RmtFlashArbiter arbiter;
    firmware::SettingsPhysicalAccess physical;
    physical.Bind(physical_sdk::task, &physical_sdk::settingsPartition);
    physical.BindFlashArbiter(arbiter);
    physical.BeginFlashLease();
    firmware::RegisterSettingsPhysicalAccess(&physical);
    physical_sdk::activeArbiter = &arbiter;
    physical_sdk::result = ESP_OK;
    physical_sdk::resultError = 0;
    physical_sdk::delayMs = 0U;
    Need(arbiter.TryBeginRender(), "timeout fixture starts an actual RMT transfer");
    arbiter.EndRender(false);
    const uint32_t generation = arbiter.CompletedRenderGeneration();
    fxe::ManualMillis::SetNowMs(UINT32_MAX - 49U);
    static unsigned waits;
    waits = 0U;
    physical_sdk::flashWait = [] {
        fxe::ManualMillis::AdvanceMs(1U);
        ++waits;
        Need(waits <= 100U, "unsafe RMT acquisition must return within the existing 100ms hardware budget");
    };
    const auto timeoutWait = physical_sdk::flashWait;
    const unsigned calls = physical_sdk::calls;
    errno = ENOENT;
    const auto result = __wrap_esp_partition_write(
        &physical_sdk::settingsPartition, 0U, nullptr, 128U);
    Need(result == ESP_ERR_TIMEOUT && errno == ETIMEDOUT && waits == 100U &&
         physical_sdk::calls == calls && arbiter.CompletedRenderGeneration() == generation,
         "wrapped SDK timeout preserves quarantine, retires its request and never issues unsafe flash");
    Need(!arbiter.TryBeginFlash(generation), "timeout cannot certify uncertain hardware quiescence");
    Need(arbiter.TryBeginRender(), "existing RMT recovery remains usable after timeout");
    arbiter.EndRender();
    Need(arbiter.TryBeginRender(), "no expired pending claim blocks later healthy LED transfers");
    arbiter.EndRender();
    physical_sdk::flashWait = nullptr;
    const auto recovered = __wrap_esp_partition_write(
        &physical_sdk::settingsPartition, 0U, nullptr, 128U);
    Need(recovered == ESP_OK && physical_sdk::calls == calls + 1U &&
         arbiter.TryBeginRender(), "confirmed retry restores normal protected SDK access and release");
    arbiter.EndRender();
    Need(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()), "another flash owner holds the hardware");
    arbiter.RequestFlash(); // A separate writer must survive this caller's timeout.
    waits = 0U;
    physical_sdk::flashWait = timeoutWait;
    const auto competing = __wrap_esp_partition_write(
        &physical_sdk::settingsPartition, 0U, nullptr, 128U);
    Need(competing == ESP_ERR_TIMEOUT && errno == ETIMEDOUT && waits == 100U &&
         physical_sdk::calls == calls + 1U && !arbiter.TryBeginRender() &&
         !arbiter.TryBeginRequestedFlash(),
         "canceling this wrapped SDK request cannot release another active flash owner");
    arbiter.EndFlash();
    Need(!arbiter.TryBeginRender() && arbiter.TryBeginRequestedFlash(),
         "timed-out caller preserves the separate writer's pending request");
    arbiter.EndFlash();
    Need(arbiter.TryBeginRender(), "no timed-out pending claim remains after the other writer finishes");
    arbiter.EndRender();
    physical_sdk::flashWait = nullptr;
    std::printf("PHYSICAL_ACQUIRE_TIMEOUT wait_ms=%u primitive_calls=%u tick_wrap=1 quarantine=retained recovery=1 other_owner=preserved other_waiter=preserved\n",
        waits, physical_sdk::calls - calls);
    physical_sdk::activeArbiter = nullptr;
    physical.EndFlashLease();
    firmware::RegisterSettingsPhysicalAccess(nullptr);
}

void RunFrameFileSdkSchedulingCase() {
    char directory[] = "/tmp/litch-frame-sdk-independent-XXXXXX";
    Need(::mkdtemp(directory) != nullptr, "create isolated FrameFile SDK fixture");
    settings_save_sdk::root = directory;
    firmware::SettingsPhysicalAccess physical;
    physical.Bind(physical_sdk::task, &physical_sdk::settingsPartition);
    firmware::RegisterSettingsPhysicalAccess(&physical);
    hardware::RmtFlashArbiter renderer;
    physical.BindFlashArbiter(renderer);
    const auto generation = renderer.CompletedRenderGeneration();
    physical_sdk::activeArbiter = &renderer;
    physical_sdk::result = physical_sdk::resultError = 0;
    physical_sdk::delayMs = 4U;
    static unsigned sdkYields;
    sdkYields = 0U;
    physical_sdk::sdkYield = [] {
        Need(!physical_sdk::activeArbiter->TryBeginRender(), "SDK internal yield retains hardware flash exclusion");
        ++sdkYields; std::this_thread::yield();
    };
    static bool readyFrameSchedule;
    readyFrameSchedule = false;
    settings_save_sdk::access = [](const char* operation) {
        if (std::strcmp(operation, "frame_writer_write") == 0) {
            Need(__wrap_esp_partition_write(&physical_sdk::settingsPartition,
                0U, nullptr, 128U) == ESP_OK, "actual VFS program leaf retains physical exclusion");
            return;
        }
        if (std::strcmp(operation, "frame_sync") != 0) return;
        for (unsigned index = 0U; index < 1000U; ++index) {
            Need(__wrap_esp_partition_read(&physical_sdk::settingsPartition,
                index * 128U, nullptr, 128U) == ESP_OK, "normal SDK leaf completes");
            if (readyFrameSchedule && index % 100U == 49U)
                physical_sdk::activeArbiter->RequestRender();
        }
    };
    firmware::ArduinoLeaseFrameFile file;
    Need(file.SetAccessOps({&physical,
        [](void* context) { static_cast<firmware::SettingsPhysicalAccess*>(context)->BeginFlashLease(); },
        [](void* context, firmware::ArduinoLeaseFrameFile::AccessOperation, size_t, uint32_t) {
            static_cast<firmware::SettingsPhysicalAccess*>(context)->EndFlashLease();
        }}), "file retains SDK observation without future-frame scheduling");
    const auto ops = file.StoreOps();
    constexpr size_t bytes = modules::litchcraft_frame_store_layout::FRAME_SLOT_BYTES;
    std::array<uint8_t, bytes> payload{}, readback{};
    for (size_t index = 0U; index < bytes; ++index) payload[index] = uint8_t(index * 37U + 11U);
    const size_t offset = modules::litchcraft_frame_store_layout::Layout::SlotOffset(0U, 0U);
    Need(ops.write(ops.context, offset, payload.data(), payload.size()), "real file writes exact authored slot");
    sdkYields = 0U;
    static unsigned cooperationWaits;
    static uint32_t cooperationTicks;
    cooperationWaits = 0U;
    cooperationTicks = 0U;
    physical_sdk::flashWait = [] {
        Need(physical_sdk::requestedDelayTicks == 1U,
             "a blocked physical acquisition yields one scheduler tick");
        Need(physical_sdk::activeArbiter->TryBeginRequestedRender(),
            "an already-ready frame wins before the next metadata primitive");
        Need(!physical_sdk::activeArbiter->TryBeginRequestedFlash(),
             "the waiting SDK caller cannot overlap the frame");
        physical_sdk::activeArbiter->EndRender();
        ++cooperationWaits;
        cooperationTicks += physical_sdk::requestedDelayTicks;
        fxe::ManualMillis::AdvanceMs(physical_sdk::requestedDelayTicks);
        physical_sdk::elapsedUs += static_cast<uint64_t>(physical_sdk::requestedDelayTicks) * 1000U;
    };
    physical.BeginTransaction();
    const auto calls = physical_sdk::calls;
    const auto started = fxe::Millis();
    readyFrameSchedule = true;
    Need(ops.sync(ops.context), "checked sync and close finish with hardware exclusion and no future-frame wait");
    readyFrameSchedule = false;
    const auto elapsed = fxe::Millis() - started;
    physical.EndTransaction();
    physical_sdk::flashWait = nullptr;
    const auto timing = physical.TransactionSample().work;
    Need(physical_sdk::calls - calls == 1000U && sdkYields == 1000U && elapsed == 4010U &&
         cooperationWaits == 10U && cooperationTicks == 10U &&
         timing.handoffs == 10U && timing.handoffWaitMs == 10U &&
         timing.physical[0U].totalUs == 4000000U && timing.physical[0U].maxUs == 4000U &&
         renderer.CompletedRenderGeneration() == generation + 10U,
         "only actual ready transfers interrupt the metadata train, without artificial save sleeps or future frames");
    Need(ops.read(ops.context, offset, readback.data(), readback.size()) && readback == payload &&
         fxe::Crc32(readback.data(), readback.size()) == fxe::Crc32(payload.data(), payload.size()),
         "independent scheduling preserves complete bytes and CRC after real sync");
    std::printf("FRAME_SYNC_HARDWARE_EXCLUSION sdk_calls=%u lower_yields=%u cooperation=%u/%lu requested_ticks=%lu primitive_us=%lu app_render_generation=%lu elapsed_ms=%lu exact_crc=1\n",
        physical_sdk::calls - calls, sdkYields, cooperationWaits, static_cast<unsigned long>(timing.handoffWaitMs),
        static_cast<unsigned long>(cooperationTicks),
        static_cast<unsigned long>(timing.physical[0U].totalUs), static_cast<unsigned long>(generation), static_cast<unsigned long>(elapsed));
    Need(renderer.TryBeginRender(), "LED output resumes after the completed primitive");
    static unsigned hardwareWaits;
    hardwareWaits = 0U;
    physical_sdk::flashWait = [] {
        Need(!physical_sdk::activeArbiter->TryBeginRender(), "pending flash blocks a second LED transfer");
        ++hardwareWaits;
        physical_sdk::activeArbiter->EndRender();
    };
    physical.BeginFlashLease();
    Need(__wrap_esp_partition_read(&physical_sdk::settingsPartition, 0U, nullptr, 0U) == ESP_OK,
         "flash proceeds after only the already-active LED transfer completes");
    physical.EndFlashLease();
    Need(hardwareWaits == 1U && renderer.CompletedRenderGeneration() == generation + 11U,
         "current-transfer completion requires no additional animation frame");
    physical_sdk::flashWait = nullptr;
    // Same real FrameStore/File chain, with controlled SDK leaves inside the
    // real VFS calls. No parallel codec, parser, queue or persistence model.
    Need(file.Remove(), "hardware regression starts an empty real FrameStore");
    namespace store = modules::litchcraft_frame_store;
    store::FrameStore frames(file.StoreOps());
    store::Manifest manifest{}; manifest.frameCount = 1U;
    store::TransactionToken token{};
    store::Frame frame{}; frame.durationMs = 57U;
    frame.emitters[0].color = {17U, 29U, 43U};
    Need(frames.Recover() == store::Status::NoArtwork &&
         frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok &&
         frames.WriteFrame(token, 0U, frame) == store::Status::Ok &&
         frames.Commit(token) == store::Status::Ok,
         "actual FrameStore commits through File and guarded wrapped SDK leaves");
    store::FrameStore reopened(file.StoreOps()); store::Frame durable{};
    Need(reopened.Recover() == store::Status::Ok && reopened.ReadFrame(0U, durable) == store::Status::Ok &&
         durable.durationMs == frame.durationMs && durable.emitters[0].color.r == 17U &&
         durable.emitters[0].color.g == 29U && durable.emitters[0].color.b == 43U,
         "actual FrameStore persisted readback and CRC survive hardware exclusion");
    std::printf("FRAME_STORE_HARDWARE_EXCLUSION active_waits=%u guarded_sdk_calls=%u crc_readback=1\n",
        hardwareWaits, physical_sdk::deniedRenderAttempts);
    file.CloseCachedHandles();
    physical_sdk::activeArbiter = nullptr;
    physical_sdk::sdkYield = nullptr;
    physical_sdk::delayMs = 0U;
    firmware::RegisterSettingsPhysicalAccess(nullptr);
    settings_save_sdk::access = nullptr;
    std::filesystem::remove_all(directory);
}

void RunFrameReadPathCase() {
    namespace store = modules::litchcraft_frame_store;
    using File = firmware::ArduinoLeaseFrameFile;
    char directory[] = "/tmp/litch-frame-read-paths-XXXXXX";
    Need(::mkdtemp(directory) != nullptr, "create read path fixture");
    settings_save_sdk::root = directory;
    settings_save_sdk::access = ObserveAccess;
    hardware::RmtFlashArbiter arbiter;
    saveArbiter = &arbiter;
    const File::AccessOps access{&arbiter,
        [](void* context) {
            auto& owner = *static_cast<hardware::RmtFlashArbiter*>(context);
            Need(owner.TryBeginFlash(owner.CompletedRenderGeneration()), "read path owns flash");
        },
        [](void* context, firmware::ArduinoLeaseFrameFile::AccessOperation, size_t, uint32_t) { static_cast<hardware::RmtFlashArbiter*>(context)->EndFlash(); }};
    for (const bool custom : {false, true}) {
        File file(custom ? "/custom.r0" : File::DEFAULT_ROOT0_PATH,
                  custom ? "/custom.r1" : File::DEFAULT_ROOT1_PATH,
                  custom ? "/custom.b0" : File::DEFAULT_BODY0_PATH,
                  custom ? "/custom.b1" : File::DEFAULT_BODY1_PATH);
        Need(file.SetAccessOps(access), "configure existing read path access owner");
        const auto ops = file.StoreOps();
        const uint8_t rootMarker = 0x91U;
        Need(ops.write(ops.context, store::FrameStore::RootOffset(0U), &rootMarker, 1U) &&
                 ops.sync(ops.context), "seed a real authoritative root for fresh stdio reads");
        for (uint8_t bank = 0U; bank < 2U; ++bank) {
            for (uint8_t part = 0U; part < File::BODY_FILES_PER_BANK; ++part) {
                const size_t offset = store::FrameStore::BodyOffset(bank) + part * File::BODY_FILE_BYTES;
                const uint8_t marker = bank * File::BODY_FILES_PER_BANK + part;
                Need(ops.write(ops.context, offset, &marker, 1U) && ops.sync(ops.context),
                     "write a distinct marker through every Arduino physical path");
            }
        }
        for (uint8_t bank = 0U; bank < 2U; ++bank) {
            for (uint8_t part = 0U; part < File::BODY_FILES_PER_BANK; ++part) {
                const size_t offset = store::FrameStore::BodyOffset(bank) + part * File::BODY_FILE_BYTES;
                const uint8_t marker = bank * File::BODY_FILES_PER_BANK + part;
                const auto opens = settings_save_sdk::counts["frame_read_open"];
                const auto exists = settings_save_sdk::counts["exists"];
                const auto sdkOpens = settings_save_sdk::counts["open"];
                for (unsigned read = 0U; read < 2U; ++read) {
                    uint8_t actual = 0xffU;
                    Need(ops.read(ops.context, offset, &actual, 1U) && actual == marker,
                         "fresh and retained Arduino reads resolve both banks and all sixteen suffixes");
                }
                Need(settings_save_sdk::counts["frame_read_open"] == opens + 1U &&
                         settings_save_sdk::counts["exists"] == exists &&
                         settings_save_sdk::counts["open"] == sdkOpens && firmware::frameReadHandles == 1U,
                     "body replay retains one stdio reader without Arduino exists or duplicate opens");
            }
        }
        const size_t boundary = store::FrameStore::BodyOffset(1U) + 10U * File::BODY_FILE_BYTES - 1U;
        const uint8_t expected[]{0x72U, 0x39U};
        uint8_t actual[2U]{};
        Need(ops.write(ops.context, boundary, expected, sizeof(expected)) && ops.sync(ops.context) &&
                 ops.read(ops.context, boundary, actual, sizeof(actual)) &&
                 std::equal(std::begin(actual), std::end(actual), std::begin(expected)),
             "Arduino read and write loops advance exact bytes across physical parts nine and ten");
        namespace fixed = modules::litchcraft_frame_store_layout;
        const size_t summary = fixed::Layout::SummaryOffset(0U, 0U);
        const uint8_t summaryMarker = 0x67U;
        uint8_t observed = 0U;
        Need(ops.write(ops.context, summary, &summaryMarker, 1U) && ops.sync(ops.context) &&
                 ops.read(ops.context, summary, &observed, 1U) && observed == summaryMarker,
             "summary uses the same retained stdio reader owner");
        const auto summaryOpens = settings_save_sdk::counts["frame_read_open"];
        Need(ops.write(ops.context, store::FrameStore::RootOffset(0U), &rootMarker, 1U) &&
                 ops.sync(ops.context) && ops.read(ops.context, summary, &observed, 1U) &&
                 observed == summaryMarker && settings_save_sdk::counts["frame_read_open"] == summaryOpens + 1U,
             "writing a root invalidates its aliased retained summary handle");
        const auto rootOpens = settings_save_sdk::counts["frame_read_open"];
        const auto rootCloses = settings_save_sdk::counts["frame_read_close"];
        for (unsigned read = 0U; read < 2U; ++read) {
            observed = 0U;
            Need(ops.read(ops.context, store::FrameStore::RootOffset(0U), &observed, 1U) && observed == rootMarker,
                 "authoritative root bytes remain freshly read beside the retained summary");
        }
        Need(settings_save_sdk::counts["frame_read_open"] == rootOpens + 2U &&
                 settings_save_sdk::counts["frame_read_close"] == rootCloses + 2U && firmware::frameReadHandles == 1U,
             "each authoritative root read owns one transient open and close");
        Need(file.Remove(), "remove only the path fixture's artwork files");
        Need(firmware::frameReadHandles == 0U, "explicit removal closes the retained reader");
    }
    namespace fixed = modules::litchcraft_frame_store_layout;
    const std::array<size_t, 5U> offsets{{store::FrameStore::RootOffset(0U),
        store::FrameStore::RootOffset(1U), fixed::Layout::StageOffset(),
        fixed::Layout::SummaryOffset(0U, 0U), fixed::Layout::SummaryOffset(1U, 0U)}};
    for (const size_t length : {95U, 96U, 112U}) {
        const std::array<std::string, 2U> paths{{"/" + std::string(length - 1U, 'a'),
                                               "/" + std::string(length - 1U, 'b')}};
        for (const auto& path : paths) std::ofstream(settings_save_sdk::Path(path.c_str()), std::ios::binary).close();
        for (size_t index = 0U; index < offsets.size(); ++index) {
            File::PhysicalSpan span{};
            Need(File::ResolvePhysicalSpan(offsets[index], 1U, span), "resolve root read bound fixture");
            const bool second = span.region == File::Region::Root1 || span.region == File::Region::Summary1;
            std::fstream seed(settings_save_sdk::Path(paths[second ? 1U : 0U].c_str()),
                              std::ios::binary | std::ios::in | std::ios::out);
            seed.seekp(span.offset);
            seed.put(static_cast<char>(0x91U + index));
            Need(seed.good(), "seed each root, stage and summary boundary byte");
        }
        File file(paths[0U].c_str(), paths[1U].c_str());
        Need(file.SetAccessOps({&arbiter, access.begin,
            [](void* context, firmware::ArduinoLeaseFrameFile::AccessOperation, size_t, uint32_t) {
                static_cast<hardware::RmtFlashArbiter*>(context)->EndFlash();
                errno = EBUSY;
            }}), "path-bound fixture also checks an errno-changing lease release");
        const auto ops = file.StoreOps();
        for (size_t index = 0U; index < offsets.size(); ++index) {
            const unsigned calls = settings_save_sdk::calls;
            uint8_t actual = 0xffU;
            errno = 0;
            const bool read = ops.read(ops.context, offsets[index], &actual, 1U);
            const int error = errno;
            const unsigned filesystemCalls = settings_save_sdk::calls - calls;
            std::printf("[ROOT_READ_BOUND] length=%zu region=%zu read=%u errno=%d fs_calls=%u\n",
                        length, index, read, error, filesystemCalls);
            if (length < 96U) {
                Need(read && actual == 0x91U + index && filesystemCalls != 0U,
                     "95-byte custom root, stage and summary paths retain exact read access");
            } else {
                Need(!read && error == ENAMETOOLONG && filesystemCalls == 0U,
                     "overlong root, stage and summary paths fail before filesystem access and retain ENAMETOOLONG");
            }
        }
    }
    settings_save_sdk::access = nullptr;
    std::filesystem::remove_all(directory);
    std::puts("[FRAME_READ_PATHS] defaults=32 custom=32 fresh=ok retained=ok boundary09_10=ok");
}

void RunFrameReadFaultCase() {
    namespace store = modules::litchcraft_frame_store;
    namespace fixed = modules::litchcraft_frame_store_layout;
    using File = firmware::ArduinoLeaseFrameFile;
    using Fault = firmware::FrameReadFault;
    char directory[] = "/tmp/litch-frame-read-faults-XXXXXX";
    Need(::mkdtemp(directory) != nullptr, "create read fault fixture");
    settings_save_sdk::root = directory;
    settings_save_sdk::access = ObserveAccess;
    hardware::RmtFlashArbiter arbiter;
    saveArbiter = &arbiter;
    File file;
    Need(file.SetAccessOps({&arbiter,
        [](void* context) {
            auto& owner = *static_cast<hardware::RmtFlashArbiter*>(context);
            Need(owner.TryBeginFlash(owner.CompletedRenderGeneration()), "faulted read owns flash");
        },
        [](void* context, File::AccessOperation, size_t, uint32_t) {
            static_cast<hardware::RmtFlashArbiter*>(context)->EndFlash();
        }}), "configure read fault access owner");
    const auto ops = file.StoreOps();
    const std::array<size_t, 3U> offsets{{store::FrameStore::RootOffset(0U),
        store::FrameStore::BodyOffset(0U), fixed::Layout::SummaryOffset(0U, 0U)}};
    for (size_t index = 0U; index < offsets.size(); ++index) {
        const uint8_t marker = 0x62U + index;
        Need(ops.write(ops.context, offsets[index], &marker, 1U) && ops.sync(ops.context),
             "seed authoritative root, body and summary bytes through real writer");
    }
    struct ReadFailure { Fault operation; int error; };
    const ReadFailure faults[]{{Fault::Open, EIO}, {Fault::Open, ENOMEM},
        {Fault::SeekEnd, EIO}, {Fault::Size, EIO}, {Fault::Size, ENOMEM},
        {Fault::SeekRead, EIO}, {Fault::Read, EIO}};
    for (size_t index = 0U; index < offsets.size(); ++index) {
        for (const auto fault : faults) {
            file.CloseCachedHandles();
            firmware::frameReadFault = fault.operation;
            firmware::frameReadError = fault.error;
            uint8_t actual = 0U;
            Need(!ops.read(ops.context, offsets[index], &actual, 1U) &&
                     firmware::frameReadFault == Fault::None,
                 "open, size, seek and read errors cannot be accepted as erased artwork");
            Need(ops.read(ops.context, offsets[index], &actual, 1U) && actual == 0x62U + index,
                 "failed transient and retained reads recover exact bytes on retry");
        }
    }
    Need(file.Remove() && firmware::frameReadHandles == 0U,
         "missing-file cases start with no retained handles or artwork files");
    const auto exists = settings_save_sdk::counts["exists"];
    const auto sdkOpens = settings_save_sdk::counts["open"];
    for (const auto offset : offsets) {
        std::array<uint8_t, 4U> erased{};
        Need(ops.read(ops.context, offset, erased.data(), erased.size()) &&
                 std::all_of(erased.begin(), erased.end(), [](uint8_t value) { return value == 0xffU; }),
             "confirmed ENOENT alone supplies a successful erased-byte read");
        for (const int error : {EIO, ENOMEM}) {
            firmware::frameReadFault = Fault::Open;
            firmware::frameReadError = error;
            Need(!ops.read(ops.context, offset, erased.data(), erased.size()) &&
                     firmware::frameReadFault == Fault::None,
                 "a failed missing-path lookup is not proof that artwork is absent");
        }
    }
    Need(settings_save_sdk::counts["exists"] == exists && settings_save_sdk::counts["open"] == sdkOpens &&
             firmware::frameReadHandles == 0U,
         "all missing-file and failed-open decisions use one checked stdio open without Arduino existence probes");
    settings_save_sdk::access = nullptr;
    std::filesystem::remove_all(directory);
    std::puts("[FRAME_READ_FAULTS] root_body_summary=3 errors=21 retry=exact ENOENT=erased EIO_ENOMEM=failed handles=balanced");
}

void RunFrameWriterSwitchFailureCase() {
    namespace store = modules::litchcraft_frame_store;
    char directory[] = "/tmp/litch-frame-writer-fault-XXXXXX";
    Need(::mkdtemp(directory) != nullptr, "create checked writer fixture");
    settings_save_sdk::root = directory;
    settings_save_sdk::access = ObserveAccess;
    hardware::RmtFlashArbiter arbiter;
    saveArbiter = &arbiter;
    {
        firmware::ArduinoLeaseFrameFile file;
        file.SetAccessOps({&arbiter,
            [](void* context) {
                auto& owner = *static_cast<hardware::RmtFlashArbiter*>(context);
                Need(owner.TryBeginFlash(owner.CompletedRenderGeneration()), "writer owns flash");
            },
            [](void* context, firmware::ArduinoLeaseFrameFile::AccessOperation, size_t, uint32_t) {
                static_cast<hardware::RmtFlashArbiter*>(context)->EndFlash();
                errno = EBUSY;
            }});
        const auto ops = file.StoreOps();
        const uint8_t marker = 0x7cU;
        for (const auto fault : {firmware::BankPrepareFault::Sync,
                                firmware::BankPrepareFault::Close}) {
            Need(ops.write(ops.context, store::FrameStore::RootOffset(0U), &marker, 1U),
                 "region switch starts with a retained real root writer");
            firmware::rootWriterFault = fault;
            Need(!ops.write(ops.context, store::FrameStore::BodyOffset(1U), &marker, 1U) &&
                     errno == EIO && !std::filesystem::exists(settings_save_sdk::root /
                         "litchcraft_frames.b1.00") && !ops.sync(ops.context) && errno == EIO,
                 "failed old-writer barrier stops before opening or writing the next region");
            Need(!file.Remove() && errno == EIO &&
                     std::filesystem::exists(settings_save_sdk::root / "litchcraft_frames.r0"),
                 "destructive removal also preserves files instead of hiding a failed writer barrier");
            firmware::rootWriterFault = firmware::BankPrepareFault::None;
            Need(ops.write(ops.context, store::FrameStore::RootOffset(0U), &marker, 1U) &&
                     ops.sync(ops.context), "new real write and barrier recover the failed owner");
        }
    }
    settings_save_sdk::access = nullptr;
    std::filesystem::remove_all(directory);
}

void RunAbortedWriterFailureRecoveryCase() {
    namespace store = modules::litchcraft_frame_store;
    using Stream = modules::litchcraft_frame_stream::FrameStream;
    for (const auto fault : {firmware::BankPrepareFault::Sync,
                            firmware::BankPrepareFault::Close}) {
        char directory[] = "/tmp/litch-frame-writer-retry-XXXXXX";
        Need(::mkdtemp(directory) != nullptr, "create aborted writer retry fixture");
        settings_save_sdk::root = directory;
        settings_save_sdk::access = ObserveAccess;
        hardware::RmtFlashArbiter arbiter;
        saveArbiter = &arbiter;
        const firmware::ArduinoLeaseFrameFile::AccessOps access{&arbiter,
            [](void* context) {
                auto& owner = *static_cast<hardware::RmtFlashArbiter*>(context);
                Need(owner.TryBeginFlash(owner.CompletedRenderGeneration()), "edit retry owns flash");
            },
            [](void* context, firmware::ArduinoLeaseFrameFile::AccessOperation, size_t, uint32_t) {
                static_cast<hardware::RmtFlashArbiter*>(context)->EndFlash();
                errno = EBUSY;
            }};
        Stream stream;
        {
            firmware::ArduinoLeaseFrameFile file;
            file.SetAccessOps(access);
            store::FrameStore frames(file.StoreOps());
            store::Manifest manifest{};
            store::Frame original{};
            original.emitters[0U].color.r = 9U;
            store::TransactionToken token{};
            Need(frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok &&
                     frames.WriteFrame(token, 0U, original) == store::Status::Ok &&
                     frames.Commit(token) == store::Status::Ok &&
                     stream.StageFirstFrame(manifest, 1U, original) &&
                     stream.PaintDisplayed(0U, 0U, 1U, {100U, 80U, 70U}, true),
                 "actual stream owns the accepted pending painted frame");
            stream.RequestImmediateSave();
            firmware::rootWriterFault = fault;
            Need(stream.Service(frames, false) == Stream::ServiceResult::IoFailure &&
                     stream.HasUnpersistedEdit() && frames.Generation() == 1U,
                 "real edit service aborts failed stage sync/close without acknowledging paint");
            firmware::rootWriterFault = firmware::BankPrepareFault::None;
            const auto ops = file.StoreOps();
            Need(!ops.sync(ops.context) && errno == EIO,
                 "failed old owner cannot acknowledge an empty-handle durability barrier");
            file.CloseCachedHandles();
            // Worker IoFailure recovery destroys FrameTransferStorage after
            // retaining stream dirty intent, then reconstructs it for retry:
            // settings_persistence_worker.cpp Release/StartFrameStorage.
        }
        {
            firmware::ArduinoLeaseFrameFile file;
            file.SetAccessOps(access);
            store::FrameStore frames(file.StoreOps());
            Need(frames.Recover() == store::Status::Ok && frames.Generation() == 1U &&
                     stream.HasUnpersistedEdit(),
                 "worker-equivalent owner reconstruction restores durable root and pending paint");
            stream.RequestImmediateSave();
            Need(stream.Service(frames, false) == Stream::ServiceResult::Idle &&
                     !stream.HasUnpersistedEdit() && frames.Generation() == 2U,
                 "next actual edit service saves pending paint after checked barriers recover");
            store::Frame saved{};
            Need(frames.ReadFrame(0U, saved) == store::Status::Ok &&
                     saved.emitters[0U].color.r == 100U,
                 "retry preserves the human's accepted color and completes durability");
        }
        settings_save_sdk::access = nullptr;
        std::filesystem::remove_all(directory);
    }
    std::puts("[FRAME_WRITER_RETRY] aborted_sync=ok aborted_close=ok pending_paint=saved");
}

void RunPersistedBankResetCase() {
    namespace store = modules::litchcraft_frame_store;
    for (const auto fault : {firmware::BankPrepareFault::ImplicitSync,
                            firmware::BankPrepareFault::SizeRead}) {
        char directory[] = "/tmp/litch-bank-reset-readback-XXXXXX";
        Need(::mkdtemp(directory) != nullptr, "create persisted bank reset fixture");
        settings_save_sdk::root = directory;
        settings_save_sdk::access = ObserveAccess;
        hardware::RmtFlashArbiter arbiter;
        saveArbiter = &arbiter;
        bool stopped = false;
        unsigned sizeReads = 0U;
        size_t retainedBytes = 0U;
        int result = 0, error = 0;
        {
            firmware::ArduinoLeaseFrameFile file;
            file.SetAccessOps({&arbiter,
                [](void* context) {
                    auto& owner = *static_cast<hardware::RmtFlashArbiter*>(context);
                    Need(owner.TryBeginFlash(owner.CompletedRenderGeneration()), "reset readback owns flash");
                },
                [](void* context, firmware::ArduinoLeaseFrameFile::AccessOperation, size_t, uint32_t) {
                    static_cast<hardware::RmtFlashArbiter*>(context)->EndFlash();
                    errno = EBUSY;
                }});
            store::FrameStore frames(file.StoreOps());
            store::Manifest manifest{};
            store::Frame original{};
            original.emitters[0U].color.r = 9U;
            store::TransactionToken token{};
            Need(frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok &&
                     frames.WriteFrame(token, 0U, original) == store::Status::Ok &&
                     frames.Commit(token) == store::Status::Ok,
                 "size-readback fixture has a complete selected resident");
            const auto selectedPath = settings_save_sdk::root / "litchcraft_frames.b0.00";
            const auto incomingPath = settings_save_sdk::root / "litchcraft_frames.b1.00";
            const std::string selected = ReadBytes(selectedPath);
            if (fault == firmware::BankPrepareFault::ImplicitSync) {
                std::ofstream(incomingPath, std::ios::binary) << std::string(8192U, 'x');
            }
            Need(frames.Begin(manifest, 1U, 2U, token) == store::Status::Ok,
                 "reset readback replacement admits no bytes before preparation");
            firmware::bankPrepareFault = fault;
            const unsigned beforeReads = firmware::bankPrepareSizeReads;
            const auto status = frames.WriteFrame(token, 0U, original);
            error = errno;
            result = static_cast<int>(status);
            sizeReads = firmware::bankPrepareSizeReads - beforeReads;
            retainedBytes = std::filesystem::file_size(incomingPath);
            stopped = status == store::Status::IoFailure && error == EIO && sizeReads == 1U &&
                frames.Commit(token) == store::Status::Incomplete &&
                frames.Generation() == 1U && ReadBytes(selectedPath) == selected;
            firmware::bankPrepareFault = firmware::BankPrepareFault::None;
            if (stopped) {
                Need(frames.WriteFrame(token, 0U, original) == store::Status::Ok &&
                         frames.Commit(token) == store::Status::Ok &&
                         std::filesystem::file_size(incomingPath) == store::STAGE_SLOT_BYTES &&
                         ReadBytes(selectedPath) == selected,
                     "real retry resets and verifies persisted EOF before publishing replacement");
            } else {
                (void)frames.Abort(token);
            }
        }
        settings_save_sdk::access = nullptr;
        std::filesystem::remove_all(directory);
        std::printf("[BANK_RESET_READBACK] fault=%s status=%d errno=%d size_reads=%u retained_eof=%zu\n",
                    fault == firmware::BankPrepareFault::ImplicitSync ? "implicit_sync" : "fresh_size_read",
                    result, error, sizeReads, retainedBytes);
        Need(stopped, "successful descriptor/barriers cannot replace persisted zero-length proof");
    }
}

std::string ReadBodyBank(const std::filesystem::path& base,
                         uint8_t parts = firmware::ArduinoLeaseFrameFile::BODY_FILES_PER_BANK) {
    std::string result;
    for (uint8_t part = 0U; part < parts; ++part) {
        char suffix[8]{}; std::snprintf(suffix, sizeof(suffix), ".%02u", unsigned(part));
        result += ReadBytes(base.string() + suffix);
    }
    return result;
}

void RunReplacementBankTailCase() {
    namespace store = modules::litchcraft_frame_store;
    namespace fixed = modules::litchcraft_frame_store_layout;
    char directory[] = "/tmp/litch-frame-replacement-XXXXXX";
    Need(::mkdtemp(directory) != nullptr, "create replacement-bank fixture");
    settings_save_sdk::root = directory;
    settings_save_sdk::access = ObserveAccess;
    hardware::RmtFlashArbiter arbiter;
    saveArbiter = &arbiter;
    bool boundedTail = false;
    size_t oldBytes = 0U, newBytes = 0U;
    unsigned prepares = 0U;
    // Equivalent controlled old adapter: it supplied only read/write/sync.
    const bool oldTailFault = std::getenv("LITCH_FRAME_RETAIN_OLD_TAIL") != nullptr;
    const uint8_t usedParts = (32U + firmware::ArduinoLeaseFrameFile::FRAMES_PER_BODY_FILE - 1U) /
        firmware::ArduinoLeaseFrameFile::FRAMES_PER_BODY_FILE;
    const unsigned expectedPreparation = oldTailFault ? 0U : usedParts;
    const unsigned firstPreparation = oldTailFault ? 0U : 1U;
    {
        firmware::ArduinoLeaseFrameFile file;
        file.SetAccessOps({&arbiter,
            [](void* context) {
                auto& owner = *static_cast<hardware::RmtFlashArbiter*>(context);
                Need(owner.TryBeginFlash(owner.CompletedRenderGeneration()),
                     "replacement preparation acquires the normal flash lease");
            },
            [](void* context, firmware::ArduinoLeaseFrameFile::AccessOperation, size_t, uint32_t) {
                static_cast<hardware::RmtFlashArbiter*>(context)->EndFlash();
                errno = EBUSY; // A release callback cannot replace the I/O error.
            }});
        auto storeOps = file.StoreOps();
        if (oldTailFault) storeOps.prepareReplacementBank = nullptr;
        store::FrameStore frames(storeOps);
        store::Manifest manifest{};
        manifest.frameCount = 32U;
        manifest.active = true;
        manifest.playbackOnEntry = true;
        store::Frame frame{};
        frame.emitters[0U].color = {12U, 34U, 56U};
        store::TransactionToken token{};
        Need(frames.Recover() == store::Status::NoArtwork &&
                 frames.Begin(manifest, 0U, 1U, token) == store::Status::Ok,
             "replacement fixture begins through the actual Store");
        const unsigned initialPrepares = firmware::bankPrepareOpens;
        for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
            Need(frames.WriteFrame(token, index, frame) == store::Status::Ok,
                 "selected bank contains complete real frames");
        }
        Need(frames.Commit(token) == store::Status::Ok &&
                 firmware::bankPrepareOpens == initialPrepares + expectedPreparation,
             "generated replacement prepares exactly once even with no prior root");
        const auto activePath = settings_save_sdk::root / "litchcraft_frames.b0";
        const auto incomingPath = settings_save_sdk::root / "litchcraft_frames.b1";
        const std::string activeBytes = ReadBodyBank(activePath);
        std::ofstream(settings_save_sdk::root / "settings.json") << "retained settings";
        auto largeManifest = manifest;
        largeManifest.frameCount = store::FRAME_COUNT;
        Need(frames.Begin(largeManifest, 1U, 2U, token) == store::Status::Ok,
             "older inactive animation begins as a valid 256-frame resident");
        for (uint16_t index = 0U; index < largeManifest.frameCount; ++index) {
            Need(frames.WriteFrame(token, index, frame) == store::Status::Ok,
                 "full predecessor bank contains 256 valid fixed slots");
        }
        store::FrameStore::ResidentDescriptor selected{};
        Need(frames.Commit(token) == store::Status::Ok && frames.Resident(0U, selected) &&
                 frames.SelectResident(selected.rootSlot, selected.generation,
                     selected.bodyCrc32) == store::Status::Ok && frames.Generation() == 1U,
             "selecting the older animation makes the full bank inactive without body writes");
        const auto ops = file.StoreOps();
        uint8_t retained = 0U;
        Need(ops.read(ops.context, fixed::Layout::BodyOffset(1U) +
                 fixed::BODY_BANK_BYTES - 1U, &retained, 1U),
             "oversized inactive bank also has a retained Arduino reader");
        // A used file also starts oversized. This retains a real EOF-proof
        // regression when unused files are intentionally outside the manifest.
        std::ofstream(incomingPath.string() + ".01", std::ios::binary)
            << std::string(firmware::ArduinoLeaseFrameFile::BODY_FILE_BYTES + 64U, 'x');
        oldBytes = ReadBodyBank(incomingPath).size();
        std::vector<store::Frame> source(manifest.frameCount, frame);
        std::vector<uint8_t> body;
        for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
            source[index].emitters[0U].color.r = static_cast<uint8_t>(index + 80U);
            std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
            size_t size = 0U;
            Need(modules::litchcraft_wire_codec::FrameCodec::Encode(
                     index == 0U ? nullptr : &source[0U], source[index],
                     encoded.data(), encoded.size(), size), "encode actual wire frame");
            const size_t at = body.size();
            body.resize(at + store::WIRE_RECORD_HEADER_BYTES + size);
            fxe::WriteU16LE(static_cast<uint16_t>(size), body.data() + at);
            fxe::WriteU32LE(fxe::Crc32(encoded.data(), size), body.data() + at + 2U);
            std::copy_n(encoded.data(), size,
                        body.data() + at + store::WIRE_RECORD_HEADER_BYTES);
        }
        const uint32_t crc = fxe::Crc32(body.data(), body.size());
        prepares = firmware::bankPrepareOpens;
        Need(frames.BeginWire(manifest, 1U, 2U, body.size(), crc, token) ==
                 store::Status::Ok && firmware::bankPrepareOpens == prepares &&
                 frames.WriteBodyChunk(token, 0U, body.data(), 1U) == store::Status::Ok,
             "first admitted wire byte prepares the unpublished bank, not Begin");
        store::TransactionToken rejected{};
        Need(frames.BeginWire(manifest, 1U, 2U, body.size(), crc, rejected) ==
                 store::Status::Incomplete &&
                 frames.Begin(manifest, 1U, 3U, rejected) == store::Status::Incomplete &&
                 frames.WriteBodyChunk(token, 1U, body.data() + 1U, body.size() / 2U) ==
                     store::Status::Ok &&
                 frames.Checkpoint(token, 1U + body.size() / 2U) == store::Status::Ok &&
                 frames.WriteBodyChunk(token, 1U + body.size() / 2U,
                     body.data() + 1U + body.size() / 2U,
                     body.size() - 1U - body.size() / 2U) == store::Status::Ok &&
                 frames.Commit(token) == store::Status::Ok &&
                 firmware::bankPrepareOpens == prepares + expectedPreparation,
             "duplicate/conflicting Begin and resumed chunks cannot truncate admitted bytes");
        // Logical artwork reaches only the files containing manifest slots.
        // Old unused files remain bounded and unreachable until their first use.
        newBytes = ReadBodyBank(incomingPath, usedParts).size();
        boundedTail = newBytes == manifest.frameCount * fixed::FRAME_SLOT_BYTES;
        Need(ReadBodyBank(activePath) == activeBytes &&
                 ReadBytes(settings_save_sdk::root / "settings.json") == "retained settings",
             "replacement preserves the selected physical bank and settings exactly");
        file.CloseCachedHandles();
        store::FrameStore recovered(file.StoreOps());
        Need(recovered.Recover() == store::Status::Ok && recovered.Generation() == 3U &&
                 recovered.ResidentCount() == 2U,
             "both completed residents retain their real recovery authority");
        for (uint16_t index = 0U; index < manifest.frameCount; ++index) {
            store::Frame actual{};
            Need(recovered.ReadFrame(index, actual) == store::Status::Ok &&
                     actual.emitters[0U].color.r == source[index].emitters[0U].color.r,
                 "reloaded replacement retains distinct frame parity");
        }
        std::vector<uint8_t> reverse(body.size());
        size_t read = 0U;
        Need(recovered.ReadWireBody(3U, 0U, reverse.data(), reverse.size(), read) ==
                 store::Status::Ok && read == body.size() && reverse == body,
             "full canonical readback and hash still match after replacement");
        for (const auto fault : {firmware::BankPrepareFault::Sync,
                                firmware::BankPrepareFault::Close}) {
            const std::string inactiveBytes = ReadBodyBank(activePath);
            const std::string selectedBytes = ReadBodyBank(incomingPath);
            const unsigned beforeRootFault = firmware::bankPrepareOpens;
            firmware::rootWriterFault = fault;
            Need(recovered.BeginWire(manifest, 3U, 4U, body.size(), crc, token) ==
                     store::Status::Ok &&
                     recovered.WriteBodyChunk(token, 0U, body.data(), 1U) ==
                         store::Status::IoFailure && errno == EIO &&
                     firmware::bankPrepareOpens == beforeRootFault &&
                     !ops.sync(ops.context) && errno == EIO &&
                     recovered.Commit(token) == store::Status::Incomplete,
                 "root sync/close error blocks ALL preparation and cannot become an empty-handle success");
            firmware::rootWriterFault = firmware::BankPrepareFault::None;
            Need(ReadBodyBank(activePath) == inactiveBytes &&
                     ReadBodyBank(incomingPath) == selectedBytes,
                 "failed root barrier preserves active and inactive body bytes exactly");
            Need(recovered.WriteBodyChunk(token, 0U, body.data(), 1U) ==
                     store::Status::Ok && firmware::bankPrepareOpens == beforeRootFault + firstPreparation &&
                     recovered.Abort(token) == store::Status::Ok &&
                     recovered.Generation() == 3U && ReadBodyBank(incomingPath) == selectedBytes,
                 "same admitted transaction retries real root writes/barriers before any truncation");
        }
        const unsigned beforeEdit = firmware::bankPrepareOpens;
        Need(recovered.BeginEdit(3U, 3U, token) == store::Status::Ok,
             "ordinary COW edit starts from the selected recovered generation");
        Need(recovered.ReplaceFrame(token, 0U, frame) == store::Status::Ok,
             "ordinary COW replacement is accepted through the real edit owner");
        Need(recovered.Commit(token) == store::Status::Ok,
             "ordinary COW replacement commits through the shared stage journal");
        // CommitFrameEdit stages in Root0; FoldStage reuses the now-vacant
        // original physical slot. These already-written parts must not reset.
        Need(firmware::bankPrepareOpens == beforeEdit,
             "ordinary COW preserves already-written physical parts without preparation");
        const unsigned afterFirstEdit = firmware::bankPrepareOpens;
        Need(recovered.BeginEdit(4U, 3U, token) == store::Status::Ok &&
                 recovered.ReplaceFrame(token, 1U, frame) == store::Status::Ok &&
                 recovered.Commit(token) == store::Status::Ok &&
                 firmware::bankPrepareOpens == afterFirstEdit,
             "a later ordinary COW edit never retruncates a written part");
        store::Frame edited{};
        Need(recovered.ReadFrame(0U, edited) == store::Status::Ok &&
                 edited.emitters[0U].color.r == frame.emitters[0U].color.r &&
                 recovered.ReadFrame(1U, edited) == store::Status::Ok &&
                 edited.emitters[0U].color.r == frame.emitters[0U].color.r &&
                 recovered.ReadFrame(31U, edited) == store::Status::Ok &&
                 edited.emitters[0U].color.r == source[31U].emitters[0U].color.r,
             "journal edits retain both changed frames and untouched tail frames");
        const std::string selectedBytes = ReadBodyBank(incomingPath);
        for (const auto fault : {firmware::BankPrepareFault::Open,
                                firmware::BankPrepareFault::Sync,
                                firmware::BankPrepareFault::Close}) {
            firmware::bankPrepareFault = fault;
            Need(recovered.BeginWire(manifest, 5U, 4U, body.size(), crc, token) ==
                     store::Status::Ok &&
                     recovered.WriteBodyChunk(token, 0U, body.data(), 1U) ==
                         store::Status::IoFailure &&
                     errno == (fault == firmware::BankPrepareFault::Open ? EACCES : EIO),
                 "checked truncate/open, sync and close failure preserve errno across lease release");
            Need(recovered.Commit(token) == store::Status::Incomplete &&
                     recovered.Abort(token) == store::Status::Ok,
                 "failed preparation cannot publish; cancellation permits a fresh retry");
            file.CloseCachedHandles();
            store::FrameStore reboot(file.StoreOps());
            store::Frame actual{};
            Need(reboot.Recover() == store::Status::Ok && reboot.Generation() == 5U &&
                     reboot.ResidentCount() == 1U &&
                     reboot.ReadFrame(31U, actual) == store::Status::Ok &&
                     actual.emitters[0U].color.r == source[31U].emitters[0U].color.r &&
                     ReadBodyBank(incomingPath) == selectedBytes,
                 "failed preparation/reboot preserves selected artwork and rejects retired alternate");
            firmware::bankPrepareFault = firmware::BankPrepareFault::None;
        }
        Need(recovered.BeginWire(manifest, 5U, 5U, body.size(), crc, token) ==
                 store::Status::Ok &&
                 recovered.WriteBodyChunk(token, 0U, body.data(), body.size() / 2U) ==
                     store::Status::Ok &&
                 recovered.Checkpoint(token, body.size() / 2U) == store::Status::Ok,
             "new transaction retries preparation after cancelled failures");
        file.CloseCachedHandles();
        store::FrameStore interrupted(file.StoreOps());
        Need(interrupted.Recover() == store::Status::Ok && interrupted.Generation() == 5U &&
                 interrupted.ResidentCount() == 1U && ReadBodyBank(incomingPath) == selectedBytes,
             "interrupted prepared body cannot recover as a published replacement");
        recovered.Abort(token);
    }
    settings_save_sdk::access = nullptr;
    std::filesystem::remove_all(directory);
    std::printf("[REPLACEMENT_BANK] old_eof=%zu replacement_eof=%zu expected=%zu "
                "wire_prepares=%u generated_prepares=%u edit_prepares=0 "
                "root_faults=2 prepare_faults=3 recovery=ok old_tail_fault=%u\n",
                oldBytes, newBytes, 32U * fixed::FRAME_SLOT_BYTES,
                expectedPreparation, expectedPreparation, oldTailFault);
    Need(boundedTail, "whole replacement must bound every manifest-used file after checked reset");
}
} // namespace

void RunFirstBodyPreparationLatencyProbe() {
    fxe::ScopedMillisProviderOverride probeClock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(100U);
    namespace store = modules::litchcraft_frame_store;
    char directory[] = "/tmp/litch-first-body-prepare-XXXXXX";
    Need(::mkdtemp(directory) != nullptr, "create first-body preparation fixture");
    settings_save_sdk::root = directory;
    settings_save_sdk::access = ObserveAccess;
    hardware::RmtFlashArbiter arbiter;
    saveArbiter = &arbiter;
    uint32_t elapsed = 0U, reportedBody = 0U;
    unsigned prepares = 0U;
    const auto parts = firmware::ArduinoLeaseFrameFile::BODY_FILES_PER_BANK;
    {
        firmware::ArduinoLeaseFrameFile file;
        Need(file.SetAccessOps({&arbiter,
            [](void* context) {
                auto& owner = *static_cast<hardware::RmtFlashArbiter*>(context);
                Need(owner.TryBeginFlash(owner.CompletedRenderGeneration()), "first body owns real flash exclusion");
            },
            [](void* context, firmware::ArduinoLeaseFrameFile::AccessOperation, size_t, uint32_t) {
                static_cast<hardware::RmtFlashArbiter*>(context)->EndFlash();
            }}), "bind actual FrameFile access owner");
        store::FrameStore frames(file.StoreOps());
        frames.SetTimingObserver([](void* context, store::FrameStore::TimingPhase phase, uint32_t ms) {
            if (phase == store::FrameStore::TimingPhase::BodyWrite) {
                auto& maximum = *static_cast<uint32_t*>(context);
                maximum = std::max(maximum, ms);
            }
        }, &reportedBody);
        store::Frame frame{};
        frame.emitters[0U].color.r = 9U;
        std::array<uint8_t, store::WIRE_FRAME_MAX_BYTES> encoded{};
        size_t encodedSize = 0U;
        Need(modules::litchcraft_wire_codec::FrameCodec::Encode(nullptr, frame,
            encoded.data(), encoded.size(), encodedSize), "encode real first frame");
        std::vector<uint8_t> body(6U + encodedSize);
        fxe::WriteU16LE(uint16_t(encodedSize), body.data());
        fxe::WriteU32LE(fxe::Crc32(encoded.data(), encodedSize), body.data() + 2U);
        std::copy_n(encoded.data(), encodedSize, body.data() + 6U);
        store::Manifest manifest{};
        store::TransactionToken token{};
        Need(frames.BeginWire(manifest, 0U, 1U, uint32_t(body.size()),
            fxe::Crc32(body.data(), body.size()), token) == store::Status::Ok,
            "real BeginWire admits before replacement preparation");
        const unsigned before = firmware::bankPrepareOpens;
        delayedOperation = "bank_prepare_sync";
        operationDelayMs = 300U; // Controlled leaf cost, not an ESP32 measurement.
        const uint32_t started = fxe::Millis();
        Need(frames.WriteBodyChunk(token, 0U, body.data(), body.size()) == store::Status::Ok,
            "first body traverses real bank preparation and slot write");
        elapsed = fxe::Millis() - started;
        prepares = firmware::bankPrepareOpens - before;
        delayedOperation = ""; operationDelayMs = 0U;
        (void)frames.Abort(token);
    }
    settings_save_sdk::access = nullptr;
    std::filesystem::remove_all(directory);
    std::printf("[FIRST_BODY_PREPARE] bank_parts=%u prepared_parts=%u controlled_sync_ms=300 first_body_ms=%lu reported_body_ms=%lu\n",
        unsigned(parts), prepares, static_cast<unsigned long>(elapsed), static_cast<unsigned long>(reportedBody));
    Need(elapsed < 2000U, "first body must not exhaust shared Object recovery while preparing untouched parts");
}

int main() {
    if (std::getenv("LITCH_FIRST_BODY_PREPARE_LATENCY") != nullptr) { RunFirstBodyPreparationLatencyProbe(); return 0; }
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    if (std::getenv("LITCH_FRAME_READ_STDIO") != nullptr) {
        RunFrameReadPathCase();
        RunFrameReadFaultCase();
        return 0;
    }
    if (std::getenv("LITCH_STORAGE_ACQUIRE_TIMEOUT") != nullptr) { RunPhysicalAcquireTimeout(); return 0; }
    if (std::getenv("LITCH_STORAGE_DECOUPLED") != nullptr) { RunFrameFileSdkSchedulingCase(); return 0; }
    if (std::getenv("LITCH_FRAME_BANK_RESET_READBACK") != nullptr) {
        RunPersistedBankResetCase();
        return 0;
    }
    if (std::getenv("LITCH_FRAME_REPLACEMENT_BANK") != nullptr) {
        RunFrameWriterSwitchFailureCase();
        RunAbortedWriterFailureRecoveryCase();
        RunReplacementBankTailCase();
        return 0;
    }
    if (std::getenv("LITCH_SAVE_METADATA_OPENS") != nullptr) {
        RunMetadataOpenCase(MetadataCase::OpenCost);
        return 0;
    }
    if (std::getenv("LITCH_SAVE_PROMOTION_COUNT") != nullptr) {
        RunMetadataOpenCase(MetadataCase::PromotionRecovery);
        RunMetadataOpenCase(MetadataCase::PromotionWithoutVerification);
        return 0;
    }
    RunMetadataOpenCase(MetadataCase::OpenCost);
    RunMetadataOpenCase(MetadataCase::PromotionRecovery);
    RunMetadataOpenCase(MetadataCase::PromotionWithoutVerification);
    RunCase(true);
    RunCase(false);
    RunOperationWitnessCase();
    RunPhysicalWitnessCase();
    RunPhysicalWrapperFilters();
    RunPhysicalTransactionTiming();
    RunPhysicalAcquireTimeout();
    RunFrameFileSdkSchedulingCase();
    RunReplacementBankTailCase();
    RunFrameReadPathCase();
    RunFrameReadFaultCase();
    RunFrameWriterSwitchFailureCase();
    RunAbortedWriterFailureRecoveryCase();
    RunPersistedBankResetCase();
    RunFirstBodyPreparationLatencyProbe();
    std::puts("PASS Arduino save: bounded direct writes, checked durability, retained recovery, retry");
    return 0;
}
