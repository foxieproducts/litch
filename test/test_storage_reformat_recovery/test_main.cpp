#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/console_transport_memory.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "freertos/task.h"
#include "runtime/progress_watchdog.hpp"

namespace recovery_fixture {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::printf("[FAIL] %s\n", message); }
}
std::mutex mutex;
std::condition_variable changed;
std::thread task;
bool stop = false;
bool notified = false;
bool paused = true;
struct Stop {};
void Resume() {
    std::lock_guard<std::mutex> lock(mutex);
    paused = false;
    changed.notify_all();
}
void Pause() { std::lock_guard<std::mutex> lock(mutex); paused = true; }
template<typename Predicate> void Wait(Predicate ready) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    Check(ready(), "bounded worker completion");
}
void Finish() {
    { std::lock_guard<std::mutex> lock(mutex); stop = true; changed.notify_all(); }
    task.join();
}
}
TaskHandle_t RecoveryTaskCreate(TaskFunction_t task, const char*, uint32_t,
                              void* context, uint32_t, StackType_t*, StaticTask_t*, int) {
    recovery_fixture::task = std::thread([=] {
        try { task(context); } catch (const recovery_fixture::Stop&) {}
    });
    return reinterpret_cast<TaskHandle_t>(1U);
}
void RecoveryDelay(uint32_t) { std::this_thread::yield(); }
void RecoveryDelete(TaskHandle_t) {}
uint32_t RecoveryNotifyTake(bool, uint32_t) {
    std::unique_lock<std::mutex> lock(recovery_fixture::mutex);
    recovery_fixture::changed.wait(lock, [] {
        return recovery_fixture::stop ||
            (recovery_fixture::notified && !recovery_fixture::paused);
    });
    if (recovery_fixture::stop) throw recovery_fixture::Stop{};
    recovery_fixture::notified = false;
    return 1U;
}
void RecoveryNotifyGive(TaskHandle_t) {
    std::lock_guard<std::mutex> lock(recovery_fixture::mutex);
    recovery_fixture::notified = true;
    recovery_fixture::changed.notify_all();
}

using UBaseType_t = uint32_t;
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define ARDUINO 1
#define SettingsPersistenceWorker RecoverySettingsPersistenceWorker
#define xTaskCreateStaticPinnedToCore RecoveryTaskCreate
#define vTaskDelay RecoveryDelay
#define vTaskDelete RecoveryDelete
#define ulTaskNotifyTake RecoveryNotifyTake
#define xTaskNotifyGive RecoveryNotifyGive
#include "../../src/settings_persistence_worker.cpp"
#include "../../src/settings_persistence_worker_recovery.cpp"
#undef xTaskNotifyGive
#undef ulTaskNotifyTake
#undef vTaskDelete
#undef vTaskDelay
#undef xTaskCreateStaticPinnedToCore
#undef SettingsPersistenceWorker
#undef ARDUINO

#ifdef LITCH_STORAGE_REFORMAT_FILESYSTEM_TEST
#define ARDUINO 1
#include "../../lib/foxie_embedded/src/settings_store_reformat.cpp"
#undef ARDUINO
#endif

namespace {
using recovery_fixture::Check;
using Worker = firmware::RecoverySettingsPersistenceWorker;

void ConsoleConfirmationGuard() {
    fxe::SettingsStore settings;
    fxe::MemoryConsoleTransport transport;
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);
    fxe::Console console(transport);
    console.SetSettingsStore(&settings);
    fxe::test_support::RunCommand(console, transport, "settings format");
    Check(output.Str().find("To confirm: settings format ERASE_BOARD_STORAGE") !=
              std::string::npos, "bare format discloses exact explicit confirmation");
    Check(output.Str().find("storage_format=unavailable reboot_required=0") !=
              std::string::npos, "no recovery owner is truthful unavailable");
    using Action = fxe::Console::StorageReformatAction;
    using Status = fxe::Console::StorageReformatStatus;
    uint32_t confirmations = 0U;
    uint32_t observations = 0U;
    Status state = Status::Idle;
    console.SetStorageReformatCallback([&](Action action) {
        if (action == Action::Confirm) ++confirmations;
        else ++observations;
        return state;
    });
    for (const char* command : {"settings format", "settings format status"}) {
        fxe::test_support::RunCommand(console, transport, command);
    }
    Check(confirmations == 0U && observations == 2U,
          "bare format and status cannot admit erasure");
    for (const char* command : {"settings format yes", "settings format erase_board_storage",
        "settings format ERASE_BOARD_STORAGE extra", "settings format ERASE_BOARD_STORAGE;"}) {
        fxe::test_support::RunCommand(console, transport, command);
    }
    Check(confirmations == 0U && observations == 2U,
          "invalid token and extra arguments cannot invoke recovery owner");
    state = Status::Pending;
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "settings format ERASE_BOARD_STORAGE");
    Check(confirmations == 1U &&
              output.Str().find("storage_format=pending reboot_required=0") != std::string::npos,
          "exact token admits asynchronous work without claiming completion");
    for (const Status status : {Status::Running, Status::Busy, Status::Failed, Status::Succeeded}) {
        state = status;
        output.Clear();
        fxe::test_support::RunCommand(console, transport, "settings format status");
        const bool requiresReboot = output.Str().find("reboot_required=1") != std::string::npos;
        Check(requiresReboot == (status == Status::Succeeded),
              "only verified recovery completion requires restart");
    }
}

void DisabledWorkerAndRetainedRecovery(const std::filesystem::path& root) {
    const std::string path = (root / "worker.json").string();
    fxe::SettingsStoreConfig config;
    config.path = path.c_str();
    config.allowSynchronousDeferredSaves = false;
    fxe::SettingsStore source(config);
    source.Doc()["CURRENT_GAME"] = "keep RAM game";
    source.Doc()["BOARD_ID"] = "test-board";
    { std::ofstream file(path); file << "rejected original bytes"; }
    Worker worker(config);
    StaticJsonDocument<256> safe;
    safe["BOARD_ID"] = "test-board";
    safe["LED_BROWNOUT_LIMIT_LATCH"] = true;
    Check(!worker.RequestStorageReformat(safe), "unstarted worker cannot claim format admission");
    uint32_t prepared = 0U;
    Check(worker.SetSnapshotTransactionCallbacks(
        [](void* context, JsonDocument&) { ++*static_cast<uint32_t*>(context); return true; },
        [](void*, Worker::SnapshotTransactionOutcome, const JsonDocument&) {}, &prepared),
          "real transaction owner installed");
    Check(worker.Begin(source), "worker starts even for recovery capability");
    Worker::SaveReceipt receipt = 0U;
    Check(worker.EnqueueTracked(source.Doc(), receipt), "pre-disable snapshot queued");
    Check(!worker.RequestStorageReformat(safe), "queued save retains storage ownership");
    source.ReportAsyncSaveFailure(fxe::SettingsStoreError::WriteFailed);
    source.DisablePersistenceForSession();
    recovery_fixture::Resume();
    recovery_fixture::Wait([&] { return worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Failed; });
    Check(prepared == 0U && worker.GetCompletedSaveCount() == 0U,
          "disabled queued snapshot performs no game transaction or durable write");
    Check(worker.ReleaseSaveReceipt(receipt), "cancelled exact receipt releases");
    Check(!worker.Enqueue(source.Doc()), "disabled source rejects ordinary admission");
    std::ifstream retained(path);
    std::string bytes;
    std::getline(retained, bytes);
    Check(bytes == "rejected original bytes", "queued cancellation preserves original disk bytes");
    recovery_fixture::Pause();
    Check(worker.RequestStorageReformat(safe), "disabled source admits explicit safe recovery");
    Check(worker.GetStorageReformatState() == Worker::StorageReformatState::Pending &&
              !worker.RequestStorageReformat(safe), "pending recovery is retained and never duplicated");
    safe["BOARD_ID"] = "changed caller document";
    recovery_fixture::Resume();
    recovery_fixture::Wait([&] {
        const auto state = worker.GetStorageReformatState();
        return state == Worker::StorageReformatState::Failed ||
               state == Worker::StorageReformatState::Succeeded;
    });
#ifdef LITCH_STORAGE_REFORMAT_FILESYSTEM_TEST
    Check(worker.GetStorageReformatState() == Worker::StorageReformatState::Succeeded,
          "real confirmed format, remount, write and promoted readback succeed");
    fxe::SettingsStore restored(config);
    Check(restored.Load() && restored.Doc()["BOARD_ID"].as<std::string>() == "test-board" &&
              restored.Doc()["LED_BROWNOUT_LIMIT_LATCH"].as<bool>() &&
              !restored.Doc().containsKey("CURRENT_GAME"),
          "reboot restores frozen identity and protection without RAM game");
#else
    Check(worker.GetStorageReformatState() == Worker::StorageReformatState::Failed &&
              worker.GetLastError() == fxe::SettingsStoreError::Unsupported,
          "native unavailable formatter cannot claim recovered from mounting alone");
#endif
    Check(source.IsDisabled() && source.Doc()["CURRENT_GAME"].as<std::string>() == "keep RAM game" &&
              source.Doc()["BOARD_ID"].as<std::string>() == "test-board" &&
              worker.GetCompletedSaveCount() == 0U,
          "recovery retains live RAM game, disabled state and truthful ordinary-save count");
    Check(!worker.RequestStorageReformat(safe) && worker.ReleaseStorageReformatResult(),
          "terminal result prevents repeat erasure until explicitly released");
    recovery_fixture::Finish();
}

#ifdef LITCH_STORAGE_REFORMAT_FILESYSTEM_TEST
void FilesystemFaultMatrix(const std::filesystem::path& root) {
    using Fault = reformat_sdk::Fault;
    for (const Fault fault : {Fault::None, Fault::Format, Fault::Mount, Fault::Write,
                             Fault::Read, Fault::Corrupt, Fault::Open}) {
        reformat_sdk::Reset(root, fault);
        const std::string path = (root / "settings.json").string();
        fxe::SettingsStoreConfig config;
        config.path = path.c_str();
        config.verifyWrites = false;
        fxe::SettingsStore writer(config);
        writer.Doc()["BOARD_ID"] = "preserved";
        writer.Doc()["LED_BROWNOUT_LIMIT_LATCH"] = true;
        { std::ofstream file(root / "artwork.bin"); file << "erasable board artwork"; }
        { std::ofstream file(root / "currentline.bin"); file << "erasable board game"; }
        int leases = 0;
        uint32_t accesses = 0U;
        writer.SetStorageAccessCallbacks([&] {
            Check(leases++ == 0, "filesystem lease does not overlap another flash owner");
            ++accesses;
        }, [&] { Check(--leases == 0, "filesystem lease is released"); });
        reformat_sdk::lease = &leases;
        const bool result = writer.ReformatFilesystemAndSaveNowBlocking();
        Check(result == (fault == Fault::None), "format fault matrix has truthful terminal result");
        Check(reformat_sdk::formatCalls == 1U && reformat_sdk::label == "spiffs" &&
                  reformat_sdk::unmounts == 1U, "confirmed operation force-formats exact board filesystem once");
        Check(leases == 0 && accesses != 0U && reformat_sdk::unleased == 0U,
              "format and every readback access stay inside flash ownership");
        Check(fault == Fault::Format || !std::filesystem::exists(root / "artwork.bin"),
              "successful format erases complete board filesystem");
        Check(fault == Fault::Format || !std::filesystem::exists(root / "currentline.bin"),
              "game log belongs to erased board filesystem");
        Check(writer.Doc()["BOARD_ID"].as<std::string>() == "preserved" &&
                  writer.Doc()["LED_BROWNOUT_LIMIT_LATCH"].as<bool>(),
              "formatter retains bounded safe snapshot on all failures");
        if (fault == Fault::Read || fault == Fault::Corrupt || fault == Fault::Open) {
            Check(writer.GetLastError() == fxe::SettingsStoreError::VerifyFailed,
                  "promoted file readback failure cannot report recovery success");
        }
    }
}
#endif
}

int main() {
    #ifdef LITCH_STORAGE_REFORMAT_FILESYSTEM_TEST
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "litch_fmt_sdk";
#else
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "litch_fmt_native";
#endif
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    ConsoleConfirmationGuard();
#ifdef LITCH_STORAGE_REFORMAT_FILESYSTEM_TEST
    reformat_sdk::Reset(root, reformat_sdk::Fault::None);
#endif
    DisabledWorkerAndRetainedRecovery(root);
#ifdef LITCH_STORAGE_REFORMAT_FILESYSTEM_TEST
    FilesystemFaultMatrix(root);
#endif
    std::filesystem::remove_all(root);
    std::printf("[STORAGE_REFORMAT_RECOVERY] failures=%d\n", recovery_fixture::failures);
    return recovery_fixture::failures == 0 ? 0 : 1;
}
