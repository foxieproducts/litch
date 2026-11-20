#include <unity.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <Update.h>
#include <bootloader_common.h>
#include <esp_ota_ops.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "firmware/ota_platform_adapter.hpp"
#include "firmware/ota_boot_health.hpp"
#include "firmware/ota_runtime_evidence_internal.hpp"
#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "firmware/settings_physical_access.hpp"
#include "modules/ota_boot_policy.hpp"
#include "modules/ota_attempt_proof.hpp"
#include "modules/litchcraft_session.hpp"
#include "modules/sha256.hpp"
#include "bounded_hint_engine.hpp"

// Consume the hardware-only declarations before compiling the runtime owner
// below with the bounded fixture in place of the firmware aggregate.
#include "firmware/firmware_app_internal.hpp"
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_tasks_internal.hpp"

namespace ota_platform_contract {
using Image = std::array<uint8_t, 400U>;
using Results = fxe::test_support::TestResults;

uint32_t Crc(const uint32_t sequence) {
    uint32_t crc = 0U;
    for (unsigned byte = 0U; byte < 4U; ++byte) {
        crc ^= static_cast<uint8_t>(sequence >> (8U * byte));
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
        }
    }
    return crc ^ UINT32_MAX;
}
Image MakeImage(const uint8_t buildBase, const uint32_t epoch = 2U) {
    Image image{};
    image[0] = 0xE9U; image[1] = 2U; image[12] = 9U; image[23] = 1U;
    image[24] = 0x20U; image[27] = 0x3CU; image[28] = 0x10U; image[29] = 1U;
    image[32] = 0x32U; image[33] = 0x54U; image[34] = 0xCDU; image[35] = 0xABU;
    for (size_t index = 0; index < 32U; ++index) image[176U + index] = buildBase + index;
    const auto policy = modules::EncodeOtaBootPolicyDescriptor(epoch);
    std::copy(policy.begin(), policy.end(), image.begin() + 288U);
    image[305] = 0x80U; image[306] = 0xC8U; image[307] = 0x3FU; image[308] = 48U;
    for (size_t index = 0; index < 48U; ++index) image[312U + index] = 0xD0U + index;
    uint8_t checksum = 0xEFU;
    for (size_t index = 32U; index < 304U; ++index) checksum ^= image[index];
    for (size_t index = 312U; index < 360U; ++index) checksum ^= image[index];
    image[367] = checksum;
    modules::Sha256 sha;
    (void)sha.Update(image.data(), 368U);
    const auto digest = sha.Finish();
    std::copy(digest.begin(), digest.end(), image.begin() + 368U);
    return image;
}

// Compile the shipping SDK snapshot, software partition reader, adapter,
// transaction and executor. Only IDF/Arduino storage, task clock and reset
// boundaries are controlled. This is not electrical flash or bootloader HIL.
struct Board {
    std::array<esp_partition_t, 2U> slots{};
    esp_partition_t otadata{};
    std::array<esp_ota_select_entry_t, 2U> entries{};
    std::array<Image, 2U> images{{MakeImage(0x20U), MakeImage(0x60U)}};
    Image staged{};
    hardware::RmtFlashArbiter* arbiter = nullptr;
    unsigned running = 0U;
    uint8_t appCount = 2U;
    unsigned bootTarget = 0U;
    const esp_partition_t* updateTarget = nullptr;
    size_t expected = 0U, written = 0U;
    unsigned beginCalls = 0U, activationCalls = 0U, abortCalls = 0U;
    unsigned confirms = 0U, rollbacks = 0U, imageReads = 0U;
    bool leaseCorrect = true, unavailableRunning = false, configuredMismatch = false;
    bool confirmNoWrite = false, corruptRead = false, changeDuringImageRead = false;
    bool beginFails = false, endFails = false;
    bool corruptReadAfterActivation = false;
    bool publicationBeforeRelease = false, awaitingRelease = false;
    bool admissionBeforeSdkSuccess = false;
    uint32_t cacheSampleDuringRead = 0U;
    uint32_t activationSequenceDelta = 0U;
    TickType_t ticks = 0U;

    Board() {
        for (unsigned index = 0; index < 2U; ++index) {
            slots[index].type = ESP_PARTITION_TYPE_APP;
            slots[index].subtype = static_cast<esp_partition_subtype_t>(0x10U + index);
            slots[index].address = index == 0 ? 0x10000U : 0x340000U;
            slots[index].size = 0x330000U;
        }
        otadata.type = ESP_PARTITION_TYPE_DATA;
        otadata.subtype = ESP_PARTITION_SUBTYPE_DATA_OTA;
        otadata.address = 0xF000U; otadata.size = 0x2000U;
        std::memset(entries.data(), 0xFF, sizeof(entries));
        SetEntry(0U, 1U, ESP_OTA_IMG_VALID);
    }
    void SetEntry(unsigned index, uint32_t seq, uint32_t state) {
        entries[index].ota_seq = seq;
        entries[index].ota_state = state;
        entries[index].crc = Crc(seq);
    }
    int Active() const {
        const auto valid = [](const esp_ota_select_entry_t& entry) {
            return entry.ota_seq != UINT32_MAX && entry.crc == Crc(entry.ota_seq) &&
                   entry.ota_state != ESP_OTA_IMG_INVALID && entry.ota_state != ESP_OTA_IMG_ABORTED;
        };
        const bool first = valid(entries[0]), second = valid(entries[1]);
        return first && second ? (entries[0].ota_seq >= entries[1].ota_seq ? 0 : 1)
                               : first ? 0 : second ? 1 : -1;
    }
    void CheckLease() {
        if (arbiter->TryBeginRender()) { leaseCorrect = false; arbiter->EndRender(); }
    }
    void Boot() {
        running = bootTarget;
        entries[Active()].ota_state = ESP_OTA_IMG_PENDING_VERIFY;
    }
};
Board* g_board = nullptr;
UpdateClass update;

// Dispatch exactly one iteration of the shipping worker's request loop. Only
// RTOS scheduling/completion is synchronous; all capture/health/cache logic is
// production code. No second storage or worker behavior is implemented here.
struct RequestFinished {};
TaskFunction_t workerTask = nullptr;
void* workerContext = nullptr;
SemaphoreHandle_t workerCompletion = nullptr;
bool completionPosted = false;
unsigned notificationTakes = 0U;
bool pausedSettingsCase = false;
bool deferWorker = false;
bool queuedWorker = false;
bool foreignRead = false;
unsigned blockingCompletionTakes = 0U;
unsigned foreignCompletionConsumes = 0U;
unsigned workerNotifications = 0U;
void (*afterWorkerRelease)() = nullptr;
TaskHandle_t CreateWorker(TaskFunction_t task, const char*, uint32_t stackBytes, void* context,
                          uint32_t, StackType_t* stack, StaticTask_t* control, int) {
    if (stackBytes != 4096U || stack == nullptr || control == nullptr) return nullptr;
    workerTask = task; workerContext = context;
    return &workerTask;
}
SemaphoreHandle_t CreateCompletion(StaticSemaphore_t* storage) {
    workerCompletion = storage; completionPosted = false; return storage;
}
BaseType_t Take(SemaphoreHandle_t semaphore, TickType_t ticks) {
    if (semaphore != workerCompletion) return xSemaphoreTake(semaphore, ticks);
    if (ticks != 0U) ++blockingCompletionTakes;
    if (foreignRead && ticks == 0U && completionPosted) ++foreignCompletionConsumes;
    if (!completionPosted) return pdFALSE;
    completionPosted = false; return pdTRUE;
}
BaseType_t Give(SemaphoreHandle_t semaphore) {
    if (semaphore != workerCompletion) return xSemaphoreGive(semaphore);
    completionPosted = true; return pdTRUE;
}
void DispatchQueuedWorker() {
    queuedWorker = false;
    notificationTakes = 0U;
    try { workerTask(workerContext); } catch (const RequestFinished&) {}
}
BaseType_t Notify(TaskHandle_t) {
    ++workerNotifications;
    if (deferWorker) queuedWorker = true;
    else DispatchQueuedWorker();
    return pdPASS;
}
uint32_t TakeNotification(BaseType_t, TickType_t timeout) {
    if (timeout == 0U) return 0U;
    if (notificationTakes++ != 0U) throw RequestFinished{};
    return 1U;
}
TickType_t Tick() { return g_board->ticks; }
void Delay(uint32_t ticks) {
    g_board->ticks += ticks;
    if (pausedSettingsCase) std::this_thread::yield();
}
uint32_t EvidenceClock() {
    return g_board->ticks;
}
void AfterRelease() {
    auto& b = *g_board;
    if (b.awaitingRelease) {
        firmware::OtaBootEvidence cached;
        if (!firmware::CopyEsp32OtaBootEvidence(cached) ||
            cached.sample != b.cacheSampleDuringRead) b.publicationBeforeRelease = true;
        b.awaitingRelease = false;
    }
    if (afterWorkerRelease != nullptr) afterWorkerRelease();
}
const esp_partition_t* Running() {
    g_board->CheckLease();
    return g_board->unavailableRunning ? nullptr : &g_board->slots[g_board->running];
}
const esp_partition_t* Configured() {
    g_board->CheckLease();
    const int active = g_board->Active();
    if (active < 0) return nullptr;
    const unsigned slot = (g_board->entries[active].ota_seq - 1U) % 2U;
    return &g_board->slots[g_board->configuredMismatch ? 1U - slot : slot];
}
uint8_t PartitionCount() { g_board->CheckLease(); return g_board->appCount; }
const esp_partition_t* Next(const esp_partition_t*) {
    g_board->CheckLease();
    return &g_board->slots[1U - g_board->running];
}
const esp_partition_t* Find(esp_partition_type_t type, esp_partition_subtype_t subtype, const char*) {
    // The worker uses the SDK's already initialized immutable partition list
    // outside a flash lease. Actual partition reads below still require it.
    if (type == ESP_PARTITION_TYPE_DATA) return &g_board->otadata;
    for (auto& slot : g_board->slots) if (slot.subtype == subtype) return &slot;
    return nullptr;
}
esp_err_t Read(const esp_partition_t* part, size_t offset, void* destination, size_t size) {
    auto& board = *g_board;
    board.CheckLease();
    if (board.corruptRead) return ESP_FAIL;
    if (part == &board.otadata) {
        firmware::OtaBootEvidence cached;
        if (!firmware::CopyEsp32OtaBootEvidence(cached)) return ESP_FAIL;
        board.cacheSampleDuringRead = cached.sample;
        board.awaitingRelease = true;
        const size_t index = offset / 0x1000U;
        if (index >= 2U || size != sizeof(esp_ota_select_entry_t)) return ESP_FAIL;
        std::memcpy(destination, &board.entries[index], size);
    } else {
        ++board.imageReads;
        const unsigned slot = part->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
        if (slot >= 2U || offset + size > board.images[slot].size()) return ESP_FAIL;
        std::memcpy(destination, board.images[slot].data() + offset, size);
        if (board.changeDuringImageRead) {
            board.changeDuringImageRead = false;
            const int active = board.Active();
            board.SetEntry(active, board.entries[active].ota_seq + 2U, board.entries[active].ota_state);
        }
    }
    return ESP_OK;
}
uint32_t SelectorCrc(const esp_ota_select_entry_t* entry) { return Crc(entry->ota_seq); }
bool SelectorValid(const esp_ota_select_entry_t* entry) {
    return entry->ota_seq != UINT32_MAX && entry->crc == Crc(entry->ota_seq) &&
           entry->ota_state != ESP_OTA_IMG_INVALID && entry->ota_state != ESP_OTA_IMG_ABORTED;
}
int Active(esp_ota_select_entry_t* entries) {
    const bool first = SelectorValid(&entries[0]), second = SelectorValid(&entries[1]);
    return first && second ? (entries[0].ota_seq >= entries[1].ota_seq ? 0 : 1)
                           : first ? 0 : second ? 1 : -1;
}
esp_err_t MarkValid() {
    auto& board = *g_board; board.CheckLease(); ++board.confirms;
    if (!board.confirmNoWrite) board.entries[board.Active()].ota_state = ESP_OTA_IMG_VALID;
    return ESP_OK;
}
esp_err_t Rollback() {
    auto& board = *g_board; board.CheckLease(); ++board.rollbacks;
    const int active = board.Active();
    board.entries[active].ota_state = ESP_OTA_IMG_INVALID;
    const int backup = board.Active();
    if (backup < 0) return ESP_FAIL;
    board.running = (board.entries[backup].ota_seq - 1U) % 2U;
    board.bootTarget = board.running;
    return ESP_OK; // Real SDK restarts internally; fixture exposes next-boot state.
}
} // namespace ota_platform_contract

bool UpdateClass::begin(size_t size, int, int, uint8_t, const char*) {
    auto& b = *ota_platform_contract::g_board; b.CheckLease(); ++b.beginCalls;
    modules::OtaAdmissionEvidence admission;
    if (firmware::ota_evidence_internal::CopyAdmission(admission) &&
        admission.phase == modules::OtaAdmissionPhase::Receiving) b.admissionBeforeSdkSuccess = true;
    b.updateTarget = ota_platform_contract::Next(nullptr); b.expected = size; b.written = 0U;
    return !b.beginFails && b.updateTarget != nullptr && size <= b.updateTarget->size;
}
size_t UpdateClass::write(uint8_t* data, size_t size) {
    auto& b = *ota_platform_contract::g_board; b.CheckLease();
    if (b.written + size > b.staged.size()) return 0U;
    std::memcpy(b.staged.data() + b.written, data, size); b.written += size; return size;
}
bool UpdateClass::end(bool remaining) {
    auto& b = *ota_platform_contract::g_board; b.CheckLease(); ++b.activationCalls;
    if (b.endFails || remaining || b.written != b.expected) return false;
    b.bootTarget = b.updateTarget->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
    b.images[b.bootTarget] = b.staged;
    const int active = b.Active(); uint32_t seq = b.entries[active].ota_seq + 1U;
    while ((seq - 1U) % 2U != b.bootTarget) ++seq;
    b.SetEntry(1U - active, seq + b.activationSequenceDelta, ESP_OTA_IMG_NEW);
    if (b.corruptReadAfterActivation) b.corruptRead = true;
    return true;
}
void UpdateClass::abort() { ++ota_platform_contract::g_board->abortCalls; }
bool UpdateClass::hasError() { return false; }

namespace ota_platform_contract {
struct RuntimeBle {
    bool available = true;
    unsigned accepted = 0U;
    bool IsAdvertising() const { return available; }
    bool IsConnected() const { return false; }
    void ReserveBootFirmwareDigest() {}
    void ReleaseBootFirmwareDigest() {}
    void PublishOtaRuntimeAccepted() { ++accepted; }
    std::array<uint8_t, modules::OTA_POST_BOOT_SHA256_BYTES> digest{};
    void PublishVerifiedRunningFirmwareDigest(const uint8_t* value) {
        std::copy_n(value, digest.size(), digest.begin());
    }
};
struct RuntimeGuard {
    unsigned healthy = 0U;
    void MarkBootHealthy() { ++healthy; }
};
struct HealthRuntimeFixture {
    fxe::SettingsStore& settings;
    RuntimeBle ble;
    hardware::RmtFlashArbiter& rmtFlashArbiter;
    RuntimeGuard bootGuard;
    firmware::RuntimeBootHealthState runtimeBootHealth;
    struct Animation {
        uint32_t startupCompletedAtMs = UINT32_MAX;
        const Animation& Receipt() const { return *this; }
    } ledAnimation;
    struct Game {
        modules::LitchcraftSession session;
        const modules::LitchcraftSession& Litchcraft() const { return session; }
    } gamePersistence;
    bool bootFirstNonzeroRenderLogged = false;
    bool otaBootAwaitingHealth = true;
    bool factoryBootOnly = false;
    bool safeModeActive = false;
};
} // namespace ota_platform_contract
namespace firmware_internal {
using OtaWorkerHealthRuntimeFixture = ota_platform_contract::HealthRuntimeFixture;
}

namespace firmware {
bool QueueEsp32FirmwareDigestForContract(modules::FirmwareDigestProvider& provider);
}

#define ARDUINO 1
#define LITCH_ESP32 1
#define LITCH_ESP32S3 1
#define ReadEsp32ImageIdentity ReadEsp32ImageIdentityForContract
#define QueueEsp32FirmwareDigest QueueEsp32FirmwareDigestForContract
#define ReadEsp32FirmwareDigest ReadEsp32FirmwareDigestForContract
#define StartEsp32FirmwareDigest StartEsp32FirmwareDigestForContract
#define ReadEsp32OtaVerifiedSourceOnWorker ReadEsp32OtaVerifiedSourceOnWorkerForContract
#define VerifyEsp32OtaUpdateSource VerifyEsp32OtaUpdateSourceForContract
#define ReadEsp32OtaBootSnapshotWithFlashLease ReadEsp32OtaBootSnapshotWithFlashLeaseForContract
#define InitializeEsp32OtaPlatformAdapter InitializeEsp32OtaPlatformAdapterForContract
#define InitializeEsp32OtaPostBootWorker InitializeEsp32OtaPostBootWorkerForContract
#define ReadEsp32OtaRunningState ReadEsp32OtaRunningStateForContract
#define ProcessEsp32OtaBootHealth ProcessEsp32OtaBootHealthForContract
#define BeginEsp32OtaBootHealth BeginEsp32OtaBootHealthForContract
#define PollEsp32OtaBootHealth PollEsp32OtaBootHealthForContract
#define ResolveEsp32OtaBootHealth ResolveEsp32OtaBootHealthForContract
#define IsEsp32OtaBootPreparationFinished IsEsp32OtaBootPreparationFinishedForContract
#define PreparePendingOtaBootHealth PreparePendingOtaBootHealthForWorkerContract
#define CheckPendingOtaAtBoot CheckPendingOtaAtBootForWorkerContract
#define CommitPendingOtaAtHealthBoundary CommitPendingOtaAtHealthBoundaryForWorkerContract
#define ReportPendingOtaBootFailure ReportPendingOtaBootFailureForWorkerContract
#define verifyRollbackLater VerifyRollbackLaterForWorkerContract
#define esp_ota_get_running_partition ota_platform_contract::Running
#define esp_ota_get_boot_partition ota_platform_contract::Configured
#define esp_ota_get_app_partition_count ota_platform_contract::PartitionCount
#define esp_ota_get_next_update_partition ota_platform_contract::Next
#define esp_partition_find_first ota_platform_contract::Find
#define esp_partition_read ota_platform_contract::Read
#define bootloader_common_get_active_otadata ota_platform_contract::Active
#define bootloader_common_ota_select_crc ota_platform_contract::SelectorCrc
#define bootloader_common_ota_select_valid ota_platform_contract::SelectorValid
#define xTaskGetTickCount ota_platform_contract::Tick
#define vTaskDelay ota_platform_contract::Delay
#define xTaskCreateStaticPinnedToCore ota_platform_contract::CreateWorker
#define xSemaphoreCreateBinaryStatic ota_platform_contract::CreateCompletion
#define xSemaphoreTake ota_platform_contract::Take
#define xSemaphoreGive ota_platform_contract::Give
#define xTaskNotifyGive ota_platform_contract::Notify
#define ulTaskNotifyTake ota_platform_contract::TakeNotification
// Observational binding only: native execution cannot measure an ESP task stack.
#define uxTaskGetStackHighWaterMark(task) 0U
// Scheduling is target-only; this fixture exercises the worker protocol.
#define vTaskPrioritySet(task, priority) static_cast<void>(priority)
#define esp_ota_mark_app_valid_cancel_rollback ota_platform_contract::MarkValid
#define esp_ota_mark_app_invalid_rollback_and_reboot ota_platform_contract::Rollback
#define Update ota_platform_contract::update
// The real RMT release executes first; inspect cache before the subsequent
// production publication. This catches publication moved inside the lease.
#define EndFlash() EndFlash(); ota_platform_contract::AfterRelease()
#include "../../src/ota_post_boot_snapshot.cpp"
#include "../../src/ota_image_identity_esp32.cpp"
#include "../../src/firmware_digest_provider_esp32.cpp"
#define AcquireFlash WorkerAcquireFlash
#define FLASH_ACQUIRE_TIMEOUT_MS WORKER_FLASH_ACQUIRE_TIMEOUT_MS
#include "../../src/ota_post_boot_worker.cpp"
#undef FLASH_ACQUIRE_TIMEOUT_MS
#undef AcquireFlash
#include "../../src/ota_platform_adapter.cpp"
#include "../../src/ota_boot_health.cpp"
#define FirmwareSystem OtaWorkerHealthRuntimeFixture
#define PollRuntimeBootHealth PollRuntimeBootHealthForWorkerContract
#define StartupPresentationComplete(sys) ((sys).ledAnimation.startupCompletedAtMs != UINT32_MAX)
#include "../../src/firmware_boot_health_runtime.cpp"
#undef StartupPresentationComplete
#undef PollRuntimeBootHealth
#undef FirmwareSystem
#undef verifyRollbackLater
#undef ReportPendingOtaBootFailure
#undef CommitPendingOtaAtHealthBoundary
#undef CheckPendingOtaAtBoot
#undef EndFlash
#undef Update
#undef esp_ota_mark_app_invalid_rollback_and_reboot
#undef esp_ota_mark_app_valid_cancel_rollback
#undef uxTaskGetStackHighWaterMark
#undef vTaskPrioritySet
#undef ulTaskNotifyTake
#undef xTaskNotifyGive
#undef xSemaphoreGive
#undef xSemaphoreTake
#undef xSemaphoreCreateBinaryStatic
#undef xTaskCreateStaticPinnedToCore
#undef vTaskDelay
#undef xTaskGetTickCount
#undef bootloader_common_ota_select_valid
#undef bootloader_common_ota_select_crc
#undef bootloader_common_get_active_otadata
#undef esp_partition_read
#undef esp_partition_find_first
#undef esp_ota_get_next_update_partition
#undef esp_ota_get_app_partition_count
#undef esp_ota_get_boot_partition
#undef esp_ota_get_running_partition
#undef InitializeEsp32OtaPostBootWorker
#undef ProcessEsp32OtaBootHealth
#undef PreparePendingOtaBootHealth
#undef IsEsp32OtaBootPreparationFinished
#undef ResolveEsp32OtaBootHealth
#undef PollEsp32OtaBootHealth
#undef BeginEsp32OtaBootHealth
#undef ReadEsp32OtaRunningState
#undef InitializeEsp32OtaPlatformAdapter
#undef ReadEsp32OtaBootSnapshotWithFlashLease
#undef VerifyEsp32OtaUpdateSource
#undef ReadEsp32OtaVerifiedSourceOnWorker
#undef StartEsp32FirmwareDigest
#undef ReadEsp32FirmwareDigest
#undef QueueEsp32FirmwareDigest
#undef ReadEsp32ImageIdentity
#undef LITCH_ESP32S3
#undef LITCH_ESP32
#undef ARDUINO

namespace ota_platform_contract {
modules::OtaPostBootExecutionResult Health(modules::OtaPostBootHealth health) {
    return firmware::ProcessEsp32OtaBootHealthForContract(health);
}
void Reset(Board& board, modules::OtaPlatformAdapter& adapter) {
    adapter.Abort(); auto* arbiter = board.arbiter; board = Board{}; board.arbiter = arbiter;
}
bool Transfer(modules::OtaPlatformAdapter& adapter, const Image& image) {
    return adapter.Begin(image.size()) && adapter.Write(image.data(), 31U) &&
           adapter.Write(image.data() + 31U, image.size() - 31U) && adapter.End();
}

std::atomic<uint32_t> settingsClock{0U};
uint32_t SettingsClock() { return settingsClock.load(); }

template <typename Action>
void SynchronousFlashDuringSettings(Results& r, Board& board,
                                   hardware::RmtFlashArbiter& arbiter, Action action) {
    // Ordinary settings work no longer holds or pauses the application RMT
    // arbiter. OTA retains only its existing active-transfer exclusion.
    const uint32_t generation = arbiter.CompletedRenderGeneration();
    const uint32_t started = board.ticks;
    const bool completed = action();
    r.Check(completed && board.ticks - started < 5000U &&
            arbiter.CompletedRenderGeneration() == generation,
            "synchronous OTA completes without a settings-owned future LED frame");
}

void RetainedHealthOwnership(Results& r, Board& board,
                             modules::OtaPlatformAdapter& adapter) {
    using Status = firmware::OtaBootHealthRequestStatus;
    using Result = modules::OtaPostBootExecutionResult;
    Reset(board, adapter);
    deferWorker = true;
    blockingCompletionTakes = foreignCompletionConsumes = 0U;
    const unsigned notifications = workerNotifications;
    firmware::OtaBootHealthTicket ticket = 0U;
    r.Check(firmware::BeginEsp32OtaBootHealthForContract(modules::OtaPostBootHealth::NotReported, ticket) ==
                Status::Pending && ticket != 0U && queuedWorker,
            "health submission retains one addressed production worker request");
    Result result = Result::RollbackFailed;
    std::array<uint8_t, modules::OTA_POST_BOOT_SHA256_BYTES> imageSha256{};
    for (unsigned tick = 0U; tick < 5U; ++tick) {
        r.Check(firmware::PollEsp32OtaBootHealthForContract(ticket, result, imageSha256) == Status::Pending &&
                    result == Result::RollbackFailed,
                "pending polls return immediately without inventing or consuming a result");
    }
    firmware::OtaBootHealthTicket refused = 99U;
    r.Check(firmware::BeginEsp32OtaBootHealthForContract(modules::OtaPostBootHealth::Failed, refused) ==
                Status::Busy && refused == 0U,
            "another health request cannot replace or cancel the retained live job");
    r.Check(firmware::PollEsp32OtaBootHealthForContract(ticket + 1U, result, imageSha256) == Status::Stale,
            "wrong ticket cannot observe or consume another owner's health job");
    modules::OtaPostBootRunningState state{};
    modules::OtaPostBootSnapshot source{};
    foreignRead = true;
    r.Check(!firmware::ReadEsp32OtaRunningStateForContract(state) &&
                !firmware::VerifyEsp32OtaUpdateSourceForContract(source),
            "running-state and upload-source consumers refuse a pending health job");
    foreignRead = false;
    r.Check(firmware::ResolveEsp32OtaBootHealthForContract(ticket,
                modules::OtaPostBootHealth::Passed) == Status::Pending,
            "authoritative health resolves the same retained preparation before dispatch");
    deferWorker = false;
    DispatchQueuedWorker();
    foreignRead = true;
    r.Check(!firmware::ReadEsp32OtaRunningStateForContract(state) &&
                !firmware::VerifyEsp32OtaUpdateSourceForContract(source),
            "posted health completion remains owned until its exact poll consumes it");
    foreignRead = false;
    r.Check(foreignCompletionConsumes == 0U && blockingCompletionTakes == 0U &&
                workerNotifications == notifications + 2U,
            "health lifecycle has one submit and resolve, no blocking wait, foreign consume or replacement");
    r.Check(firmware::PollEsp32OtaBootHealthForContract(ticket, result, imageSha256) == Status::Complete &&
                result == Result::Confirmed,
            "existing full integrity and coherent VALID proof completes the retained job");
    r.Check(std::equal(imageSha256.begin(), imageSha256.end(), board.images[0].begin() + 368U),
            "exact completed healthy ticket carries the fully verified image digest");
    r.Check(firmware::PollEsp32OtaBootHealthForContract(ticket, result, imageSha256) == Status::Stale &&
                firmware::ReadEsp32OtaRunningStateForContract(state),
            "completion is delivered once and frees the existing worker for ordinary queries");

    // A failed image result must release its receipt before a new attempt,
    // while an old ticket can never consume that retry's response.
    board.images[0U][70U] ^= 1U;
    deferWorker = true;
    firmware::OtaBootHealthTicket failed = 0U;
    r.Check(firmware::BeginEsp32OtaBootHealthForContract(modules::OtaPostBootHealth::Passed, failed) ==
                Status::Pending, "failed-integrity attempt starts on the existing worker");
    DispatchQueuedWorker();
    r.Check(firmware::PollEsp32OtaBootHealthForContract(failed, result, imageSha256) == Status::Complete &&
                result == Result::ImageEvidenceUnavailable && board.confirms == 0U && board.rollbacks == 0U,
            "integrity failure reports failure without acceptance or selector mutation");
    r.Check(std::all_of(imageSha256.begin(), imageSha256.end(), [](uint8_t value) { return value == 0U; }),
            "failed integrity cannot lend an earlier healthy digest");
    board.images[0U][70U] ^= 1U;
    firmware::OtaBootHealthTicket retry = 0U;
    r.Check(firmware::BeginEsp32OtaBootHealthForContract(modules::OtaPostBootHealth::Passed, retry) ==
                Status::Pending && retry != failed &&
                firmware::PollEsp32OtaBootHealthForContract(failed, result, imageSha256) == Status::Stale,
            "retry owns a fresh ticket and rejects the previous lifetime");
    DispatchQueuedWorker();
    r.Check(firmware::PollEsp32OtaBootHealthForContract(retry, result, imageSha256) == Status::Complete &&
                result == Result::Confirmed, "retry still requires the complete production verification");

    // A synchronous caller that timed out has relinquished its result. Keep
    // the prior bounded recovery without allowing it to reap live health jobs.
    const auto abandoned = Health(modules::OtaPostBootHealth::NotReported);
    r.Check(abandoned == Result::SnapshotUnavailable && queuedWorker,
            "controlled synchronous timeout leaves its old worker request outstanding");
    refused = 0U;
    r.Check(firmware::BeginEsp32OtaBootHealthForContract(modules::OtaPostBootHealth::Passed, refused) ==
                Status::Busy, "health cannot replace a still-running abandoned synchronous job");
    DispatchQueuedWorker();
    deferWorker = false;
    r.Check(firmware::ReadEsp32OtaRunningStateForContract(state),
            "ordinary query may reap a completed abandoned synchronous result");
}

struct RuntimeProgress {
    Results& results;
    Board& storage;
    HealthRuntimeFixture& runtime;
    runtime::VirtualLitchSession& board;
    unsigned rendered = 0U;
    unsigned polls = 0U;
};
RuntimeProgress* currentProgress = nullptr;
void ContinueRuntimeAfterReadLease() {
    auto& progress = *currentProgress;
    progress.storage.ticks += 20U;
    ++progress.polls;
    progress.results.Check(!firmware_internal::PollRuntimeBootHealthForWorkerContract(progress.runtime),
                          "main health poll remains pending while real worker reads the image");
    const bool rendered = progress.storage.arbiter->TryBeginRender();
    progress.results.Check(rendered, "main renderer can acquire between production partition-read leases");
    if (rendered) {
        progress.storage.arbiter->EndRender();
        ++progress.rendered;
    }
    progress.results.Check(progress.board.Advance(20U) == VIRTUAL_LITCH_OK,
                          "production board input, timers and compositor advance while verification runs");
}

void RuntimeHealthJourney(Results& r, Board& storage,
                          hardware::RmtFlashArbiter& arbiter,
                          modules::OtaPlatformAdapter& adapter) {
    Reset(storage, adapter);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    constexpr uint64_t OCCUPANCY = UINT64_C(0xFFFF00000000FFFF);
    r.Check(board.ObserveHallMask(OCCUPANCY) == VIRTUAL_LITCH_OK &&
                board.Advance(200U) == VIRTUAL_LITCH_OK, "health journey starts from the normal physical opening");
    HealthRuntimeFixture runtime{board.SettingsForPersistenceTest(), {}, arbiter};
    r.Check(!firmware_internal::PollRuntimeBootHealthForWorkerContract(runtime),
            "runtime starts before confirmed completion of startup presentation");
    storage.ticks = 250U;
    runtime.ledAnimation.startupCompletedAtMs = 250U;
    deferWorker = true;
    blockingCompletionTakes = 0U;
    const unsigned notifications = workerNotifications;
    r.Check(!firmware_internal::PollRuntimeBootHealthForWorkerContract(runtime) &&
                runtime.runtimeBootHealth.healthTicket != 0U && runtime.ble.accepted == 0U,
            "main resolves one prepared transaction after confirmed startup and transport health");
    const auto ticket = runtime.runtimeBootHealth.healthTicket;
    const uint32_t frame = board.PhysicalFrame().frame_sequence;
    r.Check(board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, true) ==
                VIRTUAL_LITCH_OK, "reported held X enters the real physical/debounce path");
    // Quick Setup first shows its existing one-second icon, then its value.
    // Keep verification held through both the X threshold and that transition.
    for (unsigned tick = 0U; tick < 125U; ++tick) {
        storage.ticks += 20U;
        r.Check(!firmware_internal::PollRuntimeBootHealthForWorkerContract(runtime) &&
                    runtime.runtimeBootHealth.healthTicket == ticket,
                "main health polling does not block, resubmit, or release pending ownership");
        r.Check(board.Advance(20U) == VIRTUAL_LITCH_OK, "ordinary production input/render cadence continues");
    }
    r.Check(board.QuickSettingsActive() && !board.DisplayText().empty() &&
                board.PhysicalFrame().frame_sequence > frame && runtime.ble.accepted == 0U,
            "held X opens visible Quick Setup while the health job remains pending");
    std::printf("[OTA_HEALTH_JOURNEY] stage=pending input=X-held occupancy=%016llx mode=Quick-Setup "
                "text=%.*s frames=%u ticket=%lu accepted=0 next=release-X expiry=ordinary-menu-idle\n",
                static_cast<unsigned long long>(board.State().physical_occupied_mask),
                static_cast<int>(board.DisplayText().size()), board.DisplayText().data(),
                board.PhysicalFrame().frame_sequence - frame, static_cast<unsigned long>(ticket));
    RuntimeProgress progress{r, storage, runtime, board};
    currentProgress = &progress;
    afterWorkerRelease = &ContinueRuntimeAfterReadLease;
    deferWorker = false;
    DispatchQueuedWorker();
    afterWorkerRelease = nullptr;
    currentProgress = nullptr;
    r.Check(progress.rendered > 0U && progress.polls == progress.rendered &&
                blockingCompletionTakes == 0U && workerNotifications == notifications + 2U,
            "production worker's short read leases allow main polls and rendered frames without extra jobs");
    r.Check(firmware_internal::PollRuntimeBootHealthForWorkerContract(runtime) &&
                runtime.ble.accepted == 1U && !runtime.otaBootAwaitingHealth &&
                runtime.runtimeBootHealth.healthTicket == 0U && board.QuickSettingsActive(),
            "only terminal full verification publishes acceptance and preserves the user's open menu");
    r.Check(std::equal(runtime.ble.digest.begin(), runtime.ble.digest.end(), storage.images[0].begin() + 368U),
            "normal acceptance publishes the exact completed image proof for digest reuse");
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(runtime);
    r.Check(runtime.ble.accepted == 1U && workerNotifications == notifications + 2U &&
                board.State().physical_occupied_mask == OCCUPANCY && board.UndoHistoryMoveCount() == 0U,
            "acceptance is published once without changing the game or physical next action");
    std::printf("[OTA_HEALTH_JOURNEY] stage=verified input=X-held mode=Quick-Setup accepted=1 "
                "read-lease-render-handoffs=%u next=release-X physical-timing=PENDING\n", progress.rendered);

    // Native completion cannot be cancelled or reused across a change from
    // normal runtime to recovery; its old Passed result is retired first.
    Reset(storage, adapter);
    HealthRuntimeFixture retired{board.SettingsForPersistenceTest(), {}, arbiter};
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(retired);
    storage.ticks = 250U;
    retired.ledAnimation.startupCompletedAtMs = 250U;
    deferWorker = true;
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(retired);
    const auto retiredTicket = retired.runtimeBootHealth.healthTicket;
    retired.safeModeActive = true;
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(retired);
    r.Check(retired.runtimeBootHealth.healthTicket == retiredTicket && retiredTicket != 0U,
            "recovery entry retains the live native job rather than cancelling or replacing it");
    deferWorker = false;
    DispatchQueuedWorker();
    r.Check(!firmware_internal::PollRuntimeBootHealthForWorkerContract(retired) &&
                retired.ble.accepted == 0U && retired.runtimeBootHealth.healthTicket == 0U,
            "recovery consumes old Passed completion without publishing normal acceptance");

    Reset(storage, adapter);
    HealthRuntimeFixture lostRadio{board.SettingsForPersistenceTest(), {}, arbiter};
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(lostRadio);
    storage.ticks = 250U; deferWorker = true;
    lostRadio.ledAnimation.startupCompletedAtMs = 250U;
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(lostRadio);
    lostRadio.ble.available = false;
    deferWorker = false; DispatchQueuedWorker();
    r.Check(!firmware_internal::PollRuntimeBootHealthForWorkerContract(lostRadio) &&
                lostRadio.ble.accepted == 0U && !lostRadio.runtimeBootHealth.boundaryReached,
            "transport loss during hashing prevents normal acceptance at completion");
    lostRadio.ble.available = true; storage.ticks += 250U; deferWorker = true;
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(lostRadio);
    deferWorker = false;
    if (queuedWorker) DispatchQueuedWorker();
    r.Check(firmware_internal::PollRuntimeBootHealthForWorkerContract(lostRadio) &&
                lostRadio.ble.accepted == 1U, "restored transport follows the existing full verification before acceptance");

    Reset(storage, adapter);
    HealthRuntimeFixture failedRead{board.SettingsForPersistenceTest(), {}, arbiter};
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(failedRead);
    storage.ticks = 250U; deferWorker = true;
    failedRead.ledAnimation.startupCompletedAtMs = 250U;
    storage.images[0U][70U] ^= 1U;
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(failedRead);
    deferWorker = false; DispatchQueuedWorker();
    const unsigned failedNotifications = workerNotifications;
    r.Check(!firmware_internal::PollRuntimeBootHealthForWorkerContract(failedRead) &&
                failedRead.runtimeBootHealth.commitErrors == 1U && failedRead.ble.accepted == 0U,
            "real image-integrity failure reaches the existing bounded main retry policy");
    storage.ticks += 249U;
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(failedRead);
    r.Check(workerNotifications == failedNotifications, "failed health does not resubmit before the retry boundary");
    storage.images[0U][70U] ^= 1U; storage.ticks += 1U; deferWorker = true;
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(failedRead);
    deferWorker = false; DispatchQueuedWorker();
    r.Check(firmware_internal::PollRuntimeBootHealthForWorkerContract(failedRead) && failedRead.ble.accepted == 1U,
            "main retry accepts only after the repaired source passes complete verification");

    Reset(storage, adapter);
    storage.running = storage.bootTarget = 1U;
    storage.SetEntry(1U, 2U, ESP_OTA_IMG_PENDING_VERIFY);
    modules::OtaPostBootRunningState observed{};
    r.Check(firmware::ReadEsp32OtaRunningStateForContract(observed) &&
                observed == modules::OtaPostBootRunningState::PendingVerify,
            "failed-boot fixture begins with actual current pending SDK evidence");
    HealthRuntimeFixture recovery{board.SettingsForPersistenceTest(), {}, arbiter};
    recovery.safeModeActive = true;
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(recovery);
    storage.ticks += 250U; deferWorker = true;
    (void)firmware_internal::PollRuntimeBootHealthForWorkerContract(recovery);
    r.Check(recovery.runtimeBootHealth.healthTicket != 0U && storage.rollbacks == 0U,
            "failed boot waits nonblockingly for its guarded fallback verification");
    deferWorker = false; DispatchQueuedWorker();
    r.Check(!firmware_internal::PollRuntimeBootHealthForWorkerContract(recovery) &&
                recovery.runtimeBootHealth.commitFinished && recovery.ble.accepted == 0U &&
                recovery.bootGuard.healthy == 0U && storage.rollbacks == 1U && storage.running == 0U,
            "only the existing verified fallback owner requests SDK rollback; recovery never claims normal health");
}

int Run() {
    Results r("sdk_ota_platform"); r.Start(); Board b; hardware::RmtFlashArbiter arbiter; b.arbiter = &arbiter; g_board = &b;
    const fxe::ScopedMillisProviderOverride clock(&EvidenceClock);
    firmware::OtaBootHealthTicket unavailable = 99U;
    r.Check(firmware::BeginEsp32OtaBootHealthForContract(modules::OtaPostBootHealth::Passed, unavailable) ==
                firmware::OtaBootHealthRequestStatus::Unavailable && unavailable == 0U,
            "uninitialized native worker reports unavailable without creating a health ticket");
    auto* adapter = firmware::InitializeEsp32OtaPlatformAdapterForContract(arbiter);
    r.Check(adapter != nullptr, "actual SDK adapter initialized without filesystem");
    if (adapter == nullptr) return r.Finish();
    firmware::OtaBootEvidence boot;
    modules::OtaAdmissionEvidence admission;
    modules::OtaPostBootRunningState state;
    r.Check(firmware::CopyEsp32OtaBootEvidence(boot) && !boot.available && boot.sample == 0U,
            "initial RAM cache reports no capture instead of invented readiness");
    b.ticks = 111U;
    r.Check(firmware::ReadEsp32OtaRunningStateForContract(state) && state == modules::OtaPostBootRunningState::Valid,
            "actual worker captures coherent initial VALID state");
    r.Check(firmware::CopyEsp32OtaBootEvidence(boot) && boot.available &&
                boot.snapshot.running.address == b.slots[0].address &&
                boot.snapshot.selected.address == b.slots[0].address &&
                boot.snapshot.selectedSequence == 1U && boot.capturedAtMs == 111U &&
                !b.publicationBeforeRelease,
            "cache dates actual capture and publishes only after flash release");
    const auto observed = boot;
    const unsigned reads = b.imageReads;
    const auto flashAccesses = arbiter.GetFlashDiagnostics().accesses;
    b.ticks = 222U;
    r.Check(adapter->ReadBootEvidence(boot) && boot.sample == observed.sample &&
                boot.capturedAtMs == observed.capturedAtMs && b.imageReads == reads &&
                arbiter.GetFlashDiagnostics().accesses == flashAccesses,
            "actual platform adapter's cached copy preserves age and performs no new flash/hash");
    modules::OtaPlatformTransaction unsupported({});
    r.Check(!unsupported.ReadAdmissionEvidence(admission) && !unsupported.ReadBootEvidence(boot),
            "unsupported adapter never invents admission or SDK cache evidence");
    for (unsigned attempt = 0; attempt < 20U; ++attempt) {
        const unsigned prior = b.running; const auto fallback = b.images[prior];
        const uint32_t sourceSequence = b.entries[b.Active()].ota_seq;
        const auto image = MakeImage(0x40U);
        r.Check(adapter->Begin(image.size()), "same-image Begin reaches actual SDK admission");
        r.Check(adapter->ReadAdmissionEvidence(admission) && admission.attempt != 0U &&
                    admission.phase == modules::OtaAdmissionPhase::Receiving &&
                    admission.source.address == b.slots[prior].address &&
                    admission.target.address == b.slots[1U - prior].address &&
                    admission.sourceSequence == sourceSequence && admission.expectedSequence == sourceSequence + 1U,
                "Begin evidence identifies actual inactive target and exact next SDK sequence");
        const uint32_t admittedAttempt = admission.attempt;
        char proof[224U];
        char expectedAdmission[96U];
        std::snprintf(expectedAdmission, sizeof(expectedAdmission),
                      "\"a\":[%lu,1,%u,%u,%lu,%lu]}",
                      static_cast<unsigned long>(admission.attempt), prior, 1U - prior,
                      static_cast<unsigned long>(sourceSequence),
                      static_cast<unsigned long>(sourceSequence + 1U));
        const auto proofFlashAccesses = arbiter.GetFlashDiagnostics().accesses;
        const unsigned proofImageReads = b.imageReads;
        r.Check(modules::FormatOtaAttemptProof(adapter,
                    {1U, UINT64_C(0x8899aabbccddeeff), true}, proof, sizeof(proof)) &&
                    std::strstr(proof, expectedAdmission) != nullptr &&
                    b.imageReads == proofImageReads &&
                    arbiter.GetFlashDiagnostics().accesses == proofFlashAccesses,
                "typed proof copies actual SDK source/target admission without flash/hash work");
        r.Check(adapter->Write(image.data(), image.size()) && adapter->End(), "repeated same image activates inactive slot");
        r.Check(adapter->ReadAdmissionEvidence(admission) && admission.attempt == admittedAttempt &&
                    admission.phase == modules::OtaAdmissionPhase::Activated &&
                    admission.expectedSequence == b.entries[b.Active()].ota_seq,
                "End retains exact actual activation proof after transaction resets");
        r.Check(firmware::CopyEsp32OtaBootEvidence(boot) && boot.available &&
                    boot.snapshot.running.address == b.slots[prior].address &&
                    boot.snapshot.selected.address == b.slots[1U - prior].address &&
                    boot.snapshot.selectedSequence == admission.expectedSequence &&
                    boot.runningState == modules::OtaPostBootRunningState::Unknown,
                "post-End NEW selection is observed without claiming running acceptance");
        r.Check(b.images[prior] == fallback && b.bootTarget != prior, "activation preserves complete current fallback");
        b.Boot(); const unsigned before = b.imageReads;
        r.Check(Health(modules::OtaPostBootHealth::NotReported) == modules::OtaPostBootExecutionResult::AwaitingHealth && b.imageReads == before, "cheap preflight never hashes");
        r.Check(firmware::CopyEsp32OtaBootEvidence(boot) && boot.available &&
                    boot.runningState == modules::OtaPostBootRunningState::PendingVerify,
                "actual worker cache distinguishes current PENDING from accepted VALID");
        r.Check(!adapter->Begin(400U), "pending image cannot erase verified rollback slot");
        r.Check(adapter->ReadAdmissionEvidence(admission) && admission.attempt != admittedAttempt &&
                    admission.phase == modules::OtaAdmissionPhase::Failed && admission.target.address == 0U,
                "new rejected Begin cannot reuse the preceding admission proof");
        r.Check(Health(modules::OtaPostBootHealth::Passed) == modules::OtaPostBootExecutionResult::Confirmed, "full software integrity and raw VALID readback accept actual SDK attempt");
        r.Check(firmware::CopyEsp32OtaBootEvidence(boot) && boot.available &&
                    boot.runningState == modules::OtaPostBootRunningState::Valid &&
                    boot.snapshot.selectedSequence == sourceSequence + 1U,
                "health publishes full guarded VALID readback through actual worker");
        r.Check(b.entries[b.Active()].ota_state == ESP_OTA_IMG_VALID, "accepted selector is durable VALID");
    }
    r.Check(b.confirms == 20U && b.leaseCorrect && !b.publicationBeforeRelease && !b.admissionBeforeSdkSuccess,
            "twenty modeled confirmations retain flash exclusion and publish only after SDK/lease success");
    Reset(b, *adapter); b.images[0][70] ^= 1U;
    r.Check(!adapter->Begin(400U) && b.beginCalls == 0U, "corrupt running fallback rejected before inactive write");
    r.Check(firmware::CopyEsp32OtaBootEvidence(boot) && !boot.available,
            "failed latest source capture invalidates prior cached readiness");
    Reset(b, *adapter); b.configuredMismatch = true;
    r.Check(!adapter->Begin(400U) && b.beginCalls == 0U, "configured SDK/raw selection mismatch rejects before write");
    Reset(b, *adapter); b.changeDuringImageRead = true;
    r.Check(!adapter->Begin(400U) && b.beginCalls == 0U, "source selector change during full hash rejects before write");
    Reset(b, *adapter);
    r.Check(!Transfer(*adapter, MakeImage(0x40U, 1U)) && b.activationCalls == 0U, "record-dependent NEW image rejected before SDK activation");
    Reset(b, *adapter);
    r.Check(Transfer(*adapter, MakeImage(0x40U)), "failed-health fixture activates"); b.Boot();
    r.Check(Health(modules::OtaPostBootHealth::Failed) == modules::OtaPostBootExecutionResult::RollbackRequested && b.running == 0U && b.rollbacks == 1U, "verified distinct fallback permits SDK rollback request");
    r.Check(firmware::CopyEsp32OtaBootEvidence(boot) && !boot.available,
            "returning rollback request cannot claim a post-reset snapshot");
    r.Check(firmware::ReadEsp32OtaRunningStateForContract(state) &&
                firmware::CopyEsp32OtaBootEvidence(boot) && boot.available &&
                boot.snapshot.running.address == b.slots[0].address &&
                boot.snapshot.selected.address == b.slots[0].address &&
                boot.runningState == modules::OtaPostBootRunningState::Valid,
            "fresh fallback capture identifies current valid source separately from failed attempt");
    Reset(b, *adapter);
    b.SetEntry(0U, 19U, ESP_OTA_IMG_VALID);
    r.Check(Transfer(*adapter, b.images[0]), "intact same-image candidate reaches NEW activation");
    r.Check(adapter->ReadAdmissionEvidence(admission) && admission.phase == modules::OtaAdmissionPhase::Activated,
            "same-image attempt has actual pre-reboot admission evidence");
    b.SetEntry(1U, admission.expectedSequence, ESP_OTA_IMG_ABORTED);
    b.running = 0U; b.bootTarget = 0U; // Controlled selecting-boot boundary.
    r.Check(firmware::ReadEsp32OtaRunningStateForContract(state) &&
                firmware::CopyEsp32OtaBootEvidence(boot) && boot.available &&
                boot.runningState == modules::OtaPostBootRunningState::Valid &&
                boot.snapshot.selectors[1][0] == admission.expectedSequence &&
                boot.snapshot.selectors[1][24] == ESP_OTA_IMG_ABORTED && b.images[0] == b.images[1],
            "healthy VALID same-image fallback retains raw failed ABORTED attempt sequence");
    r.Check(boot.snapshot.running.address != admission.target.address &&
                boot.snapshot.selectedSequence != admission.expectedSequence,
            "old activated admission cannot identify the fallback as its selected attempt");
    // The controlled boot boundary above keeps this test's adapter object alive.
    // A physical reboot initializes the RAM admission owner to None.
    firmware::ota_evidence_internal::RecordAdmission({});
    char fallbackProof[224U];
    r.Check(modules::FormatOtaAttemptProof(adapter,
                {24U, UINT64_C(0x0123456789abcdef), true}, fallbackProof, sizeof(fallbackProof)) &&
                std::strstr(fallbackProof, ",19,2,19,2,1,20,4,1]") != nullptr &&
                std::strstr(fallbackProof, "\"a\":[0,0,255,255,0,0]") != nullptr,
            "same-image selecting boot emits actual source19 VALID plus exact target20 ABORTED proof");
    Reset(b, *adapter);
    modules::OtaPostBootSnapshot first, second;
    r.Check(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()) &&
                firmware::ReadEsp32OtaBootSnapshotWithFlashLeaseForContract(first),
            "first ordering fixture captures actual SDK state");
    b.ticks = 1000U;
    const auto firstStamp = firmware::ota_evidence_internal::CaptureStamp();
    arbiter.EndFlash(); AfterRelease();
    b.SetEntry(0U, 3U, ESP_OTA_IMG_VALID);
    r.Check(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()) &&
                firmware::ReadEsp32OtaBootSnapshotWithFlashLeaseForContract(second),
            "second ordering fixture captures later SDK selection");
    b.ticks = 1100U;
    const auto secondStamp = firmware::ota_evidence_internal::CaptureStamp();
    arbiter.EndFlash(); AfterRelease();
    firmware::ota_evidence_internal::RecordSnapshot(&second, secondStamp);
    firmware::ota_evidence_internal::RecordSnapshot(&first, firstStamp);
    r.Check(firmware::CopyEsp32OtaBootEvidence(boot) && boot.sample == secondStamp.sample &&
                boot.capturedAtMs == 1100U && boot.snapshot.selectedSequence == 3U,
            "delayed older publisher cannot overwrite or falsely redate a newer SDK capture");
    Reset(b, *adapter);
    r.Check(Transfer(*adapter, MakeImage(0x40U)), "corrupt-fallback fixture activates"); b.Boot(); b.images[0][70] ^= 1U;
    r.Check(Health(modules::OtaPostBootHealth::Failed) == modules::OtaPostBootExecutionResult::ImageEvidenceUnavailable && b.rollbacks == 0U, "corrupt fallback cannot authorize invalidation");
    Reset(b, *adapter);
    r.Check(Transfer(*adapter, MakeImage(0x40U)), "readback-fault fixture activates"); b.Boot(); b.confirmNoWrite = true;
    r.Check(Health(modules::OtaPostBootHealth::Passed) == modules::OtaPostBootExecutionResult::ConfirmReadbackFailed, "SDK success without raw VALID is not accepted");
    r.Check(firmware::CopyEsp32OtaBootEvidence(boot) && boot.available &&
                boot.runningState == modules::OtaPostBootRunningState::PendingVerify,
            "failed confirmation reports actual PENDING readback, never accepted");
    Reset(b, *adapter); b.corruptRead = true;
    r.Check(!firmware::ReadEsp32OtaRunningStateForContract(state) &&
                firmware::CopyEsp32OtaBootEvidence(boot) && !boot.available,
            "latest raw read failure clears availability rather than retaining old VALID");
    Reset(b, *adapter); b.SetEntry(0U, 1U, 0x12345678U);
    r.Check(!firmware::ReadEsp32OtaRunningStateForContract(state) &&
                firmware::CopyEsp32OtaBootEvidence(boot) && boot.available &&
                boot.runningState == modules::OtaPostBootRunningState::Unknown,
            "coherent SDK-selectable unknown state stays raw-only and unaccepted");
    Reset(b, *adapter); b.beginFails = true;
    r.Check(!adapter->Begin(400U) && adapter->ReadAdmissionEvidence(admission) &&
                admission.phase == modules::OtaAdmissionPhase::Failed && admission.target.address == 0U,
            "failed actual SDK Begin never publishes Receiving");
    Reset(b, *adapter);
    r.Check(adapter->Begin(400U), "abort fixture begins"); adapter->Abort();
    r.Check(adapter->ReadAdmissionEvidence(admission) && admission.phase == modules::OtaAdmissionPhase::Aborted,
            "Abort retires Receiving proof");
    Reset(b, *adapter);
    const auto uncertainImage = MakeImage(0x40U);
    r.Check(adapter->Begin(uncertainImage.size()) &&
                adapter->Write(uncertainImage.data(), uncertainImage.size()) &&
                adapter->ReadBootEvidence(boot) && boot.available &&
                boot.runningState == modules::OtaPostBootRunningState::Valid &&
                boot.snapshot.selected.address == b.slots[0U].address,
            "uncertain End fixture first caches actual selected source VALID");
    b.endFails = true;
    r.Check(!adapter->End() && adapter->ReadAdmissionEvidence(admission) &&
                admission.phase == modules::OtaAdmissionPhase::Failed &&
                adapter->ReadBootEvidence(boot) && !boot.available &&
                modules::FormatOtaAttemptProof(adapter,
                    {1U, UINT64_C(0x8899aabbccddeeff), true}, fallbackProof, sizeof(fallbackProof)) &&
                std::strcmp(fallbackProof, "{\"op\":1,\"q\":1,\"e\":\"unavailable\"}") == 0,
            "uncertain failed SDK End invalidates old source VALID and emits only unavailable");
    Reset(b, *adapter);
    b.corruptReadAfterActivation = true;
    r.Check(!Transfer(*adapter, uncertainImage) && b.entries[1U].ota_state == ESP_OTA_IMG_NEW &&
                adapter->ReadAdmissionEvidence(admission) &&
                admission.phase == modules::OtaAdmissionPhase::Failed &&
                adapter->ReadBootEvidence(boot) && !boot.available &&
                modules::FormatOtaAttemptProof(adapter,
                    {1U, UINT64_C(0x8899aabbccddeeff), true}, fallbackProof, sizeof(fallbackProof)) &&
                std::strcmp(fallbackProof, "{\"op\":1,\"q\":1,\"e\":\"unavailable\"}") == 0,
            "SDK NEW publication followed by failed readback cannot expose stale source VALID proof");
    Reset(b, *adapter); b.activationSequenceDelta = 2U;
    r.Check(!Transfer(*adapter, MakeImage(0x40U)) && adapter->ReadAdmissionEvidence(admission) &&
                admission.phase == modules::OtaAdmissionPhase::Failed &&
                adapter->ReadBootEvidence(boot) && boot.available &&
                boot.snapshot.selectedSequence != admission.expectedSequence &&
                boot.snapshot.running.address == b.slots[0U].address &&
                boot.snapshot.selected.address == b.slots[1U].address &&
                boot.runningState == modules::OtaPostBootRunningState::Unknown &&
                modules::FormatOtaAttemptProof(adapter,
                    {1U, UINT64_C(0x8899aabbccddeeff), true}, fallbackProof, sizeof(fallbackProof)) &&
                std::strcmp(fallbackProof, "{\"op\":1,\"q\":1,\"e\":\"unavailable\"}") == 0,
            "fresh higher NEW readback plus Failed never appears as accepted selected source");
    Reset(b, *adapter);
    const auto changedImage = MakeImage(0x40U);
    r.Check(adapter->Begin(changedImage.size()) && adapter->Write(changedImage.data(), changedImage.size()),
            "changed-selection fixture receives candidate");
    b.SetEntry(0U, 3U, ESP_OTA_IMG_VALID);
    r.Check(!adapter->End() && b.activationCalls == 0U && adapter->ReadAdmissionEvidence(admission) &&
                admission.phase == modules::OtaAdmissionPhase::Failed &&
                admission.sourceSequence == 1U && admission.expectedSequence == 2U,
            "selection change before End rejects activation and retains admitted identity");
    Reset(b, *adapter); b.SetEntry(0, 17U, ESP_OTA_IMG_VALID); b.SetEntry(1, 19U, ESP_OTA_IMG_PENDING_VERIFY);
    r.Check(!adapter->Begin(400U), "old first same-slot VALID cannot hide active PENDING");
    r.Check(Health(modules::OtaPostBootHealth::Failed) == modules::OtaPostBootExecutionResult::NoValidFallback && b.rollbacks == 0U, "same-slot VALID record is not a distinct rollback image");
    Reset(b, *adapter); b.appCount = 3U;
    r.Check(!adapter->Begin(400U) && b.beginCalls == 0U, "extra OTA slot geometry rejected before write");
    Reset(b, *adapter); b.SetEntry(0, UINT32_MAX - 2U, ESP_OTA_IMG_VALID);
    r.Check(!adapter->Begin(400U) && b.beginCalls == 0U, "near-wrap attempt rejected before write");
    Reset(b, *adapter);
    SynchronousFlashDuringSettings(r, b, arbiter, [&] {
        modules::OtaPostBootRunningState running;
        return firmware::ReadEsp32OtaRunningStateForContract(running) &&
               running == modules::OtaPostBootRunningState::Valid;
    });
    SynchronousFlashDuringSettings(r, b, arbiter, [&] { return adapter->Begin(b.images[1].size()); });
    SynchronousFlashDuringSettings(r, b, arbiter, [&] { return adapter->Write(b.images[1].data(), 31U); });
    r.Check(b.leaseCorrect, "all real SDK observations and mutations retain flash lease");
    RetainedHealthOwnership(r, b, *adapter);
    RuntimeHealthJourney(r, b, arbiter, *adapter);
    adapter->Abort(); g_board = nullptr;
    std::printf("[SDK_OTA] production_snapshot=1 partition_reader=1 adapter=1 worker=1 executor=1 modeled_same_image=20 physical_credit=0 boot_evidence_bytes=%zu admission_bytes=%zu\n",
                sizeof(firmware::OtaBootEvidence), sizeof(modules::OtaAdmissionEvidence));
    return r.Finish();
}
} // namespace ota_platform_contract

namespace { void TestSdkOnlyCustomerOtaThroughProductionAdapter() { TEST_ASSERT_EQUAL_INT(0, ota_platform_contract::Run()); } }
int main() { UNITY_BEGIN(); RUN_TEST(TestSdkOnlyCustomerOtaThroughProductionAdapter); return UNITY_END(); }
