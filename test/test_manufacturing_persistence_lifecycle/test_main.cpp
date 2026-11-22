#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "firmware/firmware_app_internal.hpp" // Empty native header; fixture supplies only SDK/wiring below.
#include "firmware/hardware_test_mode.hpp"
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_internal.hpp"
#include "firmware/firmware_tasks_internal.hpp"
#include "firmware/led_runtime_admission.hpp"
#include "firmware/litchcraft_persistence_runtime.hpp"
#include "console.hpp"
#include "owner_pairing_console_test_transport.hpp"
#include "modules/hint_button_gesture_controller.hpp"
#include "modules/reset_hold_gesture_controller.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "hardware/button_manager.hpp"
#include "hardware/user_input_timing.hpp"
#include "firmware/settings_physical_access.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "modules/hardware_test_record.hpp"
#include "modules/board_identity_persistence.hpp"
#include "modules/factory_provisioning_policy.hpp"
#include "modules/factory_radio_role_gate.hpp"
#include "modules/current_game_log_persistence.hpp"
#include "modules/duck_game_persistence.hpp"
#include "modules/initial_fen.hpp"
#include "modules/settings_registry.hpp"
#include "freertos/task.h"
#include "current_line_memory_storage.hpp"
#include "logger.hpp"
#include "bounded_hint_engine.hpp"
#include "modules/game_console_commands.hpp"
#include "modules/sha256.hpp"
#include "modules/volatile_persistence_session.hpp"
#include "modules/board_storage_recovery.hpp"
#include "modules/support_runtime_stats.hpp"
#include "runtime/system_context.hpp"

namespace fixture {
std::mutex mutex;
std::condition_variable changed;
std::thread worker;
std::atomic<uint32_t> now{0U};
std::atomic<bool> blocked{false}, hold{true}, workerAtWait{false};
std::atomic<bool> holdRetry{false}, retryDelayReached{false};
std::atomic<bool> failNextTaskCreate{false};
bool notified = false, stopping = false;
struct Stop {};
uint32_t Millis() { return now.load(); }
template <typename Predicate> void Wait(Predicate predicate) {
    std::unique_lock<std::mutex> lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(3), predicate)) {
        std::puts("[FAIL] persistence fixture synchronization timeout"); std::abort();
    }
}
}

// The actual arbiter and SettingsStore filesystem transaction remain in use.
// This SDK scheduling fixture pauses a worker after its first storage access.
namespace hardware {
class PersistenceTestArbiter {
  public:
    static void ObserveStorage(void*, const bool begin) {
        if (!begin || !fixture::hold.load()) return;
        { std::lock_guard<std::mutex> lock(fixture::mutex); fixture::blocked.store(true); }
        fixture::changed.notify_all();
        fixture::Wait([] { return !fixture::hold.load(); });
    }
    uint32_t CompletedRenderGeneration() const { return inner.CompletedRenderGeneration(); }
    uint32_t NextRenderGeneration() const { return inner.NextRenderGeneration(); }
    void RequestFlash() { requestPublished = false; }
    void CancelFlashRequest() {
        if (requestPublished) inner.CancelFlashRequest();
        requestPublished = false;
    }
    bool TryBeginRequestedFlash() {
        if ((pauseFirst || accesses > 0U) && fixture::hold.load()) {
            {
                std::lock_guard<std::mutex> lock(fixture::mutex);
                fixture::blocked.store(true);
            }
            fixture::changed.notify_all(); return false;
        }
        // The controlled pause precedes physical SDK admission. Keep that
        // scheduling seam without publishing a hardware waiter while parked.
        if (!requestPublished) {
            inner.RequestFlash();
            requestPublished = true;
        }
        const bool accepted = inner.TryBeginRequestedFlash();
        if (accepted) { ++accesses; requestPublished = false; }
        return accepted;
    }
    void EndFlash() { inner.EndFlash(); }
    void ResetPause(bool firstAccess = false) { accesses = 0U; pauseFirst = firstAccess; fixture::blocked.store(false); fixture::hold.store(true); }
    bool AdvanceRender() { if (!inner.TryBeginRender()) return false; inner.EndRender(); return true; }
  private:
    RmtFlashArbiter inner;
    unsigned accesses = 0U;
    bool pauseFirst = false;
    bool requestPublished = false;
};
}

TaskHandle_t PersistenceTaskCreatePinnedToCore(TaskFunction_t task, const char*, uint32_t, void* context,
                                  uint32_t, StackType_t*, StaticTask_t*, int) {
    if (fixture::failNextTaskCreate.exchange(false)) { return nullptr; }
    fixture::worker = std::thread([=] { try { task(context); } catch (const fixture::Stop&) {} });
    return reinterpret_cast<TaskHandle_t>(1U);
}
void PersistenceTaskDelay(uint32_t ticks) {
    if (std::this_thread::get_id() == fixture::worker.get_id()) {
        if (ticks == 250U && fixture::holdRetry.load()) {
            fixture::retryDelayReached.store(true);
            fixture::changed.notify_all();
            fixture::Wait([] { return !fixture::holdRetry.load(); });
            fixture::retryDelayReached.store(false);
        }
        if (fixture::blocked.load() && fixture::hold.load()) {
            fixture::Wait([] { return !fixture::hold.load(); });
        }
        std::this_thread::yield();
    } else {
        fixture::Wait([] { return fixture::blocked.load(); });
        fixture::now.fetch_add(ticks);
    }
}
void PersistenceTaskDelete(TaskHandle_t) {}
uint32_t PersistenceNotifyTake(bool, uint32_t) {
    std::unique_lock<std::mutex> lock(fixture::mutex);
    fixture::workerAtWait.store(true);
    fixture::changed.notify_all();
    fixture::changed.wait(lock, [] { return fixture::notified || fixture::stopping; });
    if (fixture::stopping) { throw fixture::Stop{}; }
    fixture::workerAtWait.store(false);
    fixture::notified = false; return 1U;
}
void PersistenceNotifyGive(TaskHandle_t) {
    std::lock_guard<std::mutex> lock(fixture::mutex);
    fixture::workerAtWait.store(false);
    fixture::notified = true; fixture::changed.notify_all();
}
using UBaseType_t = uint32_t;
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define ARDUINO 1
#define xTaskCreateStaticPinnedToCore PersistenceTaskCreatePinnedToCore
#define vTaskDelay PersistenceTaskDelay
#define vTaskDelete PersistenceTaskDelete
#define ulTaskNotifyTake PersistenceNotifyTake
#define xTaskNotifyGive PersistenceNotifyGive
#include "../../src/settings_persistence_worker.cpp"
#include "../../src/settings_persistence_worker_recovery.cpp"
// Factory provisioning retains its separately modeled current-transfer guard.
// Ordinary worker bindings use the production arbiter type above.
#define RmtFlashArbiter PersistenceTestArbiter
#define FactoryProvisioning PersistenceFixtureProvisioning
#include "../../src/firmware_factory_provisioning.cpp"
#undef FactoryProvisioning
#undef RmtFlashArbiter
#include "../../src/current_game_log_service_session.cpp"
#include "../../src/current_game_log_service.cpp"
#include "../../src/current_game_sync_runtime.cpp"
#undef vTaskDelete
#undef vTaskDelay
#undef xTaskCreateStaticPinnedToCore

struct PersistenceTestBle {
    bool enabled = false;
    bool allowFactoryQuiesce = true;
    void DrainEventQueue() {}
    void Update(fxe::SettingsStore&) {}
    void SetFactoryVerifierRuntimeAllowed(bool) {}
    void ResetConnectionButtonState() {}
    bool IsOtaInProgress() const { return false; }
    bool IsOtaRuntimeAccepted() const { return true; }
    bool QuiesceFactoryBleDut() { return allowFactoryQuiesce; }
    void SetFactoryVerifierConfiguredEnabled(bool value) { enabled = value; }
    bool EnableFactoryVerifierEndpoint(fxe::SettingsStore&) { enabled = true; return true; }
};
struct PersistenceTestRadio {
    bool radioActive = false, bleProof = false;
    bool hardwareTestActive = false;
    void Update(bool active, bool radio, modules::ManufacturingTestStage,
                PersistenceTestBle&, uint32_t) { hardwareTestActive = active; radioActive = active && radio; }
    void Update(bool active, bool radio, modules::ManufacturingTestStage,
                PersistenceTestBle&, uint32_t, uint32_t) { hardwareTestActive = active; radioActive = active && radio; }
    uint32_t DiscoveryDeadlineMs() const { return 1500U; }
    bool HasPeerCandidate() const { return false; }
    bool BleEndpointStartFailed() const { return false; }
    bool IsAutomaticVerifier() const { return false; }
    bool IsVerifierBleProofActive() const { return false; }
    bool HasPendingTerminalReply() const { return false; }
    void PollTerminalReply(PersistenceTestBle&, uint32_t) {}
    bool ConsumeDutBleVerified() { return false; }
    bool ConsumeVerifierBleVerified() { const bool proof=bleProof;bleProof=false;return proof; }
};
struct BoardInput : modules::IBoardInput {
    uint64_t mask = 0;
    bool TryReadSnapshot(uint64_t& out) override { out = mask; return true; }
    bool TryReadDelta(modules::SquareDelta&) override { return false; }
    void Resync() override {}
};
struct FirmwareSystem {
    fxe::SettingsStore& settings;
    firmware::SettingsPersistenceWorker& settingsPersistence;
    bool factoryBootOnly = false, factoryVerifierBoot = false;
    bool factoryRadioReleasedForSave = false, factoryPersistenceStartFailed = false;
    bool rebootPersistencePending = false, rebootPersistenceStarted = false;
    bool otaBootAwaitingHealth = false;
    firmware::FactoryBootIntent pendingFactoryBootIntent = firmware::FactoryBootIntent::None;
    firmware::FactoryCompletionReboot factoryCompletion;
    hardware::RmtFlashArbiter rmtFlashArbiter;
    struct { void MarkBootHealthy() {} } bootGuard;
    std::atomic<bool> durableSettingsSnapshotObserved{false};
    firmware::HardwareTestMode hardwareTest;
    struct { bool active=false; bool IsActive() const { return active; } void Update() {} } comptest;
    unsigned reseeds=0U, queueClears=0U;
    modules::HintButtonGestureController hintButtonController{};
    modules::ResetHoldGestureController resetHoldController{};
    struct { void Reset(){} } quickSettingsOverlayController;
    struct { unsigned* count; void SuppressHeldControlsUntilRelease(){++*count;} } gpioUserInput{&reseeds};
    struct { unsigned* count; void Clear(){++*count;} } buttonInputQueue{&queueClears};
    PersistenceTestBle ble;
    PersistenceTestRadio factoryRadio;
    modules::BoardIdentityPersistence boardIdentity;
    struct {
        firmware::PersistenceFixtureProvisioning* actual = nullptr;
        unsigned durableSnapshots = 0U;
        unsigned trackedFactorySnapshots = 0U;
        std::atomic<uint32_t> expectedReceipt{0U};
        modules::FactoryProvisioningState state = modules::FactoryProvisioningState::LegacyExempt;
        bool RequireFactoryTest() { if(actual) return actual->RequireFactoryTest(); state = modules::FactoryProvisioningState::Required; return true; }
        void ExpectFactoryResult(const modules::HardwareTestRecord& record) { if(actual)actual->ExpectFactoryResult(record);expectedReceipt.store(0U); }
        std::atomic<uint32_t>* ExpectedReceiptPublication() { return actual ? actual->ExpectedReceiptPublication() : &expectedReceipt; }
        void ForgetExpectedResult() { if(actual)actual->ForgetExpectedResult();expectedReceipt.store(0U); }
        void OnDurableSettingsSnapshot(const JsonDocument& doc, const uint32_t receipt = 0U) {
            if(actual)actual->OnDurableSettingsSnapshot(doc,receipt);
            ++durableSnapshots;
            if (receipt != 0U && receipt == expectedReceipt.load()) ++trackedFactorySnapshots;
        }
        bool AllowsCompletedHardwareTestResult() const { return actual ? actual->AllowsCompletedHardwareTestResult() : true; }
        bool AllowsNewBoardIdentity() const { return true; }
        modules::FactoryProvisioningState State() const {
            return actual ? actual->State() : state;
        }
    } factoryProvisioning;
    struct {
        void RequestCancelAllThinking() {}
        void SetGameHistoryIdentityGenerator(
            bool (*)(void*, modules::game_history::GameId&), void*) {}
    } gamePersistence;
    hardware::RuntimeBoardHardwareProfile runtimeHardware =
        hardware::RuntimeProfileForS3Sample({true, false, false});
    hardware::ButtonManager<2U> factoryButtons;
    uint32_t ledPowerPollCostMs = 0U;
    firmware::LedRuntimeAdmission ledRuntimeAdmission;
    struct {
        bool Update(fxe::SettingsStore&) { return true; }
        bool CheckFactoryPresence(bool motionOk) { return motionOk; }
    } motionWake;
    struct {
        struct Attempt { bool attemptCompleted = false; };
        Attempt UpdateAttempt() { return {}; }
        bool Sensor0Ok() const { return false; }
        bool Sensor1Ok() const { return false; }
        uint32_t GetLastLux0() const { return 0U; }
        uint32_t GetLastLux1() const { return 0U; }
    } ambientLights;
    BoardInput boardInput;
    struct {
        uint8_t LastRespondingExpanderMask() const { return 0x0FU; }
    } inputScan;
};
// Reset delivery is outside this persistence fixture; a normal save must not
// reach it. The production reboot timing gate has separate owner coverage.
struct { void restart() { std::puts("[FAIL] unexpected reset delivery in persistence fixture"); std::abort(); } } ESP;
namespace firmware_internal {
void ArmFactoryBootIntent(firmware::FactoryBootIntent);
bool IsHardwareTestSavePending();
void ReleaseHardwareTestInput(FirmwareSystem&);
bool RequestFactoryBoot(FirmwareSystem&, firmware::FactoryBootIntent);
}
#include "../../src/firmware_identity.cpp"
// Actual manufacturing runtime and its record disposition code, not a parallel
// transaction implementation. Radio SDK wiring is unused by this save journey.
#define vTaskDelay PersistenceTaskDelay
#include "../../src/firmware_hardware_test_runtime.cpp"
#undef vTaskDelay
namespace firmware_internal {
void ArmFactoryBootIntent(firmware::FactoryBootIntent) {
    std::puts("[FAIL] RTC reboot delivery outside manufacturing persistence fixture");
    std::abort();
}
void ReleaseHardwareTestInput(FirmwareSystem&);
bool RequestFactoryBoot(FirmwareSystem&, firmware::FactoryBootIntent);
FirmwareSystem* consoleSystem = nullptr;
FirmwareSystem& GetSystem() { return *consoleSystem; }
bool PollRuntimeBootHealth(FirmwareSystem&) { return true; }
bool PollPendingReboot(FirmwareSystem&) { return false; }
void PollLedPowerSourceStatus(FirmwareSystem& sys, uint32_t) {
    fixture::now.fetch_add(sys.ledPowerPollCostMs);
}
void MaybeEnableConsoleOutput() {}
runtime::SystemContext& GetSystemContext() {
    static runtime::SystemContext context;
    return context;
}
// GPIO and the firmware input gate are outside this fixture. Use the same
// debouncer and fresh event timestamp as factory button dispatch.
void PollButtonsAndDispatch() {
    auto& sys = GetSystem();
    const uint32_t nowMs = fxe::Millis();
    modules::ButtonEvent event;
    while (sys.factoryButtons.TryReadButtonEvent(event)) {
        if (sys.hardwareTest.IsActive()) sys.hardwareTest.OnButtonEvent(event, nowMs);
    }
}
firmware::LedRenderAttemptResult RenderLeds(bool = false) {
    return firmware::LedRenderAttemptResult::Completed;
}
void delay(unsigned long) {}
// Command parsing/request handoff only. Real busy admission is exercised by
// test_litchcraft_reboot's actual RequestFactoryBoot owner. This result is an
// external boundary input, never a duplicated factory admission policy.
bool factoryBootHandoffAdmitted = true;
unsigned attemptedFactoryBoots = 0U;
unsigned requestedFactoryBoots = 0U;
firmware::FactoryBootIntent requestedFactoryBootIntent = firmware::FactoryBootIntent::None;
bool RequestFactoryBoot(FirmwareSystem&, firmware::FactoryBootIntent intent) {
    ++attemptedFactoryBoots;
    if (!factoryBootHandoffAdmitted) return false;
    ++requestedFactoryBoots;
    requestedFactoryBootIntent = intent;
    return true;
}
namespace console_commands {
// Completion is outside this command-execution test. The actual parser and
// manufacturing command handler below execute unchanged.
size_t AddMatchIfPrefix(const char*, const char*, const char**, size_t count, size_t) { return count; }
}
}
#include "../../src/firmware_factory_runtime.cpp"
#include "../../src/firmware_console_hardware_test.cpp"
#undef ARDUINO

namespace {
int failures = 0;
void Check(bool ok, const char* message) { if (!ok) { ++failures; std::printf("[FAIL] %s\n", message); } }
// Controlled native fault after actual storage access: change one staged JSON
// byte before the worker's real hash readback. The primary/backup are untouched.
struct ScratchReadbackFault {
    explicit ScratchReadbackFault(const char* settingsPath) : path(settingsPath) {}
    const char* path;
    std::atomic<bool> armed{false};
    std::atomic<unsigned> corruptions{0U};
    static void Observe(void* context, const bool begin) {
        hardware::PersistenceTestArbiter::ObserveStorage(nullptr, begin);
        auto& fault = *static_cast<ScratchReadbackFault*>(context);
        if (begin || !fault.armed.load()) return;
        for (const char* suffix : {".tmp", ".retry"}) {
            const std::string scratch = std::string(fault.path) + suffix;
            if (!std::filesystem::is_regular_file(scratch)) continue;
            std::FILE* file = std::fopen(scratch.c_str(), "r+b");
            if (file == nullptr) continue;
            if (std::fgetc(file) == '{' && std::fseek(file, 0L, SEEK_SET) == 0 &&
                std::fputc('!', file) != EOF && std::fflush(file) == 0) ++fault.corruptions;
            std::fclose(file);
        }
    }
};
// The actual RTC intent and profile decision execute here. Arduino Setup,
// reset delivery and SDK initialization remain controlled boundaries; this
// helper only selects the profile for the real persistence/gesture owners.
void SelectFixtureBootProfile(FirmwareSystem& sys, firmware::FactoryBootIntent intent) {
    firmware::FactoryBootRequest request{};
    firmware::ArmFactoryBootRequest(request, intent);
    const auto consumed = firmware::ConsumeFactoryBootRequest(request, true, false,
        hardware::SupportsProductionHardwareTest(sys.runtimeHardware.detectedRevision));
    modules::HardwareTestRecord saved;
    const bool validatedVerifier = consumed == firmware::FactoryBootIntent::Verifier &&
        sys.factoryProvisioning.AllowsCompletedHardwareTestResult() &&
        modules::DecodeHardwareTestRecord(sys.settings.ReadValue(
            modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""), saved) &&
        modules::IsFactoryVerifierEligible(saved) &&
        saved.revision == sys.runtimeHardware.detectedRevision;
    const auto profile = firmware::FactoryBootProfileFor(
        consumed == firmware::FactoryBootIntent::Test, validatedVerifier);
    sys.factoryBootOnly = profile != firmware::FactoryBootProfile::Normal;
    sys.factoryVerifierBoot = profile == firmware::FactoryBootProfile::Verifier;
    sys.factoryRadioReleasedForSave = false;
    sys.factoryCompletion.Cancel();
    sys.ble.SetFactoryVerifierConfiguredEnabled(false);
}
struct SnapshotTransactionProbe {
    using Outcome =
        firmware::SettingsPersistenceWorker::SnapshotTransactionOutcome;
    static constexpr const char* KEY = "CURRENT_LINE_TRANSACTION_TEST";
    std::atomic<unsigned> prepareCalls{0U};
    std::atomic<unsigned> resultCalls{0U};
    std::atomic<unsigned> savedResults{0U};
    std::atomic<unsigned> failedResults{0U};
    std::atomic<unsigned> wrongThreadCalls{0U};
    std::atomic<unsigned> malformedResults{0U};
    std::atomic<unsigned> publishedEarly{0U};
    std::atomic<unsigned> lastPrepared{0U};
    std::atomic<uint32_t> completedAtPrepare{0U};
    std::atomic<bool> rejectNext{false};
    firmware::SettingsPersistenceWorker* worker = nullptr;

    static bool Prepare(void* context, JsonDocument& document) {
        auto& self = *static_cast<SnapshotTransactionProbe*>(context);
        if (std::this_thread::get_id() != fixture::worker.get_id()) {
            self.wrongThreadCalls.fetch_add(1U);
        }
        const unsigned marker = self.prepareCalls.fetch_add(1U) + 1U;
        self.lastPrepared.store(marker);
        const uint32_t completed = self.worker == nullptr
                                       ? 0U
                                       : self.worker->GetCompletedSaveCount();
        self.completedAtPrepare.store(completed);
        document[KEY] = marker;
        return !self.rejectNext.exchange(false);
    }

    static void Complete(void* context, Outcome outcome,
                         const JsonDocument& document) {
        auto& self = *static_cast<SnapshotTransactionProbe*>(context);
        if (std::this_thread::get_id() != fixture::worker.get_id()) {
            self.wrongThreadCalls.fetch_add(1U);
        }
        self.resultCalls.fetch_add(1U);
        const bool saved = outcome == Outcome::Saved;
        if (saved) self.savedResults.fetch_add(1U);
        else self.failedResults.fetch_add(1U);
        if ((document[KEY] | 0U) != self.lastPrepared.load()) {
            self.malformedResults.fetch_add(1U);
        }
        if (self.worker != nullptr &&
            self.worker->GetCompletedSaveCount() !=
                self.completedAtPrepare.load()) {
            self.publishedEarly.fetch_add(1U);
        }
    }
};
using persistence_test::CurrentLineMemoryStorage;

modules::GamePersistenceState CurrentLineState(const uint16_t ply,
                                               const uint64_t revision) {
    modules::GamePersistenceState state;
    const char* const fen = ply == 0U
        ? modules::INITIAL_FEN
        : "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1";
    Check(state.fen.Assign(fen) &&
              state.undoBaseFen.Assign(modules::INITIAL_FEN),
          "current-line fixture FENs fit");
    modules::DuckGameDurableState classic;
    classic.variant = modules::GameVariant::Classic;
    char encodedVariant[32U] = {};
    Check(modules::EncodeDuckGameDurableState(
              classic, encodedVariant, sizeof(encodedVariant)) &&
              state.variantState.Assign(encodedVariant),
          "current-line fixture variant encodes");
    state.gameHistory.present = true;
    state.gameHistory.gameId.bytes[0U] = 0x52U;
    state.gameHistory.lineRevision = revision;
    state.gameHistory.currentPly = ply;
    if (ply != 0U) {
        const uint8_t move[modules::UNDO_HISTORY_FIELDS_PER_MOVE] = {
            0U, static_cast<uint8_t>('P'), 0U,
            modules::game_history::INVALID_SQUARE, 0U, 52U, 36U};
        std::memcpy(state.undoHistory, move, sizeof(move));
        state.undoHistoryCount = sizeof(move);
    }
    return state;
}

void RunCurrentGameLogWorkerJourney() {
    fixture::stopping = false;
    fixture::notified = false;
    fixture::workerAtWait.store(false);
    fixture::hold.store(false);
    fixture::blocked.store(false);
    const char* const path = "build/t_current_game_transaction.json";
    std::remove(path);
    std::remove("build/t_current_game_transaction.json.bak");
    std::remove("build/t_current_game_transaction.json.tmp");
    fxe::SettingsStoreConfig config;
    config.path = path;
    fxe::SettingsStore source(config);
    Check(source.Load(), "current-line worker fixture loads settings");
    modules::SupportRuntimeStats supportStats;
    const auto sourceRevision = source.GetRevision();
    const auto sourceSaves = source.GetSaveCount();
    supportStats.BeginBoot(source.Doc());
    Check(source.GetRevision() == sourceRevision && source.GetSaveCount() == sourceSaves &&
              !source.Doc().containsKey(modules::SupportRuntimeStats::KEY),
          "runtime boot accounting leaves live settings and save admission untouched");
    auto state = CurrentLineState(0U, 1U);
    Check(modules::WriteGamePersistenceState(source.Doc(), state),
          "current-line worker fixture writes untouched game");
    modules::GamePersistenceState decodedState;
    modules::game_history::GameHistoryCaptureSnapshot decodedSnapshot;
    Check(modules::ReadGamePersistenceState(source.Doc(), decodedState) &&
              modules::game_history::GameHistorySnapshotReader(decodedState)
                  .Build(decodedSnapshot),
          "current-line worker fixture builds a complete snapshot");
    hardware::PersistenceTestArbiter arbiter;
    firmware::SettingsPersistenceWorker worker(config);
    worker.SetSupportRuntimeStats(&supportStats);
    CurrentLineMemoryStorage storage;
    firmware::CurrentGameLogService service(storage.Ops());
    Check(service.Configure(worker, source.Doc()) &&
              (worker.SetStorageAccessTestHook(&hardware::PersistenceTestArbiter::ObserveStorage, nullptr), worker.Begin(source)),
          "current-line service owns the actual worker transaction");
    Check(worker.GetCompletedSaveCount() == 0U && supportStats.Snapshot(0U).savedCheckpointMs == 0U,
          "binding runtime accounting creates no startup save or confirmed checkpoint");
    fixture::now.fetch_add(17U);

    auto save = [&](const char* message) {
        const uint32_t completed = worker.GetCompletedSaveCount();
        const uint32_t failed = worker.GetFailureCount();
        Check(worker.Enqueue(source.Doc()), message);
        fixture::Wait([&] {
            return (worker.GetCompletedSaveCount() > completed ||
                    worker.GetFailureCount() > failed) &&
                   fixture::workerAtWait.load();
        });
        Check(worker.GetCompletedSaveCount() > completed,
              "current-line worker transaction reaches durable settings");
    };
    save("untouched game snapshot queues");
    fxe::SettingsStore firstDisk(config);
    modules::current_game_log::CommitPointer first;
    Check(firstDisk.Load() &&
              modules::current_game_log::ReadCommitPointer(
                  firstDisk.Doc(), first) && first.present &&
              first.slot == modules::current_game_log::Slot::A &&
              first.currentPly == 0U && !first.archiveRequired &&
              !storage.slots[0U].empty(),
          "log bytes promote before the exact untouched pointer");
    const uint64_t checkpoint = firstDisk.Doc()[modules::SupportRuntimeStats::KEY]["boot_checkpoint_ms"].as<uint64_t>();
    Check(checkpoint != 0U && supportStats.Snapshot(0U).savedCheckpointMs == checkpoint &&
              firstDisk.Doc()[modules::SupportRuntimeStats::KEY]["boots"].as<uint64_t>() == 1U,
          "one genuine worker save persists runtime totals and only verified completion publishes its checkpoint");

    const auto firstBytes = storage.slots[0U];
    auto acknowledgedReplacement = CurrentLineState(0U, 1U);
    acknowledgedReplacement.gameHistory.gameId.bytes[0U] = 0x53U;
    Check(modules::WriteGamePersistenceState(
              source.Doc(), acknowledgedReplacement),
          "acknowledged replacement fixture writes a new game");
    save("acknowledged replacement snapshot queues");
    fxe::SettingsStore replacementDisk(config);
    modules::current_game_log::CommitPointer replacementPointer;
    Check(replacementDisk.Load() &&
              modules::current_game_log::ReadCommitPointer(
                  replacementDisk.Doc(), replacementPointer) &&
              replacementPointer.slot == modules::current_game_log::Slot::B &&
              replacementPointer.generation == 2U &&
              replacementPointer.gameId ==
                  acknowledgedReplacement.gameHistory.gameId &&
              storage.slots[0U] == firstBytes &&
              !replacementPointer.archiveRequired,
          "acknowledged replacement preserves old slot until pointer promotion");

    state = CurrentLineState(1U, 2U);
    state.gameHistory.gameId = acknowledgedReplacement.gameHistory.gameId;
    Check(modules::WriteGamePersistenceState(source.Doc(), state),
          "current-line worker fixture writes committed move");
    const size_t bytesBeforeMove = storage.slots[1U].size();
    const char* const backupPath =
        "build/t_current_game_transaction.json.bak";
    const char* const backupBlockerPath =
        "build/t_current_game_transaction.json.bak/block";
    std::remove(backupPath);
    Check(std::filesystem::create_directory(backupPath),
          "current-line retry installs a non-removable backup target");
    std::FILE* backupBlocker = std::fopen(backupBlockerPath, "wb");
    Check(backupBlocker != nullptr,
          "current-line retry makes the backup directory non-empty");
    if (backupBlocker != nullptr) std::fclose(backupBlocker);
    fixture::retryDelayReached.store(false);
    fixture::holdRetry.store(true);
    const uint32_t moveCompleted = worker.GetCompletedSaveCount();
    Check(worker.Enqueue(source.Doc()), "committed move snapshot queues");
    fixture::Wait([] { return fixture::retryDelayReached.load(); });
    Check(storage.slots[1U].size() > bytesBeforeMove,
          "current-line bytes append before settings promotion");
    Check(std::remove(backupBlockerPath) == 0 &&
              std::filesystem::remove(backupPath),
          "current-line retry removes the one-shot rename obstruction");
    fixture::holdRetry.store(false);
    fixture::changed.notify_all();
    fixture::Wait([&] {
        return worker.GetCompletedSaveCount() > moveCompleted &&
            fixture::workerAtWait.load();
    });
    fxe::SettingsStore movedDisk(config);
    modules::current_game_log::CommitPointer moved;
    Check(movedDisk.Load() &&
              modules::current_game_log::ReadCommitPointer(
                  movedDisk.Doc(), moved) && moved.present &&
              moved.slot == modules::current_game_log::Slot::B &&
              moved.currentPly == 1U && moved.lineRevision == 2U &&
              moved.archiveRequired &&
              moved.byteLength > first.byteLength &&
              storage.slots[1U].size() == moved.byteLength,
          "settings retry promotes one append and raises archiveRequired");

    auto replacement = CurrentLineState(0U, 1U);
    replacement.gameHistory.gameId.bytes[0U] = 0x54U;
    replacement.gameHistory.captureIncomplete = true;
    Check(modules::WriteGamePersistenceState(source.Doc(), replacement),
          "replacement fixture writes a different game identity");
    const uint32_t replacementCompleted = worker.GetCompletedSaveCount();
    const uint32_t replacementFailures = worker.GetFailureCount();
    const uint32_t serviceFailures = service.FailureCount();
    Check(worker.Enqueue(source.Doc()),
          "unarchived replacement reaches the transaction owner");
    fixture::Wait([&] {
        return worker.GetFailureCount() > replacementFailures &&
            fixture::workerAtWait.load();
    });
    fxe::SettingsStore rejectedDisk(config);
    modules::current_game_log::CommitPointer retained;
    Check(worker.GetCompletedSaveCount() == replacementCompleted &&
              service.FailureCount() == serviceFailures + 1U &&
              rejectedDisk.Load() &&
              modules::current_game_log::ReadCommitPointer(
                  rejectedDisk.Doc(), retained) &&
              retained.gameId == moved.gameId &&
              retained.prefixCrc32 == moved.prefixCrc32 &&
              retained.archiveRequired,
          "unsupported partial replacement preserves durable authority");

    modules::current_game_sync::Digest archiveDigest;
    archiveDigest.fingerprint[0U] = 0xA5U;
    Check(service.AdmitArchiveAcknowledgement(moved, archiveDigest) ==
              modules::current_game_sync::ArchiveAdmission::Accepted &&
              service.ConsumeArchiveSaveRequest(),
          "archive acknowledgement is admitted against its durable authority");
    const uint32_t staleFailures = worker.GetFailureCount();
    Check(worker.Enqueue(source.Doc()),
          "changed live game reaches archive acknowledgement recheck");
    fixture::Wait([&] {
        return worker.GetFailureCount() > staleFailures &&
            fixture::workerAtWait.load();
    });
    bool staleSucceeded = true;
    modules::current_game_log::CommitPointer staleAuthority;
    Check(service.TakeArchiveCompletion(staleSucceeded, staleAuthority) &&
              !staleSucceeded && staleAuthority.archiveRequired,
          "changed live game rejects the stale archive acknowledgement");

    state = CurrentLineState(1U, 2U);
    state.gameHistory.gameId = acknowledgedReplacement.gameHistory.gameId;
    Check(modules::WriteGamePersistenceState(source.Doc(), state),
          "archive acknowledgement retains the exact current game snapshot");
    Check(service.AdmitArchiveAcknowledgement(moved, archiveDigest) ==
              modules::current_game_sync::ArchiveAdmission::Accepted &&
              service.ConsumeArchiveSaveRequest(),
          "exact current-line archive acknowledgement requests one durable save");
    save("archive acknowledgement snapshot queues");
    bool archiveSucceeded = false;
    modules::current_game_log::CommitPointer archived;
    fxe::SettingsStore archivedDisk(config);
    Check(service.TakeArchiveCompletion(archiveSucceeded, archived) &&
              archiveSucceeded && archived.present &&
              !archived.archiveRequired && archivedDisk.Load() &&
              modules::current_game_log::ReadCommitPointer(
                  archivedDisk.Doc(), archived) &&
              !archived.archiveRequired,
          "archive completion publishes only after the cleared pointer is durable");

    state = CurrentLineState(0U, 3U);
    state.gameHistory.gameId = acknowledgedReplacement.gameHistory.gameId;
    modules::DuckGameDurableState undecided;
    char undecidedText[32U]{};
    Check(modules::EncodeDuckGameDurableState(undecided, undecidedText, sizeof(undecidedText)) &&
              state.variantState.Assign(undecidedText) &&
              modules::WriteGamePersistenceState(source.Doc(), state),
          "same-ID played Undo cannot masquerade as fresh Undecided NewGame");
    const uint32_t sameIdFailures = worker.GetFailureCount();
    const uint32_t sameIdCompleted = worker.GetCompletedSaveCount();
    Check(worker.Enqueue(source.Doc()), "same-ID retirement attempt reaches the real service");
    fixture::Wait([&] {
        return worker.GetFailureCount() > sameIdFailures && fixture::workerAtWait.load();
    });
    fxe::SettingsStore sameIdDisk(config);
    modules::current_game_log::CommitPointer sameIdRetained;
    Check(worker.GetCompletedSaveCount() == sameIdCompleted && sameIdDisk.Load() &&
              modules::current_game_log::ReadCommitPointer(sameIdDisk.Doc(), sameIdRetained) &&
              sameIdRetained.gameId == archived.gameId && sameIdRetained.currentPly == 1U &&
              !sameIdRetained.archiveRequired && service.LastResult() ==
                  modules::current_game_log::Result::Bounds,
          "same-ID played line remains durable through rejected Undecided retirement");

    state = CurrentLineState(0U, 3U);
    state.gameHistory.gameId = acknowledgedReplacement.gameHistory.gameId;
    Check(modules::WriteGamePersistenceState(source.Doc(), state),
          "current-line worker fixture writes Undo snapshot");
    save("Undo snapshot queues");
    fxe::SettingsStore undoDisk(config);
    modules::current_game_log::CommitPointer undone;
    Check(undoDisk.Load() &&
              modules::current_game_log::ReadCommitPointer(
                  undoDisk.Doc(), undone) && undone.present &&
              undone.slot == modules::current_game_log::Slot::A &&
              undone.generation == 3U && undone.currentPly == 0U &&
              undone.lineRevision == 3U && undone.archiveRequired &&
              !storage.slots[0U].empty(),
          "Undo rewrites the inactive slot before pointer promotion");
    Check(storage.beginBatchCalls == 8U && storage.endBatchCalls == 8U,
          "every prepared current-line snapshot closes one storage batch");

    {
        std::lock_guard<std::mutex> lock(fixture::mutex);
        fixture::stopping = true;
        fixture::changed.notify_all();
    }
    fixture::worker.join();
    std::remove(path);
    std::remove("build/t_current_game_transaction.json.bak");
    std::remove("build/t_current_game_transaction.json.tmp");
}

void RunLongCurrentGameSnapshotJourney() {
    fixture::stopping = false;
    fixture::notified = false;
    fixture::workerAtWait.store(false);
    fixture::hold.store(false);
    fixture::blocked.store(false);
    const char* const path = "build/t_current_game_long_snapshot.json";
    for (const char* suffix : {"", ".bak", ".tmp"}) {
        std::remove((std::string(path) + suffix).c_str());
    }
    fxe::SettingsStoreConfig config;
    config.path = path;
    fxe::SettingsStore source(config);
    Check(source.Load(), "long game snapshot loads settings");
    size_t descriptorCount = 0U;
    const auto* descriptors = modules::settings_registry::EnumerateAll(descriptorCount);
    for (size_t i = 0U; i < descriptorCount; ++i) {
        const auto& descriptor = descriptors[i];
        if (!descriptor.IsPersisted() || descriptor.IsRetired()) continue;
        switch (descriptor.type) {
            case fxe::SettingType::Bool:
                source.Doc()[descriptor.key] = descriptor.defaultUint != 0U;
                break;
            case fxe::SettingType::UInt32:
                source.Doc()[descriptor.key] = descriptor.defaultUint;
                break;
            case fxe::SettingType::String:
            case fxe::SettingType::Enum:
                source.Doc()[descriptor.key] = descriptor.defaultStr != nullptr
                    ? descriptor.defaultStr : "";
                break;
        }
    }
    auto state = CurrentLineState(0U, 1U);
    Check(modules::WriteGamePersistenceState(source.Doc(), state),
          "long game snapshot writes production settings and opening");
    hardware::PersistenceTestArbiter arbiter;
    firmware::SettingsPersistenceWorker worker(config);
    CurrentLineMemoryStorage storage;
    firmware::CurrentGameLogService service(storage.Ops());
    Check(service.Configure(worker, source.Doc()) && (worker.SetStorageAccessTestHook(&hardware::PersistenceTestArbiter::ObserveStorage, nullptr), worker.Begin(source)),
          "long game snapshot uses real service and worker");
    auto save = [&] {
        const uint32_t completed = worker.GetCompletedSaveCount();
        const uint32_t failed = worker.GetFailureCount();
        Check(worker.Enqueue(source.Doc()), "long game snapshot queues");
        fixture::Wait([&] {
            return (worker.GetCompletedSaveCount() > completed ||
                    worker.GetFailureCount() > failed) && fixture::workerAtWait.load();
        });
        const bool saved = worker.GetCompletedSaveCount() == completed + 1U;
        std::printf("[LONG_CURRENT_GAME_SNAPSHOT] ply=%u json_bytes=%zu saved=%u result=%u service_failures=%u\n",
                    state.gameHistory.currentPly, measureJson(source.Doc()), saved ? 1U : 0U,
                    static_cast<unsigned>(service.LastResult()), service.FailureCount());
        Check(saved, "long game snapshot reaches durable settings");
        return saved;
    };
    save();
    for (uint16_t ply = 0U; ply < modules::UNDO_HISTORY_MOVE_CAPACITY; ++ply) {
        const uint8_t cycle = static_cast<uint8_t>(ply % 4U);
        const uint8_t from = cycle == 0U ? 62U : cycle == 1U ? 6U : cycle == 2U ? 45U : 21U;
        const uint8_t to = cycle == 0U ? 45U : cycle == 1U ? 21U : cycle == 2U ? 62U : 6U;
        const uint8_t move[modules::UNDO_HISTORY_FIELDS_PER_MOVE] = {
            static_cast<uint8_t>(ply % 2U), static_cast<uint8_t>(ply % 2U == 0U ? 'N' : 'n'),
            0U, modules::game_history::INVALID_SQUARE, 0U, from, to};
        std::memcpy(state.undoHistory + state.undoHistoryCount, move, sizeof(move));
        state.undoHistoryCount += sizeof(move);
    }
    state.gameHistory.currentPly = modules::UNDO_HISTORY_MOVE_CAPACITY;
    state.gameHistory.lineRevision = modules::UNDO_HISTORY_MOVE_CAPACITY + 1U;
    Check(state.fen.Assign("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 128 65") &&
              modules::WriteGamePersistenceState(source.Doc(), state),
          "128 legal knight plies produce the full bounded undo snapshot");
    modules::GamePersistenceState decoded;
    modules::game_history::GameHistoryCaptureSnapshot snapshot;
    Check(modules::ReadGamePersistenceState(source.Doc(), decoded) &&
              modules::game_history::GameHistorySnapshotReader(decoded).Build(snapshot),
          "long source snapshot is valid before worker parsing");
    const bool longSaved = save();
    const bool unchangedSaved = save();
    fxe::SettingsStore disk(config);
    modules::current_game_log::CommitPointer pointer;
    Check(longSaved && unchangedSaved && disk.Load() &&
              modules::ReadGamePersistenceState(disk.Doc(), decoded) &&
              decoded.undoHistoryCount == state.undoHistoryCount &&
              std::memcmp(decoded.undoHistory, state.undoHistory, state.undoHistoryCount) == 0 &&
              decoded.fen.Equals(state.fen.CStr()) &&
              modules::current_game_log::ReadCommitPointer(disk.Doc(), pointer) &&
              pointer.currentPly == state.gameHistory.currentPly && pointer.archiveRequired,
          "long game preserves every undo byte and exact durable line through repeated saves");
    {
        std::lock_guard<std::mutex> lock(fixture::mutex);
        fixture::stopping = true;
        fixture::changed.notify_all();
    }
    fixture::worker.join();
    for (const char* suffix : {"", ".bak", ".tmp"}) {
        std::remove((std::string(path) + suffix).c_str());
    }
}

void RunCoalescedUndoFirstSnapshotJourney() {
    auto resetWorkerFixture = [] {
        fixture::stopping = false;
        fixture::notified = false;
        fixture::workerAtWait.store(false);
        fixture::hold.store(false);
        fixture::blocked.store(false);
        fixture::holdRetry.store(false);
        fixture::retryDelayReached.store(false);
    };
    auto stopWorker = [] {
        std::lock_guard<std::mutex> lock(fixture::mutex);
        fixture::stopping = true;
        fixture::changed.notify_all();
    };
    const char* const path = "build/t_current_game_coalesced_undo.json";
    std::remove(path);
    std::remove("build/t_current_game_coalesced_undo.json.bak");
    std::remove("build/t_current_game_coalesced_undo.json.tmp");
    fxe::SettingsStoreConfig config;
    config.path = path;
    CurrentLineMemoryStorage storage;

    resetWorkerFixture();
    fxe::SettingsStore source(config);
    Check(source.Load(), "coalesced Undo fixture loads settings");
    const auto undoneState = CurrentLineState(0U, 3U);
    Check(modules::WriteGamePersistenceState(source.Doc(), undoneState),
          "coalesced move then Undo is one first snapshot");
    hardware::PersistenceTestArbiter arbiter;
    firmware::SettingsPersistenceWorker worker(config);
    firmware::CurrentGameLogService service(storage.Ops());
    Check(service.Configure(worker, source.Doc()) &&
              (worker.SetStorageAccessTestHook(&hardware::PersistenceTestArbiter::ObserveStorage, nullptr), worker.Begin(source)) && worker.Enqueue(source.Doc()),
          "coalesced Undo snapshot starts the real transaction owner");
    fixture::Wait([&] {
        return worker.GetCompletedSaveCount() > 0U &&
            fixture::workerAtWait.load();
    });
    fxe::SettingsStore firstDisk(config);
    modules::current_game_log::CommitPointer first;
    Check(firstDisk.Load() &&
              modules::current_game_log::ReadCommitPointer(
                  firstDisk.Doc(), first) && first.present &&
              first.currentPly == 0U && first.lineRevision == 3U &&
              first.archiveRequired,
          "first consumed move-Undo snapshot remains archive-required");
    stopWorker();
    fixture::worker.join();

    resetWorkerFixture();
    fxe::SettingsStore restartedSource(config);
    Check(restartedSource.Load(),
          "coalesced Undo authority cold-loads after restart");
    hardware::PersistenceTestArbiter restartedArbiter;
    firmware::SettingsPersistenceWorker restartedWorker(config);
    firmware::CurrentGameLogService restartedService(storage.Ops());
    Check(restartedService.Configure(
              restartedWorker, restartedSource.Doc()) &&
              (restartedWorker.SetStorageAccessTestHook(&hardware::PersistenceTestArbiter::ObserveStorage, nullptr), restartedWorker.Begin(restartedSource)),
          "coalesced Undo service restarts from its durable pointer");
    auto replacement = CurrentLineState(0U, 1U);
    replacement.gameHistory.gameId.bytes[0U] = 0x53U;
    replacement.gameHistory.captureIncomplete = true;
    Check(modules::WriteGamePersistenceState(
              restartedSource.Doc(), replacement),
          "cold-restart replacement fixture writes another game");
    const uint32_t failures = restartedWorker.GetFailureCount();
    Check(restartedWorker.Enqueue(restartedSource.Doc()),
          "cold-restart replacement reaches archive admission");
    fixture::Wait([&] {
        return restartedWorker.GetFailureCount() > failures &&
            fixture::workerAtWait.load();
    });
    fxe::SettingsStore rejectedDisk(config);
    modules::current_game_log::CommitPointer retained;
    Check(rejectedDisk.Load() &&
              modules::current_game_log::ReadCommitPointer(
                  rejectedDisk.Doc(), retained) &&
              retained.gameId == first.gameId &&
              retained.prefixCrc32 == first.prefixCrc32 &&
              retained.archiveRequired,
          "cold restart rejects an unsupported partial replacement");
    Check(storage.beginBatchCalls == 2U && storage.endBatchCalls == 2U,
          "restart and rejection both release their storage batches");
    stopWorker();
    fixture::worker.join();
    std::remove(path);
    std::remove("build/t_current_game_coalesced_undo.json.bak");
    std::remove("build/t_current_game_coalesced_undo.json.tmp");
}
// Input documents are supplied here; the deferral, worker, current-line
// promotion and publication are production owners, with only SDK/flash scheduling held.
void RunDurableLinePublicationOrdering() {
    const char* path = "build/t_current_game_publication.json";
    for (const auto* suffix : {"", ".bak", ".tmp", ".retry"}) {
        std::filesystem::remove_all(std::string(path) + suffix);
    }
    fixture::now.store(0U); fixture::stopping = false; fixture::notified = false;
    fixture::workerAtWait.store(false); fixture::hold.store(false);
    fixture::blocked.store(false); fixture::holdRetry.store(false);
    modules::SettingsStoreConfig config; config.path = path;
    modules::SettingsStore settings(config);
    Check(settings.Load() && modules::WriteGamePersistenceState(
              settings.Doc(), CurrentLineState(0U, 1U)),
          "publication fixture prepares the initial current-line document");
    hardware::PersistenceTestArbiter arbiter;
    firmware::SettingsPersistenceWorker worker(config);
    CurrentLineMemoryStorage storage;
    firmware::CurrentGameLogService log(storage.Ops());
    firmware::CurrentGameSyncRuntime sync(worker, log, storage.Ops(), settings);
    uint32_t revision = 0U;
    std::array<uint8_t, 2U> body{};
    Check(!sync.CopyLinePublication(revision, body),
          "unconfigured history cannot publish availability");
    worker.SetStorageAccessTestHook(&hardware::PersistenceTestArbiter::ObserveStorage, nullptr);
    Check(sync.Configure(settings.Doc()) && worker.Begin(settings),
          "publication uses the actual current-line runtime and settings worker");
    Check(sync.CopyLinePublication(revision, body) && body == std::array<uint8_t, 2U>{1U, 0U},
          "unsaved RAM document does not claim a durable line");
    const uint32_t emptyRevision = revision;
    settings.MarkMustSaveDirty();
    fixture::now.fetch_add(config.mustSaveDelayMs - 1U);
    settings.Update();
    Check(worker.GetCompletedSaveCount() == 0U &&
              sync.CopyLinePublication(revision, body) && revision == emptyRevision,
          "ordinary deferred delay cannot publish the pending line early");
    arbiter.ResetPause(true);
    fixture::now.fetch_add(1U); settings.Update();
    fixture::Wait([] { return fixture::blocked.load(); });
    Check(sync.CopyLinePublication(revision, body) && revision == emptyRevision && body[1U] == 0U,
          "queued save blocked at flash access cannot publish durable content");
    fixture::hold.store(false); fixture::changed.notify_all();
    fixture::Wait([&] { return worker.GetCompletedSaveCount() == 1U && fixture::workerAtWait.load(); });
    Check(sync.CopyLinePublication(revision, body) && revision > emptyRevision && body[1U] == 1U,
          "exact committed current-line promotion advances availability after saving");
    uint32_t priorRevision = revision;
    for (const uint64_t lineRevision : {2U, 3U}) {
        Check(modules::WriteGamePersistenceState(settings.Doc(), CurrentLineState(1U, lineRevision)),
              "forward move and same-position newer revision prepare real history documents");
        settings.MarkMustSaveDirty();
        fixture::now.fetch_add(config.mustSaveDelayMs); settings.Update();
        fixture::Wait([&] { return worker.GetCompletedSaveCount() == lineRevision && fixture::workerAtWait.load(); });
        Check(sync.CopyLinePublication(revision, body) && revision > priorRevision && body[1U] == 1U,
              "durable content revision advances even when availability and FEN stay unchanged");
        priorRevision = revision;
    }
    { std::lock_guard<std::mutex> lock(fixture::mutex); fixture::stopping = true; fixture::changed.notify_all(); }
    fixture::worker.join();
}

// Existing VirtualLitch owns controls, game mutation and rendering. Only the
// production boot wiring is supplied here, using the actual Arduino worker.
void RunBoardFirstNewGameArchiveJourney() {
    using Result = modules::ControlCoordinatorResult;
    using Kind = modules::GamePersistenceCoordinator::ReplacementKind;
    using Opcode = modules::current_game_sync::Opcode;
    constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
    constexpr uint64_t PLAYED = (OPENING & ~(UINT64_C(1) << 52U)) | (UINT64_C(1) << 36U);
    // No phone, pending/failed/completed receipt, and failed New Game save each
    // use the actual board control owner plus actual settings worker/runtime.
    for (unsigned scenario = 0U; scenario < 5U; ++scenario) {
        const std::string path = "build/t_board_first_new_game_" + std::to_string(scenario) + ".json";
        for (const auto suffix : {"", ".bak", ".tmp", ".retry"}) {
            std::filesystem::remove_all(path + suffix);
        }
        modules::SettingsStoreConfig config; config.path = path.c_str();
        modules::SettingsStore seed(config);
        Check(seed.Load() && modules::WriteGamePersistenceState(seed.Doc(), CurrentLineState(1U, 2U)) &&
                  seed.SaveNowBlocking(), "board-first journey seeds a resumable played game");
        virtual_litch_config boardConfig{}; virtual_litch_default_config(&boardConfig);
        boardConfig.hardware = VIRTUAL_LITCH_HARDWARE_S3;
        test_support::BoundedHintSession board(boardConfig, path.c_str());
        auto& session = board.session;
        auto& settings = session.SettingsForPersistenceTest();
        fixture::stopping = false; fixture::notified = false;
        fixture::workerAtWait.store(false); fixture::hold.store(false);
        fixture::blocked.store(false); fixture::holdRetry.store(false);
        hardware::PersistenceTestArbiter arbiter;
        firmware::SettingsPersistenceWorker worker(config);
        ScratchReadbackFault readbackFault(path.c_str());
        CurrentLineMemoryStorage storage;
        firmware::CurrentGameLogService service(storage.Ops());
        firmware::CurrentGameSyncRuntime sync(worker, service, storage.Ops(), settings);
        Check(sync.Configure(settings.Doc()) && (worker.SetStorageAccessTestHook(&ScratchReadbackFault::Observe, &readbackFault), worker.Begin(settings)),
              "board-first journey wires actual worker/service/runtime");
        struct Wiring {
            firmware::CurrentGameSyncRuntime* sync;
            modules::GamePersistenceCoordinator* coordinator = nullptr;
            uint8_t identity = 0x70U;
        } wiring{&sync};
        session.RunControlIngress(
            [](void* context, modules::SettingsStore&, modules::ControlMutationService&,
               modules::GamePersistenceCoordinator& coordinator) {
                auto& wiring = *static_cast<Wiring*>(context); wiring.coordinator = &coordinator;
                coordinator.SetGameHistoryIdentityGenerator(
                    [](void* context, modules::game_history::GameId& out) {
                        out = {}; out.bytes[0U] = ++static_cast<Wiring*>(context)->identity; return true;
                    }, context);
                coordinator.SetReplacementAdmissionCallback(
                    [](void* context, const modules::GameHistoryPersistenceState& current, const Kind kind) {
                        return static_cast<Wiring*>(context)->sync->CheckReplacementAdmission(current, kind);
                    }, context);
            }, &wiring);
        auto& coordinator = *wiring.coordinator;
        auto history = [&] { return coordinator.GetGameHistoryPersistenceState(); };
        auto save = [&] {
            const uint32_t saved = worker.GetCompletedSaveCount();
            Check(settings.SaveNow(), "explicit fixture save queues through production settings worker");
            fixture::Wait([&] { return worker.GetCompletedSaveCount() > saved && fixture::workerAtWait.load(); });
        };
        auto checkpoint = [&](const char* stage, const char* next) {
            const auto state = history();
            const auto& frame = session.SemanticFrame();
            uint64_t lit = 0U; unsigned red = 0U, green = 0U;
            for (unsigned square = 0U; square < 64U; ++square) {
                for (unsigned led = 0U; led < frame.leds_per_square; ++led) {
                    const auto pixel = frame.board[square][led];
                    if (pixel.r || pixel.g || pixel.b || pixel.w) lit |= UINT64_C(1) << square;
                }
            }
            for (const auto pixel : frame.status) {
                if (pixel.r > pixel.g && pixel.r > pixel.b) ++red;
                if (pixel.g > pixel.r && pixel.g > pixel.b) ++green;
            }
            modules::ControlMutationOutcome outcome;
            const bool hasOutcome = session.TryGetLastBoardControlOutcome(outcome);
            std::printf("[BOARD_FIRST_NEW_GAME_ARCHIVE_JOURNEY] scenario=%u stage=%s t=%u occupancy=%016llx "
                        "expected=%016llx lit_squares=%016llx status_red=%u status_green=%u "
                        "variant=%u id0=%u revision=%llu ply=%u undo=%u placement=%u result=%d reason=%d "
                        "display=\"%.*s\" cue=%u saved=%u failed=%u next=%s\n",
                scenario, stage, session.State().now_ms,
                static_cast<unsigned long long>(session.State().physical_occupied_mask),
                static_cast<unsigned long long>(session.State().expected_occupied_mask),
                static_cast<unsigned long long>(lit), red, green,
                static_cast<unsigned>(session.GameVariant()), state.gameId.bytes[0U],
                static_cast<unsigned long long>(state.lineRevision), state.currentPly,
                session.UndoHistoryMoveCount(), session.PlacementModeActive(),
                hasOutcome ? static_cast<int>(outcome.status) : -1,
                hasOutcome ? static_cast<int>(outcome.reason) : -1,
                static_cast<int>(session.DisplayText().size()), session.DisplayText().data(),
                static_cast<unsigned>(session.EdgeCueIntent().kind), worker.GetCompletedSaveCount(),
                worker.GetFailureCount(), next);
        };
        Check(session.ObserveHallMask(PLAYED) == VIRTUAL_LITCH_OK && session.Advance(200U) == VIRTUAL_LITCH_OK,
              "restored e2-e4 occupancy reaches the production board");
        save();
        uint32_t playedPublication = 0U;
        std::array<uint8_t, 2U> linePublication{};
        Check(sync.CopyLinePublication(playedPublication, linePublication) &&
                  linePublication == std::array<uint8_t, 2U>{1U, 1U},
              "played baseline publishes its actual durable line");
        const auto before = history();
        Check(before.currentPly == 1U && sync.CheckReplacementAdmission(before, Kind::NewGame) == Result::Applied &&
                  sync.CheckReplacementAdmission(before, Kind::ExternalFen) == Result::ArchiveRequired,
              "unarchived line permits explicit New Game while external FEN keeps its stricter admission");
        checkpoint("played-unarchived", "hold-X+O-without-phone");
        uint32_t epoch = 1U;
        auto send = [&](const modules::game_history::Frame& frame) {
            uint8_t bytes[20U]{}; size_t size = 0U;
            Check(modules::game_history::EncodeFrame(frame, bytes, sizeof(bytes), size) &&
                      sync.Admit(bytes, size, epoch, false), "runtime admits bounded archive protocol frame");
            fixture::Wait([] { return fixture::workerAtWait.load(); });
        };
        auto take = [&] {
            uint8_t bytes[20U]{}; size_t size = 0U; uint32_t token = 0U;
            modules::game_history::Frame frame;
            Check(sync.Peek(bytes, sizeof(bytes), size, token, epoch) &&
                      modules::game_history::DecodeFrame(bytes, size, frame), "runtime publishes archive protocol response");
            sync.Accept(token, epoch); fixture::Wait([] { return fixture::workerAtWait.load(); });
            return frame;
        };
        modules::current_game_log::CommitPointer durable;
        fxe::SettingsStore disk(config);
        Check(disk.Load() && modules::current_game_log::ReadCommitPointer(disk.Doc(), durable) && durable.archiveRequired,
              "played line has exact durable archive-required authority");
        if (scenario >= 1U && scenario <= 3U) {
            const auto& line = storage.slots[static_cast<size_t>(durable.slot)];
            modules::Sha256 hash; Check(hash.Update(line.data(), line.size()), "phone fixture hashes exact received bytes");
            const auto fingerprint = hash.Finish();
            modules::game_history::Frame frame; frame.opcode = static_cast<uint8_t>(Opcode::Begin);
            frame.transactionId = 720U; frame.payloadLength = 12U;
            modules::current_game_sync::ResumeCursor cursor;
            cursor.nextRecordSequence = durable.currentPly + 2U;
            std::copy_n(fingerprint.begin(), cursor.prefixFingerprint.size(), cursor.prefixFingerprint.begin());
            Check(modules::current_game_sync::EncodeResumeCursor(cursor, frame.payload.data()), "complete cursor encodes");
            send(frame); const auto state = take(); const auto bounds = take();
            Check(state.opcode == static_cast<uint8_t>(Opcode::State) && bounds.opcode == static_cast<uint8_t>(Opcode::Bounds),
                  "actual transfer validates complete resume and advertises its exact digest");
            frame.opcode = static_cast<uint8_t>(Opcode::ArchiveAck); frame.frameSequence = bounds.frameSequence + 1U;
            std::copy_n(state.payload.begin(), 12U, frame.payload.begin()); send(frame);
            Check(service.CommittedPointer().archiveRequired,
                  "admitted archive acknowledgement cannot clear durable archive state");
            checkpoint("archive-ack-pending", "New-Game-remains-available");
        }
        const std::string temporary = path + ".tmp";
        if (scenario == 2U) {
            readbackFault.armed.store(true);
        }
        if (scenario == 2U || scenario == 3U) {
            const uint32_t failuresBefore = worker.GetFailureCount();
            const uint32_t savedBefore = worker.GetCompletedSaveCount();
            sync.ServiceMain();
            fixture::Wait([&] { return fixture::workerAtWait.load() && (scenario == 2U
                ? worker.GetFailureCount() >= failuresBefore + 3U : worker.GetCompletedSaveCount() > savedBefore); });
            if (scenario == 3U) {
                Check(take().opcode == static_cast<uint8_t>(Opcode::Archived) &&
                          disk.Load() && modules::current_game_log::ReadCommitPointer(disk.Doc(), durable) &&
                          !durable.archiveRequired && sync.CheckReplacementAdmission(history(), Kind::ExternalFen) == Result::Unsupported,
                      "exact archive completion clears state only after durable receipt; external FEN remains unsupported");
            } else {
                Check(service.CommittedPointer().archiveRequired,
                      "failed archive receipt retains exact prior durable authority");
            }
            uint32_t afterAck = 0U;
            Check(sync.CopyLinePublication(afterAck, linePublication) && afterAck == playedPublication,
                  "archive acknowledgement success or failure never starts a capture feedback loop");
            checkpoint(scenario == 2U ? "archive-save-failed" : "archive-durable", "hold-X+O");
        }
        Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, true) == VIRTUAL_LITCH_OK &&
                  session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, true) == VIRTUAL_LITCH_OK &&
                  session.Advance(40U) == VIRTUAL_LITCH_OK && session.Advance(2001U) == VIRTUAL_LITCH_OK,
              "raw X+O reaches production New Game hold threshold");
        modules::ControlMutationOutcome outcome;
        const bool applied = session.TryGetLastBoardControlOutcome(outcome) &&
            outcome.status == modules::ControlMutationStatus::Applied && history().gameId != before.gameId &&
            history().lineRevision == 1U && history().currentPly == 0U && session.UndoHistoryMoveCount() == 0U &&
            session.GameVariant() == modules::GameVariant::Undecided && session.PlacementModeActive();
        Check(applied, "X+O starts fresh game without phone/archive/save availability and guides opening placement");
        uint32_t beforeNewSave = 0U;
        Check(sync.CopyLinePublication(beforeNewSave, linePublication) && beforeNewSave == playedPublication,
              "immediate live New Game cannot advertise its unsaved replacement as durable");
        checkpoint(applied ? "physical-new-game-applied" : "OLD-archive-blocked", "arrange-opening");
        if (!applied) {
            { std::lock_guard<std::mutex> lock(fixture::mutex); fixture::stopping = true; fixture::changed.notify_all(); }
            fixture::worker.join(); return;
        }
        Check(session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::X, false) == VIRTUAL_LITCH_OK &&
                  session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, runtime::VirtualLitchPhysicalButton::O, false) == VIRTUAL_LITCH_OK &&
                  session.Advance(40U) == VIRTUAL_LITCH_OK && session.Advance(480U) == VIRTUAL_LITCH_OK &&
                  session.EdgeCueIntent().kind != modules::EdgeLedCueKind::Unavailable,
              "release consumes reset chord without an archive warning or negative cue");
        if (scenario == 4U) {
            readbackFault.armed.store(true);
            const uint32_t failuresBefore = worker.GetFailureCount();
            Check(settings.SaveNow(), "healthy owner admits new-game atomic save before storage fails");
            fixture::Wait([&] { return worker.GetFailureCount() >= failuresBefore + 3U && fixture::workerAtWait.load(); });
            Check(session.Advance(1U) == VIRTUAL_LITCH_OK, "cooperative owner observes terminal saving failure");
        }
        const bool savingFailed = scenario == 2U || scenario == 4U;
        if (savingFailed) {
            Check(readbackFault.corruptions.load() != 0U &&
                      worker.GetLastError() == fxe::SettingsStoreError::VerifyFailed &&
                      settings.HasConfirmedStorageFailure() && settings.HasTerminalSaveFailure(),
                  "real scratch-file hash mismatch establishes terminal storage evidence before promotion");
            modules::GamePersistenceState lastGood;
            Check(settings.IsDisabled() && !settings.SaveNow() && disk.Load() &&
                      modules::ReadGamePersistenceState(disk.Doc(), lastGood) &&
                      lastGood.gameHistory.gameId == before.gameId && history().gameId != before.gameId,
                  "terminal save failure preserves last verified disk game and leaves new game playing in RAM");
            checkpoint("new-game-session-only", "continue-playing-without-phone");
            readbackFault.armed.store(false);
            std::filesystem::remove_all(temporary);
            std::filesystem::remove_all(path + ".retry");
        }
        // Scenario zero deliberately plays before the fresh opening saves:
        // the worker must replace the old archive-required line with a decided line too.
        if (scenario != 0U && !savingFailed) save();
        if (scenario == 1U) {
            uint8_t response[20U]{}; size_t size = 0U; uint32_t token = 0U;
            Check(!sync.Peek(response, sizeof(response), size, token, epoch) &&
                      !service.ConsumeArchiveSaveRequest(),
                  "superseded pending archive receipt cannot report Archived or request another save");
        }
        Check(session.ObserveHallMask(OPENING) == VIRTUAL_LITCH_OK && session.Advance(200U) == VIRTUAL_LITCH_OK &&
                  !session.PlacementModeActive(), "arranging opening completes board-only recovery");
        Check(session.ObserveHallMask(OPENING & ~(UINT64_C(1) << 52U)) == VIRTUAL_LITCH_OK &&
                  session.ObserveHallMask(PLAYED) == VIRTUAL_LITCH_OK && session.Advance(2001U) == VIRTUAL_LITCH_OK &&
                  history().currentPly == 1U && session.UndoHistoryMoveCount() == 1U,
              "fresh game accepts actual e2-e4 without requiring app participation");
        const auto newHistory = history();
        modules::GamePersistenceState savedState;
        if (!savingFailed) {
            save();
            Check(disk.Load() && modules::ReadGamePersistenceState(disk.Doc(), savedState) &&
                      modules::current_game_log::ReadCommitPointer(disk.Doc(), durable) && durable.present &&
                      durable.gameId == newHistory.gameId && durable.currentPly == 1U && durable.archiveRequired &&
                      savedState.gameHistory.gameId == newHistory.gameId && savedState.gameHistory.currentPly == 1U,
                  "healthy storage atomically persists new identity and its complete played line");
            uint32_t nextPublication = 0U;
            Check(sync.CopyLinePublication(nextPublication, linePublication) && nextPublication > playedPublication &&
                      linePublication[1U] == 1U && durable.gameId != before.gameId,
                  "New Game replaces the sole public line; notification does not retain the old identity");
        } else {
            Check(!settings.SaveNow() && disk.Load() && modules::ReadGamePersistenceState(disk.Doc(), savedState) &&
                      savedState.gameHistory.gameId == before.gameId,
                  "continued RAM play cannot overwrite protected disk authority after terminal failure");
        }
        checkpoint(savingFailed ? "new-game-play-in-RAM" : "new-game-play-durable",
                   savingFailed ? "reboot-restores-last-saved-game" : "reboot-and-resume-new-game");
        { std::lock_guard<std::mutex> lock(fixture::mutex); fixture::stopping = true; fixture::changed.notify_all(); }
        fixture::worker.join();
        test_support::BoundedHintSession restarted(boardConfig, path.c_str());
        modules::GamePersistenceState rebootState;
        Check(restarted.session.GameVariant() == modules::GameVariant::Classic &&
                  restarted.session.UndoHistoryMoveCount() == 1U &&
                  modules::ReadGamePersistenceState(restarted.session.SettingsForPersistenceTest().Doc(), rebootState) &&
                  rebootState.gameHistory.gameId == (savingFailed ? before.gameId : newHistory.gameId),
              "reboot restores the exact last verified game and its Undo history");
    }
}

void RunFactoryOverridePollJourney(const uint32_t bootAtMs) {
    using Button = modules::UserButton;
    using Mode = firmware::HardwareTestMode;
    using Owner = modules::UiOwner;
    using Stage = modules::ManufacturingTestStage;
    constexpr uint32_t debounceMs = hardware::user_input_timing::BUTTON_DEBOUNCE_MS;
    fxe::SettingsStoreConfig config;
    fxe::SettingsStore source(config);
    firmware::SettingsPersistenceWorker persistence(config);
    FirmwareSystem sys{source, persistence};
    sys.factoryBootOnly = true;
    sys.runtimeHardware = hardware::RuntimeProfileForS3Sample({true, true, false});
    // Hold the existing BLE quiesce gate to observe the pending result before
    // storage starts. The save and reset owners have separate coverage below.
    sys.ble.allowFactoryQuiesce = false;
    firmware_internal::consoleSystem = &sys;
    fixture::now.store(bootAtMs);
    sys.hardwareTest.Enter(sys.runtimeHardware.detectedRevision, bootAtMs);
    bool xDown = false, oDown = false;
    fxe::Button::Config buttonConfig;
    buttonConfig.debounceMs = debounceMs;
    sys.factoryButtons.ConfigureButton(0U, Button::P1X, Owner::P1, buttonConfig, [&] { return xDown; }, true);
    sys.factoryButtons.ConfigureButton(1U, Button::P1O, Owner::P1, buttonConfig, [&] { return oDown; }, true);
    sys.factoryButtons.Begin();
    const auto pollAt = [&](uint32_t nowMs) {
        fixture::now.store(nowMs);
        firmware_internal::LoopFactoryRuntime(sys);
    };
    xDown = true;
    pollAt(bootAtMs + 1U);
    pollAt(bootAtMs + 1U + debounceMs);
    oDown = true;
    const uint32_t oRawAtMs = fixture::now.load() + 1U;
    pollAt(oRawAtMs);
    // Cross a millisecond between loop entry and factory button dispatch as
    // ordinary board work can do. O's debounced edge starts the hold at +1.
    const uint32_t heldAtMs = oRawAtMs + debounceMs;
    sys.ledPowerPollCostMs = 1U;
    pollAt(heldAtMs - 1U);
    sys.ledPowerPollCostMs = 0U;
    Check(sys.hardwareTest.IsActive() && sys.hardwareTest.GetStage() == Stage::Local &&
              sys.hardwareTest.GetOperatorHoldProgress() == 0U,
          "factory X+O starts the hold instead of instantly leaving qualification across a loop millisecond");
    const bool activeAtPress = sys.hardwareTest.IsActive();
    pollAt(heldAtMs + Mode::OVERRIDE_HOLD_MS - 1U);
    Check(sys.hardwareTest.IsActive() && sys.hardwareTest.GetStage() == Stage::Local,
          "factory qualification remains active at 4999ms while both buttons are down");
    const bool activeBeforeThreshold = sys.hardwareTest.IsActive();
    pollAt(heldAtMs + Mode::OVERRIDE_HOLD_MS);
    modules::HardwareTestRecord record;
    Check(!sys.hardwareTest.IsActive() && sys.hardwareTest.IsOverridePending() &&
              sys.hardwareTest.BuildPendingRecord(record) &&
              record.radioResult == modules::ManufacturingTestRadioResult::Override &&
              record.radioChannelMask == 0U,
          "factory X+O requests the unverified Override at 5000ms without waiting for release");
    std::printf("[FACTORY_OVERRIDE_POLL] boot_ms=%u held_ms=%u active_at_press=%u active_at_4999=%u pending_at_5000=%u\n",
                bootAtMs, heldAtMs, activeAtPress, activeBeforeThreshold,
                sys.hardwareTest.IsOverridePending());
    firmware_internal::consoleSystem = nullptr;
}

void Prepare(firmware::HardwareTestMode& mode, uint32_t scanMs, bool bleProof = true) {
    mode.Enter(hardware::BoardRevision::V3S3, fixture::now.load());
    mode.ReportLightSensorLinks(true, true, fixture::now.load());
    mode.ReportAutomatedCheck(modules::ManufacturingTestLocalCheck::MotionSensor, true, fixture::now.load());
    auto tap=[&](modules::UserButton b,modules::UiOwner o){
        mode.OnButtonEvent({b,fxe::Button::Event::Press,o},fixture::now.load());
        mode.OnButtonEvent({b,fxe::Button::Event::Release,o},fixture::now.load());
    };
    for(unsigned channel=0;channel<4U;++channel)tap(modules::UserButton::P1O,modules::UiOwner::P1);
    BoardInput input; input.mask=UINT64_MAX;mode.UpdateSensors(input,fixture::now.load());input.mask=0;mode.UpdateSensors(input,fixture::now.load());
    tap(modules::UserButton::P2O,modules::UiOwner::P2);
    for(auto o:{modules::UiOwner::P1,modules::UiOwner::P2}){
        for(auto b:{modules::UserButton::Up,modules::UserButton::Down,modules::UserButton::Left,modules::UserButton::Right})tap(b,o);
        tap(o==modules::UiOwner::P1?modules::UserButton::P1X:modules::UserButton::P2X,o);
        tap(o==modules::UiOwner::P1?modules::UserButton::P1O:modules::UserButton::P2O,o);
    }
    for (uint32_t lux : {400U, 100U}) {
        for (uint8_t count = 0U; count < 3U; ++count) {
            mode.ObserveLightSample({3U, {lux, lux}}, fixture::now.load());
        }
    }
    mode.UpdateSensors(input,fixture::now.load());
    fixture::now.fetch_add(scanMs);mode.Tick(fixture::now.load());
    if(bleProof){mode.ReportRadioVerified(modules::FactoryRadioChannel::Ble,fixture::now.load());}
    else mode.SetAutomaticVerifier(true,fixture::now.load());
    mode.UpdateSensors(input,fixture::now.load());
    Check(mode.GetStage()==(bleProof?modules::ManufacturingTestStage::Persisting:modules::ManufacturingTestStage::Radio),"production navigation/local evidence cannot submit without BLE proof");
}
}

#include "test_runtime_storage_journey.hpp"

int main() {
    fxe::ScopedMillisProviderOverride clock(&fixture::Millis);
    RunFactoryOverridePollJourney(1000U);
    RunFactoryOverridePollJourney(UINT32_MAX - 2500U);
    fixture::now.store(0U);
    const char* path = "build/t_factory_receipt.json";
    std::remove(path); std::remove("build/t_factory_receipt.json.bak"); std::remove("build/t_factory_receipt.json.tmp");
    fxe::SettingsStoreConfig config; config.path = path;
    fxe::SettingsStore source(config);
    source.Load();
    modules::HardwareTestRecord prior{modules::HARDWARE_TEST_RECORD_VERSION, hardware::BoardRevision::V2S3,
        modules::ManufacturingTestRadioResult::Bypassed, true, 0U, 1U, 2U, 0U};
    modules::WriteHardwareTestRecord(source.Doc(), prior);
    Check(source.SaveNowBlocking(), "prior record has real durable baseline");
    hardware::PersistenceTestArbiter arbiter;
    firmware::SettingsPersistenceWorker persistence(config);
    modules::BoardIdentity expectedIdentity; expectedIdentity.bytes.fill(0x41U);
    Check(modules::WriteBoardIdentity(source.Doc(), expectedIdentity),
          "install stable identity before production configuration");
    FirmwareSystem sys{source, persistence};
    firmware_internal::ConfigureBoardIdentityPersistence(sys);
    SnapshotTransactionProbe transaction;
    transaction.worker = &persistence;
    Check(persistence.SetSnapshotTransactionCallbacks(
              &SnapshotTransactionProbe::Prepare,
              &SnapshotTransactionProbe::Complete, &transaction),
          "one current-line transaction owns the settings worker hook");
    Check(!persistence.SetSnapshotTransactionCallbacks(
              &SnapshotTransactionProbe::Prepare,
              &SnapshotTransactionProbe::Complete, &transaction) &&
          !persistence.SetSnapshotTransactionCallbacks(
              nullptr, &SnapshotTransactionProbe::Complete, &transaction) &&
          !persistence.SetSnapshotTransactionCallbacks(
              &SnapshotTransactionProbe::Prepare, nullptr, &transaction),
          "duplicate or incomplete transaction callbacks preserve the first owner");
    std::atomic<unsigned> rejectedCallbacks{0U};
    const auto rejectedObserver = [](void* context, const JsonDocument&) {
        static_cast<std::atomic<unsigned>*>(context)->fetch_add(1U);
    };
    Check(!persistence.SetDurableSnapshotCallback(rejectedObserver, &rejectedCallbacks) &&
          !persistence.SetDurableSnapshotCallback(nullptr, nullptr),
          "duplicate and removal attempts preserve the production identity observer");
    firmware::SettingsPersistenceWorker initiallyNull(config);
    Check(!initiallyNull.SetDurableSnapshotCallback(nullptr, &rejectedCallbacks) &&
          initiallyNull.SetDurableSnapshotCallback(rejectedObserver, &rejectedCallbacks),
          "null initial observer is rejected without consuming the callback slot");
    firmware::SettingsPersistenceWorker nullContext(config);
    Check(nullContext.SetDurableSnapshotCallback(rejectedObserver, nullptr),
          "a non-null observer with null context is a valid first assignment");
    fixture::failNextTaskCreate.store(true);
    Check(!(persistence.SetStorageAccessTestHook(&hardware::PersistenceTestArbiter::ObserveStorage, nullptr), persistence.Begin(source)) && !persistence.IsStarted(),
          "task creation failure leaves the worker available for retry");
    Check((persistence.SetStorageAccessTestHook(&hardware::PersistenceTestArbiter::ObserveStorage, nullptr), persistence.Begin(source)), "actual persistence worker starts");
    Check(!persistence.SetDurableSnapshotCallback(nullptr,nullptr),
          "live observer replacement cannot race a storage callback");
    auto promoteIdentitySnapshot = [&](bool expectedDurable) {
        const auto completed = persistence.GetCompletedSaveCount();
        arbiter.ResetPause();
        Check(persistence.Enqueue(source.Doc()), "actual worker accepts identity snapshot");
        fixture::Wait([] { return fixture::blocked.load(); });
        Check(!sys.boardIdentity.IsDurable(), "queued and paused identity is not durable");
        fixture::hold.store(false); fixture::changed.notify_all();
        fixture::Wait([&] { return persistence.GetCompletedSaveCount() > completed && fixture::workerAtWait.load(); });
        Check(sys.boardIdentity.IsDurable() == expectedDurable, "actual promotion callback gates exact identity");
        fxe::SettingsStore transactionDisk(config);
        Check(transactionDisk.Load() &&
                  transactionDisk.ReadValue(
                      SnapshotTransactionProbe::KEY, 0U) ==
                      transaction.lastPrepared.load(),
              "prepared current-line marker reaches the same durable settings snapshot");
    };
    // A matching pending snapshot can be coalesced away by a later unrelated one.
    const auto beforeCoalescing = persistence.GetCompletedSaveCount();
    const unsigned preparesBeforeCoalescing = transaction.prepareCalls.load();
    const unsigned resultsBeforeCoalescing = transaction.resultCalls.load();
    arbiter.ResetPause();
    Check(persistence.Enqueue(source.Doc()), "enqueue unrelated in-flight snapshot");
    fixture::Wait([] { return fixture::blocked.load(); });
    Check(modules::WriteBoardIdentity(source.Doc(), expectedIdentity) && persistence.Enqueue(source.Doc()),
          "queue matching identity behind paused write");
    source.Doc().remove(modules::KEY_BOARD_IDENTITY_V1);
    Check(persistence.Enqueue(source.Doc()) && !sys.boardIdentity.IsDurable(),
          "later unrelated snapshot coalesces pending identity without acknowledging it");
    fixture::hold.store(false); fixture::changed.notify_all();
    fixture::Wait([&] { return persistence.GetCompletedSaveCount() >= beforeCoalescing + 2U && fixture::workerAtWait.load(); });
    Check(!sys.boardIdentity.IsDurable(), "coalesced-away identity never becomes durable");
    Check(transaction.prepareCalls.load() == preparesBeforeCoalescing + 2U &&
              transaction.resultCalls.load() == resultsBeforeCoalescing + 2U,
          "only the in-flight and final coalesced snapshots run transactions");
    promoteIdentitySnapshot(false); // An unrelated successful save is insufficient.
    Check(modules::WriteBoardIdentity(source.Doc(), expectedIdentity), "install matching identity snapshot");
    promoteIdentitySnapshot(true);
    Check(!sys.boardIdentity.Prepare(source.Doc(), nullptr, nullptr) && sys.boardIdentity.IsDurable(),
          "second Prepare cannot mutate a live identity or readiness");
    auto replacementIdentity = expectedIdentity; replacementIdentity.bytes[0] = 0x42U;
    Check(modules::WriteBoardIdentity(source.Doc(), replacementIdentity), "prepare replacement snapshot");
    const auto replacementBefore = persistence.GetCompletedSaveCount();
    Check(persistence.Enqueue(source.Doc()), "enqueue replacement through real owner");
    fixture::Wait([&] { return persistence.GetCompletedSaveCount() > replacementBefore && fixture::workerAtWait.load(); });
    Check(!sys.boardIdentity.IsDurable(), "replacement promotion revokes matching identity readiness");
    firmware_internal::consoleSystem = &sys;
    test_support::OwnerPairingConsoleTransport transport;
    std::string consoleOutput;
    Console console(transport.hybrid);
    bool commandAccepted = false;
    console.RegisterCommand("hwtest", "factory test", [&](const Console::Args& args) {
        commandAccepted = firmware_internal::console_commands::HardwareTestCommand(console, args);
        return commandAccepted;
    });
    console.RegisterCommand("factory", "factory clear", [&](const Console::Args& args) {
        commandAccepted = firmware_internal::console_commands::FactoryCommand(console, args);
        return commandAccepted;
    });
    auto command = [&](const char* line, const bool handoffAdmitted = true,
                       const bool remote = false) {
        firmware_internal::factoryBootHandoffAdmitted = handoffAdmitted;
        firmware_internal::attemptedFactoryBoots = 0U;
        // Each command has a fresh external reset-request observation. A
        // rejected command cannot inherit a previously accepted intent.
        firmware_internal::requestedFactoryBoots = 0U;
        firmware_internal::requestedFactoryBootIntent = firmware::FactoryBootIntent::None;
        consoleOutput.clear();
        transport.usb.output.clear();
        (remote ? transport.ble.input : transport.usb.input) += std::string(line) + '\n';
        console.Update();
        consoleOutput = transport.usb.output;
        return commandAccepted;
    };
    // Preserve the actual unsupported strap -> ordinary V2 fallback conversion
    // that the old literal-Unknown fixture skipped. Console/runtime execute
    // unchanged; only GPIO sampling and the radio/NVS SDK remain controlled.
    for (const bool unknown : {true, false}) {
        sys.runtimeHardware = hardware::RuntimeProfileForS3Sample(
            {false, unknown, false}); // GPIO35/37 high; GPIO36 low is raw 2.
        Check(sys.runtimeHardware.resolved.revision == hardware::BoardRevision::V2S3 &&
                  sys.runtimeHardware.detectedRevision == (unknown
                      ? hardware::BoardRevision::Unknown : hardware::BoardRevision::V2S3),
              "raw strap retains detected identity and ordinary V2 hardware fallback");
        Check(command("hwtest status") && consoleOutput.find(unknown
                  ? "revision=0 " : "revision=2 ") != std::string::npos,
              "factory status reports detected identity instead of ordinary fallback");
        for (const char* start : {"hwtest start", "hwtest restart"}) {
            const std::string beforeStart = source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, "");
            const bool accepted = command(start);
            Check(beforeStart == source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""),
                  "entry admission leaves saved result bytes unchanged");
            Check(accepted == !unknown && !sys.hardwareTest.IsActive() &&
                      !sys.ble.enabled &&
                      firmware_internal::requestedFactoryBoots == (unknown ? 0U : 1U) &&
                      firmware_internal::requestedFactoryBootIntent == (unknown
                          ? firmware::FactoryBootIntent::None : firmware::FactoryBootIntent::Test),
                  "known Test admission requests clean boot; Unknown has no mutation or intent");
            if (unknown) {
                Check(consoleOutput.find("unsupported on this hardware revision") != std::string::npos,
                      "Unknown Console rejection explains unsupported factory identity");
            }
            sys.hardwareTest.Stop();
        }
        auto eligible = prior;
        eligible.radioResult = modules::ManufacturingTestRadioResult::Verified;
        eligible.radioChannelMask = modules::FACTORY_RADIO_ALL_CHANNELS;
        Check(modules::WriteHardwareTestRecord(source.Doc(), eligible),
              "seed eligible saved V2 evidence at the real runtime boundary");
        sys.ble.enabled = true;
        firmware_internal::UpdateHardwareTestRadio(sys, fixture::now.load());
        Check(!sys.ble.enabled,
              "historical verifier flag never starts service on normal boot");
        const std::string beforeVerifier = source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, "");
        Check(command("hwtest verifier on") == !unknown && !sys.ble.enabled &&
                  !sys.hardwareTest.IsActive() &&
                  firmware_internal::requestedFactoryBoots == (unknown ? 0U : 1U) &&
                  firmware_internal::requestedFactoryBootIntent == (unknown
                      ? firmware::FactoryBootIntent::None : firmware::FactoryBootIntent::Verifier),
              "known Verifier admission requests clean boot; Unknown has no intent or normal radio");
        Check(!unknown || beforeVerifier == source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""),
              "Unknown verifier request cannot mutate historical result bytes");
        // Bypass entry deliberately to prove the service fence itself, even
        // when a stale run was configured with the ordinary fallback identity.
        sys.hardwareTest.Enter(sys.runtimeHardware.resolved.revision, fixture::now.load());
        firmware_internal::UpdateHardwareTestRadio(sys, fixture::now.load());
        Check(!sys.factoryRadio.radioActive,
              "local review does not admit radio even on known hardware");
        Prepare(sys.hardwareTest, 3000U, false);
        firmware_internal::UpdateHardwareTestRadio(sys, fixture::now.load());
        Check(sys.factoryRadio.radioActive == !unknown,
              "actual Radio-stage admission uses detected identity rather than fallback");
        sys.hardwareTest.Stop();
        if (unknown) {
            Check(modules::WriteHardwareTestRecord(source.Doc(), prior), "seed historical V2 verifier record");
            sys.factoryRadio.bleProof = true;
            firmware_internal::UpdateHardwareTestRadio(sys, fixture::now.load());
            Check(!firmware_internal::IsHardwareTestSavePending(),
                  "Unknown stale terminal proof cannot submit a historical factory upgrade");
            if (firmware_internal::IsHardwareTestSavePending()) {
                fixture::Wait([&] { return persistence.GetDiagnostics().outcome !=
                    firmware::SettingsPersistenceWorker::SaveOutcome::Pending && fixture::workerAtWait.load(); });
                firmware_internal::UpdateHardwareTestPersistence(sys);
            }
        }
        factory_provisioning_sdk::Reset();
        firmware::PersistenceFixtureProvisioning detectedProvisioning;
        detectedProvisioning.Begin(sys.runtimeHardware.detectedRevision, source.Doc(), arbiter);
        sys.factoryProvisioning.actual = &detectedProvisioning;
        const unsigned writesBeforeClear = factory_provisioning_sdk::writes;
        const bool clearAccepted = command("factory clear");
        Check(clearAccepted == !unknown,
              "actual factory clear admits known V2 and rejects Unknown fallback");
        if (clearAccepted) {
            Check(arbiter.AdvanceRender(), "clear prerequisite receives its requested render generation");
            fixture::Wait([&] { return persistence.GetDiagnostics().outcome !=
                firmware::SettingsPersistenceWorker::SaveOutcome::Pending && fixture::workerAtWait.load(); });
            firmware_internal::UpdateHardwareTestPersistence(sys);
        }
        Check(factory_provisioning_sdk::writes == writesBeforeClear + (unknown ? 0U : 1U),
              "Unknown clear rejection performs no factory NVS mutation; known V2 commits Required");
        std::printf("[DETECTED_FACTORY_ADMISSION] raw=%u detected=%u resolved=%u clear=%u nvs_writes=%u\n",
                    unknown ? 2U : 0U, unsigned(sys.runtimeHardware.detectedRevision),
                    unsigned(sys.runtimeHardware.resolved.revision), clearAccepted,
                    factory_provisioning_sdk::writes - writesBeforeClear);
        sys.factoryProvisioning.actual = nullptr;
        sys.ble.enabled = false;
        Check(modules::WriteHardwareTestRecord(source.Doc(), prior), "restore original fixture record");
    }
    sys.runtimeHardware = hardware::RuntimeProfileForS3Sample({true, false, false});
    Check(command("hwtest status", true, true), "Bluetooth retains factory status");
    Check(consoleOutput.find(
              "provisioning_state=legacy_exempt completed_result=allowed next=not_required") !=
              std::string::npos,
          "status exposes persistent provisioning disposition and next action");
    Check(!command("hwtest forcepass"), "retired forcepass command cannot manufacture PASS");
    Check(!command("hwtest forceexit") && !sys.hardwareTest.IsActive(), "inactive run cannot request override");
    Check(command("hwtest restart") && !sys.hardwareTest.IsActive() &&
              firmware_internal::requestedFactoryBootIntent == firmware::FactoryBootIntent::Test,
          "actual Console requests factory boot without in-place initialization");
    SelectFixtureBootProfile(sys, firmware_internal::requestedFactoryBootIntent);
    Check(sys.factoryBootOnly && !sys.factoryVerifierBoot,
          "controlled software reset selects the production factory profile");
    sys.hardwareTest.Enter(sys.runtimeHardware.detectedRevision, fixture::now.load());
    Check(command("hwtest fault light1", true, true) &&
              sys.hardwareTest.GetProgress().diagnosticFaultMask == 0x20U &&
              sys.hardwareTest.IsActive(),
          "Bluetooth Console admits diagnostic faults during an active unsaved run");
    Check(command("hwtest fault clear", true, true) &&
              sys.hardwareTest.GetProgress().diagnosticFaultMask == 0U,
          "Bluetooth Console clears its diagnostic fault before the real sensor attempt");
    const uint8_t lightFailureBit = 1U << static_cast<uint8_t>(modules::ManufacturingTestLocalCheck::LightSensors);
    const std::string beforeFault = source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, "");
    Check(command("hwtest fault light1") && command("hwtest fault clear") &&
              (sys.hardwareTest.GetProgress().failedAutomatedCheckMask & lightFailureBit) == 0U,
          "ALS fault/clear before the first real attempt retains unknown without fabricated failure or pass");
    sys.hardwareTest.ReportLightSensorLinks(true, true, fixture::now.load());
    Check(command("hwtest fault light1") && sys.hardwareTest.GetProgress().diagnosticFaultMask == 0x20U &&
              sys.hardwareTest.GetProgress().lightSensorLinkMask == 0x01U &&
              (sys.hardwareTest.GetProgress().failedAutomatedCheckMask & lightFailureBit) != 0U,
          "actual Console light1 fault removes physical A1 driver1 while retaining H8 driver0");
    Check(command("hwtest fault clear") && command("hwtest fault light2") &&
              sys.hardwareTest.GetProgress().diagnosticFaultMask == 0x40U &&
              sys.hardwareTest.GetProgress().lightSensorLinkMask == 0x02U,
          "actual Console light2 fault removes physical H8 driver0 while retaining A1 driver1");
    sys.hardwareTest.ReportLightSensorLinks(false, true, fixture::now.load());
    Check(command("hwtest fault clear") && sys.hardwareTest.GetProgress().lightSensorLinkMask == 0x02U &&
              (sys.hardwareTest.GetProgress().failedAutomatedCheckMask & lightFailureBit) != 0U &&
              beforeFault == source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""),
          "clear restores raw missing-H8 evidence and cannot fabricate healthy links or alter saved result");
    sys.hardwareTest.ReportLightSensorLinks(true, true, fixture::now.load());
    Check(!command("hwtest fault light3") && !command("hwtest fault light1 extra"),
          "unknown ALS target and extra arguments are rejected by actual Console parser");
    Check(!command("hwtest forceexit extra"), "extra forceexit arguments rejected before mutation");
    Check(command("hwtest forceexit") && !sys.hardwareTest.IsActive() && sys.hardwareTest.IsOverridePending(), "real Console exits before saving or release, preserving pending override");
    Check(!command("hwtest fault light1"), "pending result rejects ALS diagnostic mutation");
    const uint32_t beforeOverride = persistence.GetCompletedSaveCount();
    arbiter.ResetPause(); firmware_internal::UpdateHardwareTestPersistence(sys);
    fixture::Wait([] { return fixture::blocked.load(); });
    Check(sys.factoryProvisioning.expectedReceipt.load() != 0U,
          "tracked generation is published before the actual worker can finish");
    Check(firmware_internal::IsHardwareTestSavePending() && !command("hwtest restart") && !command("hwtest forceexit"), "override receipt remains owned after exit and cannot be discarded by restart");
    fixture::hold.store(false); fixture::changed.notify_all();
    fixture::Wait([&] { return persistence.GetCompletedSaveCount()>beforeOverride && fixture::workerAtWait.load(); });
    Check(sys.factoryProvisioning.trackedFactorySnapshots == 1U &&
              persistence.CurrentDurableSnapshotReceipt() == 0U,
          "actual durable observer receives the exact published receipt only during its callback");
    firmware_internal::UpdateHardwareTestPersistence(sys);
    modules::HardwareTestRecord overridden;
    Check(sys.hardwareTest.GetStage()==modules::ManufacturingTestStage::OverrideSaved && !sys.hardwareTest.IsActive() &&
          modules::DecodeHardwareTestRecord(source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY,""),overridden) && overridden.radioResult==modules::ManufacturingTestRadioResult::Override && !modules::IsVerifiedFactoryPass(overridden),
          "actual worker/SettingsStore adoption saves distinct Override without live PASS");
    sys.ble.enabled=true;firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
    Check(!sys.ble.enabled && !sys.factoryRadio.radioActive,"saved override cannot enable radio verifier");
    const auto reseeds=sys.reseeds;Check(command("hwtest stop")&&sys.reseeds==reseeds+1U&&sys.queueClears==sys.reseeds,"actual stop and override use GPIO reseed and queue cleanup");
    sys.comptest.active=true;Check(command("hwtest status")&&!command("hwtest start"),"comptest permits status but blocks competing mutation");sys.comptest.active=false;
    std::puts("[MANUFACTURING_SAVE_JOURNEY] console_override=true immediate_exit=true receipt_after_exit=true record=Override verified_pass=false");
    for (unsigned action : {0U, 1U, 2U, 3U}) {
        // 0 completes; 1 restarts; 2 stops; 3 stops and changes verifier intent.
        const uint32_t countBefore = persistence.GetCompletedSaveCount();
        arbiter.ResetPause();
        const bool delayedResponder = action == 1U || action == 2U;
        Prepare(sys.hardwareTest, 3000U + action * 1000U, !delayedResponder);
        if (delayedResponder) {
            sys.hardwareTest.ReportRadioVerified(modules::FactoryRadioChannel::Ble, fixture::now.load());
            BoardInput empty; empty.mask = 0ULL;
            sys.hardwareTest.UpdateSensors(empty, fixture::now.load());
        }
        modules::HardwareTestRecord candidate; sys.hardwareTest.BuildPendingRecord(candidate);
        modules::EncodedHardwareTestRecord original;
        Check(original.Assign(source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, "")), "prior record fits bounded evidence");
        const uint32_t requestedAt = fixture::now.load();
        firmware_internal::UpdateHardwareTestPersistence(sys);
        Check(fixture::now.load() == requestedAt, "enqueue returns without a three-second cooperative wait");
        fixture::Wait([] { return fixture::blocked.load(); });
        fixture::now.fetch_add(3001U);
        firmware_internal::UpdateHardwareTestPersistence(sys);
        Check(sys.hardwareTest.GetStage() == modules::ManufacturingTestStage::Persisting,
              "caller deadline is pending, not terminal save failure or PASS");
        Check(original.View() == source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""),
              "pending receipt exposes the previous known record without claiming rollback");
        source.Doc()["FACTORY_FIXTURE_LATER_SETTING"] = action + 1U;
        source.MarkMustSaveDirty();
        if (action == 1U) { source.ReportAsyncSaveFailure(fxe::SettingsStoreError::VerifyFailed); }
        const auto priorError = source.GetLastError();
        const auto priorErrors = source.GetErrorCount();
        const auto priorSaves = source.GetSaveCount();
        Check(!source.SaveNow(), "temporary receipt deferral cannot fabricate queue acceptance");
        for (unsigned cadence = 0U; cadence < 5U; ++cadence) {
            fixture::now.fetch_add(101U); source.Update();
        }
        Check(source.GetErrorCount() == priorErrors && source.GetLastError() == priorError,
              "actual SettingsStore SaveNow/Update deferral preserves prior error state without false failures");
        Check(source.GetSaveCount() == priorSaves && source.ArmPersistenceGuardIfSavePending(),
              "deferred source requests remain pending without fabricated durability");
        Check(!persistence.IsIdle() && !persistence.ReleaseSaveReceipt(0U),
              "a pending retained receipt cannot be released or mistaken for an idle drain");
        if (action != 0U) { sys.hardwareTest.Stop(); }
        if (action == 1U) { sys.hardwareTest.Enter(hardware::BoardRevision::V3S3, fixture::now.load()); }
        if (action == 3U) {
            auto later = candidate; later.factoryVerifierEnabled = false; later.operatorDurationMs = 99U;
            modules::WriteHardwareTestRecord(source.Doc(), later);
            source.MarkMustSaveDirty();
        }
        fixture::hold.store(false); fixture::changed.notify_all();
        fixture::Wait([&] { return persistence.GetCompletedSaveCount() > countBefore && fixture::workerAtWait.load(); });
        // The production radio tick reconciles even after Stop disables the sensor cadence.
        firmware_internal::UpdateHardwareTestRadio(sys, fixture::now.load());
        Check(action == 0U ? sys.hardwareTest.GetStage() == modules::ManufacturingTestStage::Passed
                           : sys.hardwareTest.GetStage() != modules::ManufacturingTestStage::Passed,
              "late durable completion acknowledges only its originating active run");
        Check(action != 1U || sys.hardwareTest.GetStage() == modules::ManufacturingTestStage::Local,
              "restart remains in the replacement run's local evidence stage");
        Check(!delayedResponder || (!sys.factoryVerifierBoot && !sys.ble.enabled),
              "stopped/replaced responder receipt cannot grant the current run saved-service authority");
        modules::HardwareTestRecord reconciled;
        Check(modules::DecodeHardwareTestRecord(source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""), reconciled) &&
              reconciled.operatorDurationMs == (action == 3U ? 99U : candidate.operatorDurationMs),
              "late success reconciles the record while preserving a later explicit record mutation");
        fxe::SettingsStore loaded(config); Check(loaded.Load(), "actual delayed transaction reloads");
        modules::HardwareTestRecord disk;
        Check(modules::DecodeHardwareTestRecord(loaded.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""), disk) &&
              disk.operatorDurationMs == candidate.operatorDurationMs &&
              disk.factoryVerifierEnabled == delayedResponder,
              "accepted factory write preserves genuine role evidence after stop without granting live service");
        fixture::now.fetch_add(101U); source.Update();
        fixture::Wait([&] { return persistence.GetCompletedSaveCount() >= countBefore + 2U && fixture::workerAtWait.load(); });
        fxe::SettingsStore latest(config); Check(latest.Load() && latest.ReadValue("FACTORY_FIXTURE_LATER_SETTING", 0U) == action + 1U,
              "later settings changes survive receipt serialization and normal deferred-owner saving");
        std::printf("[MANUFACTURING_SAVE_JOURNEY] delayed=true action=%u active=%u stage=%u completed=%u disk_radio=%u next=%s\n",
                    action, sys.hardwareTest.IsActive(), static_cast<unsigned>(sys.hardwareTest.GetStage()),
                    persistence.GetCompletedSaveCount(), static_cast<unsigned>(disk.radioResult),
                    action == 0U ? "mark-board-complete" : "finish-new-run-or-inspect-last-record");
    }
    factory_provisioning_sdk::Reset();
    modules::EncodeFactoryProvisioningRecord(modules::FactoryProvisioningState::Required,
        factory_provisioning_sdk::committed);
    firmware::PersistenceFixtureProvisioning actualProvisioning;
    actualProvisioning.Begin(hardware::BoardRevision::V3S3, source.Doc(), arbiter);
    sys.factoryProvisioning.actual = &actualProvisioning;
    SelectFixtureBootProfile(sys, firmware::FactoryBootIntent::Test);
    const uint32_t beforePeer = persistence.GetCompletedSaveCount();
    Prepare(sys.hardwareTest,3000U,false);firmware_internal::UpdateHardwareTestPersistence(sys);
    Check(!firmware_internal::IsHardwareTestSavePending() && persistence.GetCompletedSaveCount()==beforePeer,"no peer creates no local-only/bypass storage transaction");
    sys.hardwareTest.ReportRadioVerified(modules::FactoryRadioChannel::Ble,fixture::now.load());
    Check(sys.hardwareTest.GetStage()==modules::ManufacturingTestStage::Radio,"BLE proof waits for a fresh current empty Hall before submitting factory result");
    BoardInput finalInput;finalInput.mask=0ULL;sys.hardwareTest.UpdateSensors(finalInput,fixture::now.load());
    arbiter.ResetPause();firmware_internal::UpdateHardwareTestPersistence(sys);fixture::Wait([] { return fixture::blocked.load(); });
    Check(sys.hardwareTest.GetStage()==modules::ManufacturingTestStage::Persisting,"BLE peer result waits for its own real receipt");
    fixture::hold.store(false);fixture::changed.notify_all();
    fixture::Wait([&]{return persistence.GetCompletedSaveCount()>beforePeer&&fixture::workerAtWait.load();});firmware_internal::UpdateHardwareTestPersistence(sys);
    modules::HardwareTestRecord peer;
    Check(sys.hardwareTest.GetStage()==modules::ManufacturingTestStage::Passed&&modules::DecodeHardwareTestRecord(source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY,""),peer)&&peer.radioResult==modules::ManufacturingTestRadioResult::PeerVerified,"one exact new receipt saves actual verifier proof before PASS");
    Check(actualProvisioning.State() == modules::FactoryProvisioningState::Passed &&
              peer.factoryVerifierEnabled && sys.factoryVerifierBoot && !sys.hardwareTest.IsActive() &&
              !sys.factoryCompletion.IsPending() && !sys.factoryRadioReleasedForSave && sys.ble.enabled,
          "exact adopted responder receipt/NVS Passed retains existing minimal saved service without reboot");
    Check(!command("hwtest fault light2"), "saved verifier rejects ALS diagnostic mutation");
    fixture::now.fetch_add(5000U);
    firmware_internal::UpdateHardwareTestRadio(sys, fixture::now.load());
    Check(sys.factoryVerifierBoot && sys.ble.enabled && !sys.hardwareTest.TakeCompletedBatchExit() &&
              !sys.factoryCompletion.IsPending(),
          "saved projection does not turn Stop's active-PASS artifact into immediate endpoint withdrawal");
    std::puts("[FACTORY_VERIFIER_RETENTION] originating_receipt=adopted NVS=Passed saved_verifier=1 minimal=1 active_test=0 reboot=0 serving_after_5000ms=1");
    sys.hardwareTest.Stop();
    SelectFixtureBootProfile(sys, firmware::FactoryBootIntent::Test);
    for (unsigned commandCase : {0U, 1U, 2U}) {
        // 0: off with no prior record. 1: off when prior bytes already say off.
        // 2: ordinary Stop is not an explicit verifier-off command.
        if (commandCase == 1U) {
            auto disabled = prior; disabled.factoryVerifierEnabled = false;
            modules::WriteHardwareTestRecord(source.Doc(), disabled);
        } else { source.Doc().remove(modules::HARDWARE_TEST_RECORD_SETTING_KEY); }
        source.NotifyDocChanged();
        const uint32_t baselineSaves = persistence.GetCompletedSaveCount();
        Check(source.SaveNow(), "command case establishes its durable prior-record condition");
        fixture::Wait([&] { return persistence.GetCompletedSaveCount() > baselineSaves && fixture::workerAtWait.load(); });
        arbiter.ResetPause();
        Prepare(sys.hardwareTest, 3000U, true);
        const uint32_t candidateSaves = persistence.GetCompletedSaveCount();
        firmware_internal::UpdateHardwareTestPersistence(sys);
        fixture::Wait([] { return fixture::blocked.load(); });
        modules::EncodedHardwareTestRecord beforeCommand;
        Check(beforeCommand.Assign(source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, "")), "command prior record is bounded");
        const char* commandLine = commandCase == 2U ? "hwtest stop" : "hwtest verifier off";
        const bool stopping = commandCase == 2U;
        Check(firmware_internal::IsHardwareTestSavePending(), "busy off/stop case owns its real tracked receipt");
        Check(command(commandLine, false) == stopping &&
                  firmware_internal::attemptedFactoryBoots == (stopping ? 0U : 1U) &&
                  firmware_internal::requestedFactoryBoots == 0U &&
                  firmware_internal::requestedFactoryBootIntent == firmware::FactoryBootIntent::None &&
                  sys.pendingFactoryBootIntent == firmware::FactoryBootIntent::None &&
                  !sys.rebootPersistencePending,
              "pending off handoff is rejected without applied boot intent; Stop remains a local action");
        Check(beforeCommand.View() == source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""),
              "actual off/no-prior and off/already-off commands leave record bytes unchanged");
        fixture::hold.store(false); fixture::changed.notify_all();
        fixture::Wait([&] { return persistence.GetCompletedSaveCount() > candidateSaves && fixture::workerAtWait.load(); });
        firmware_internal::UpdateHardwareTestRadio(sys, fixture::now.load());
        modules::HardwareTestRecord afterCommand;
        const bool shouldEnable = false; // Every genuine new result exits to normal play.
        Check(modules::DecodeHardwareTestRecord(source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""), afterCommand) &&
              afterCommand.factoryVerifierEnabled == shouldEnable && sys.ble.enabled == shouldEnable,
              "explicit off and ordinary Stop both retain the new completion's verifier-off record");
        if (!shouldEnable) {
            const uint32_t intentSaves = persistence.GetCompletedSaveCount();
            fixture::now.fetch_add(101U); source.Update();
            // Wait only if this request was accepted; old RED keeps no dirty
            // off intent, so it must fail an assertion instead of hanging.
            if (!persistence.IsIdle()) {
                fixture::Wait([&] { return persistence.GetCompletedSaveCount() > intentSaves && fixture::workerAtWait.load(); });
            }
            fxe::SettingsStore restored(config); restored.Load();
            modules::HardwareTestRecord durable;
            Check(modules::DecodeHardwareTestRecord(restored.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""), durable) &&
                  !durable.factoryVerifierEnabled, "explicit off intent eventually persists through the actual settings owner");
        }
        std::printf("[MANUFACTURING_COMMAND_JOURNEY] case=%u command=%s active=%u verifier=%u next=inspect-record\n",
                    commandCase, commandCase == 2U ? "stop" : "verifier-off", sys.hardwareTest.IsActive(), sys.ble.enabled);
    }
    // A historical Verified record is retained on normal boot without radio.
    // Explicit off/on persists intent, then a controlled reset selects the
    // production normal/minimal-verifier profile; no in-place activation.
    sys.hardwareTest.Stop();
    modules::HardwareTestRecord verified{1U,hardware::BoardRevision::V3S3,
        modules::ManufacturingTestRadioResult::Verified,true,3U,1U,2U,3U};
    modules::WriteHardwareTestRecord(source.Doc(),verified);source.MarkMustSaveDirty();
    fixture::now.fetch_add(101U);source.Update();
    fixture::Wait([&]{return persistence.IsIdle()&&fixture::workerAtWait.load();});
    factory_provisioning_sdk::Reset();
    actualProvisioning.Begin(hardware::BoardRevision::V3S3,source.Doc(),arbiter);
    sys.factoryProvisioning.actual=&actualProvisioning;
    SelectFixtureBootProfile(sys, firmware::FactoryBootIntent::None);
    firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
    Check(!sys.factoryBootOnly && !sys.ble.enabled,
          "historical verified verifier-on record stays radio-free in normal profile");
    for(bool enable:{false,true}){
        const uint32_t completed=persistence.GetCompletedSaveCount();
        Check(command(enable ? "hwtest verifier on" : "hwtest verifier off"),"inactive Verified DUT Console eligibility command accepted");
        Check(consoleOutput.find(enable ? "verifier=on" : "verifier=off")!=std::string::npos,"Console reports accepted verifier intent truthfully");
        firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
        Check(!sys.ble.enabled && !sys.hardwareTest.IsActive() &&
                  firmware_internal::requestedFactoryBoots == 1U &&
                  firmware_internal::requestedFactoryBootIntent == (enable
                      ? firmware::FactoryBootIntent::Verifier : firmware::FactoryBootIntent::None),
              "explicit saved verifier intent requests clean boot without in-place BLE");
        fixture::now.fetch_add(101U);source.Update();
        fixture::Wait([&]{return persistence.IsIdle()&&fixture::workerAtWait.load();});
        fxe::SettingsStore restored(config);modules::HardwareTestRecord restoredRecord;
        Check(restored.Load()&&modules::DecodeHardwareTestRecord(restored.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY,""),restoredRecord)&&
            restoredRecord.factoryVerifierEnabled==enable&&modules::IsVerifiedFactoryPass(restoredRecord)&&
            actualProvisioning.State()==modules::FactoryProvisioningState::Passed&&persistence.GetCompletedSaveCount()>completed,
            "Verified DUT intent persists and restores without removing genuine PASS");
        SelectFixtureBootProfile(sys, firmware_internal::requestedFactoryBootIntent);
        firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
        Check(sys.factoryVerifierBoot == enable && sys.factoryBootOnly == enable &&
                  sys.ble.enabled == enable,
              "controlled reset selects saved verifier essentials or normal play from production profile");
    }
    SelectFixtureBootProfile(sys, firmware::FactoryBootIntent::None);
    for(auto result:{modules::ManufacturingTestRadioResult::Override,modules::ManufacturingTestRadioResult::Forced}){
        auto excluded=verified;excluded.radioResult=result;excluded.radioChannelMask=0U;excluded.factoryVerifierEnabled=false;
        modules::WriteHardwareTestRecord(source.Doc(),excluded);
        Check(!command("hwtest verifier on"),"Override and Forced cannot become eligible verifier records");
        firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
        Check(!sys.ble.enabled,"excluded record cannot reopen inactive service");
    }
    source.Doc().remove(modules::HARDWARE_TEST_RECORD_SETTING_KEY);
    Check(!command("hwtest verifier on"),"missing/unverified record cannot enable verifier");

    // Reproduce a durable Required boundary with surviving historical Bypassed
    // evidence and a retained unrelated receipt that rejects narrow removal.
    auto historical=verified;historical.radioResult=modules::ManufacturingTestRadioResult::Bypassed;historical.radioChannelMask=0U;
    modules::WriteHardwareTestRecord(source.Doc(),historical);
    source.Doc()["FACTORY_CLEAR_USER_SENTINEL"]="keep-user-data";
    source.NotifyDocChanged();Check(source.SaveNow(),"historical bootstrap and user sentinel saved before explicit clear");
    fixture::Wait([&]{return persistence.IsIdle()&&fixture::workerAtWait.load();});
    fixture::hold.store(false);
    Check(actualProvisioning.RequireFactoryTest(),"controlled interrupted-clear setup commits actual Required NVS");
    firmware::SettingsPersistenceWorker::SaveReceipt obstruction=0U;
    Check(persistence.EnqueueTracked(source.Doc(),obstruction),"unrelated tracked owner creates realistic clear admission obstacle");
    fixture::Wait([&]{return persistence.GetSaveOutcome(obstruction)==firmware::SettingsPersistenceWorker::SaveOutcome::Saved&&fixture::workerAtWait.load();});
    Check(!command("factory clear")&&!firmware_internal::IsHardwareTestSavePending(),"clear rejected while unrelated retained receipt owns worker");
    Check(persistence.ReleaseSaveReceipt(obstruction),"release temporary admission obstacle");
    for(auto denied:{modules::FactoryProvisioningState::Required,modules::FactoryProvisioningState::Invalid,modules::FactoryProvisioningState::Unavailable}){
        if(denied!=modules::FactoryProvisioningState::Required){
            if(denied==modules::FactoryProvisioningState::Invalid)factory_provisioning_sdk::committed[0]^=1U;
            else factory_provisioning_sdk::failRead=true;
            actualProvisioning.Begin(hardware::BoardRevision::V3S3,source.Doc(),arbiter);
        }
        sys.factoryRadio.bleProof=true; // Controlled terminal event; actual coordinator/SDK handshake is tested separately.
        firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
        Check(!sys.ble.enabled&&!firmware_internal::IsHardwareTestSavePending()&&actualProvisioning.State()==denied,
            "inactive historical dual proof cannot submit a fresh upgrade under Required/Invalid/Unavailable");
        if(firmware_internal::IsHardwareTestSavePending()){
            fixture::Wait([&]{return persistence.GetDiagnostics().outcome != firmware::SettingsPersistenceWorker::SaveOutcome::Pending && fixture::workerAtWait.load();});
            firmware_internal::UpdateHardwareTestPersistence(sys);
            firmware::PersistenceFixtureProvisioning afterHistoricalProof;
            afterHistoricalProof.Begin(hardware::BoardRevision::V3S3,source.Doc(),arbiter);
            Check(afterHistoricalProof.State()==denied,"historical terminal proof cannot restore an exemption on reboot");
        }
    }
    factory_provisioning_sdk::failRead=false;
    modules::EncodeFactoryProvisioningRecord(modules::FactoryProvisioningState::Required,factory_provisioning_sdk::committed);
    firmware::PersistenceFixtureProvisioning rebootRequired;
    rebootRequired.Begin(hardware::BoardRevision::V3S3,source.Doc(),arbiter);
    Check(rebootRequired.State()==modules::FactoryProvisioningState::Required&&rebootRequired.ShouldAutoEnterHardwareTest()&&
          std::string_view(source.ReadValue("FACTORY_CLEAR_USER_SENTINEL",""))=="keep-user-data",
          "Required survives recovery/reboot with historical record and user data intact");
    sys.hardwareTest.Enter(hardware::BoardRevision::V3S3,fixture::now.load());
    firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
    Check(!sys.factoryRadio.radioActive,"Required local review keeps radio off until ready");
    Check(sys.factoryRadio.hardwareTestActive,
          "actual runtime preserves active local test identity rather than impersonating an inactive saved verifier");
    Prepare(sys.hardwareTest,3000U,false);
    firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
    Check(sys.factoryRadio.radioActive,"Required actual Radio stage retains production admission");
    sys.ble.enabled = true; // Previously elected temporary endpoint still exists.
    sys.hardwareTest.OnButtonEvent({modules::UserButton::P1X, fxe::Button::Event::Press, modules::UiOwner::P1}, fixture::now.load());
    sys.hardwareTest.OnButtonEvent({modules::UserButton::P1X, fxe::Button::Event::Release, modules::UiOwner::P1}, fixture::now.load());
    // Production polls current sensors after button dispatch, before radio.
    BoardInput reinspectionInput; reinspectionInput.mask = 0ULL;
    sys.hardwareTest.UpdateSensors(reinspectionInput, fixture::now.load());
    firmware_internal::UpdateHardwareTestRadio(sys, fixture::now.load());
    Check(sys.hardwareTest.GetStage() == modules::ManufacturingTestStage::Local &&
          sys.factoryRadio.hardwareTestActive && !sys.factoryRadio.radioActive,
          "X-back keeps actual active-local identity at radio boundary while revoking readiness and server proof");
    sys.hardwareTest.Stop();
    actualProvisioning.Begin(hardware::BoardRevision::V3S3,source.Doc(),arbiter);

    // A failed narrow deletion retains ordinary Verified bytes, but cannot
    // advertise or upgrade them once the worker has committed Required.
    modules::WriteHardwareTestRecord(source.Doc(),verified);source.NotifyDocChanged();Check(source.SaveNow(),"ordinary Verified record saved before failed narrow removal");
    fixture::Wait([&]{return persistence.IsIdle()&&fixture::workerAtWait.load();});
    modules::EncodeFactoryProvisioningRecord(modules::FactoryProvisioningState::Passed,factory_provisioning_sdk::committed);
    actualProvisioning.Begin(hardware::BoardRevision::V3S3,source.Doc(),arbiter);
    Check(!command("hwtest start")&&!command("hwtest restart"),"actual genuine Passed protects Console re-entry");
    const uint32_t failedClearBefore=persistence.GetFailureCount();
    transaction.rejectNext.store(true);
    Check(command("factory clear", true, true),"Bluetooth narrow deletion admitted before controlled preparation failure");
    arbiter.AdvanceRender();
    fixture::Wait([&]{return persistence.GetFailureCount()>failedClearBefore&&fixture::workerAtWait.load();});
    firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
    fxe::SettingsStore failedClearDisk(config);modules::HardwareTestRecord retainedVerified;
    Check(actualProvisioning.State()==modules::FactoryProvisioningState::Required&&!sys.ble.enabled&&!firmware_internal::IsHardwareTestSavePending()&&
          failedClearDisk.Load()&&modules::DecodeHardwareTestRecord(failedClearDisk.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY,""),retainedVerified)&&modules::IsVerifiedFactoryPass(retainedVerified),
          "terminal narrow-removal failure retains Verified record and fences inactive service under durable Required");
    // Failed Required NVS itself must prevent every filesystem mutation.
    const uint32_t nvsFailedBefore=persistence.GetFailureCount();
    const unsigned preparesBeforeNvsFailure=transaction.prepareCalls.load();
    factory_provisioning_sdk::failCommit=true;
    const bool nvsFailureAccepted=command("factory clear");
    Check(nvsFailureAccepted,"Required NVS failure is reported asynchronously after accepted pending clear");
    arbiter.AdvanceRender();
    if(nvsFailureAccepted){
        fixture::Wait([&]{return persistence.GetFailureCount()>nvsFailedBefore&&fixture::workerAtWait.load();});
        firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
    }
    Check(actualProvisioning.State()==modules::FactoryProvisioningState::Unavailable&&!sys.ble.enabled&&
          transaction.prepareCalls.load()==preparesBeforeNvsFailure&&command("hwtest status")&&consoleOutput.find("provisioning_state=unavailable")!=std::string::npos,
          "failed Required prerequisite leaves record intact and exposes truthful Console failure without settings transaction");
    factory_provisioning_sdk::failCommit=false;
    actualProvisioning.Begin(hardware::BoardRevision::V3S3,source.Doc(),arbiter);
    modules::EncodeFactoryProvisioningRecord(modules::FactoryProvisioningState::Passed,factory_provisioning_sdk::committed);
    actualProvisioning.Begin(hardware::BoardRevision::V3S3,source.Doc(),arbiter);
    modules::BoardIdentity identityBeforeClear;
    Check(modules::ReadBoardIdentity(source.Doc(),identityBeforeClear)==modules::BoardIdentityRead::Current,"clear journey starts with exact stable identity");
    modules::HardwareTestRecord beforeClearRecord;
    Check(modules::DecodeHardwareTestRecord(source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY,""),beforeClearRecord)&&
          modules::IsVerifiedFactoryPass(beforeClearRecord)&&beforeClearRecord.factoryVerifierEnabled,
          "pending clear/off journey starts with durable eligible Verified verifier-on record");
    firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
    Check(!sys.ble.enabled,"historical eligible flag stays off in normal profile before clear");
    SelectFixtureBootProfile(sys, firmware::FactoryBootIntent::Verifier);
    firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
    Check(sys.factoryVerifierBoot && sys.ble.enabled,"explicit controlled verifier boot serves saved eligible record before clear");
    const uint32_t beforeClear=persistence.GetCompletedSaveCount();
    const unsigned nvsBefore=factory_provisioning_sdk::writes;
    arbiter.ResetPause(true);
    const bool clearAccepted=command("factory clear");
    Check(clearAccepted&&!sys.hardwareTest.IsActive()&&consoleOutput.find("pending Required")!=std::string::npos,
          "Console returns pending without synchronous Required flash work");
    if(clearAccepted){
        Check(actualProvisioning.State()==modules::FactoryProvisioningState::Passed,"pending clear does not claim Required before the actual worker commit");
        arbiter.AdvanceRender();
        fixture::Wait([]{return fixture::blocked.load();});
        Check(factory_provisioning_sdk::writes==nvsBefore&&firmware_internal::IsHardwareTestSavePending(),
              "Required prerequisite waits for controlled flash exclusion on storage worker");
        const std::string clearOwnedBytes=source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY,"");
        Check(!command("hwtest verifier off", false) &&
                  consoleOutput.find("verifier transition busy") != std::string::npos && !sys.ble.enabled &&
                  firmware_internal::attemptedFactoryBoots == 1U &&
                  firmware_internal::requestedFactoryBoots == 0U &&
                  firmware_internal::requestedFactoryBootIntent == firmware::FactoryBootIntent::None &&
                  sys.pendingFactoryBootIntent == firmware::FactoryBootIntent::None &&
                  !sys.rebootPersistencePending,
              "pending clear rejects verifier-off boot handoff without claiming accepted transition");
        Check(clearOwnedBytes==source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY,""),
              "pending clear owns original record bytes across redundant verifier-off");
        std::printf("[CLEAR_OFF_JOURNEY] phase=pending off=%u protected_bytes=%u provisioning=%u\n",
                    !sys.ble.enabled,clearOwnedBytes==source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY,""),unsigned(actualProvisioning.State()));
        const uint32_t loopStart=fixture::now.load();
        for(unsigned tick=0U;tick<5U;++tick){
            fixture::now.fetch_add(25U);
            Check(command("hwtest status")&&arbiter.AdvanceRender(),"Console input and rendering progress during worker exclusion wait");
            firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
            Check(!sys.ble.enabled,"pending clear fences inactive historical verifier traffic");
        }
        Check(fixture::now.load()==loopStart+125U&&!std::string_view(source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY,"")).empty(),
              "live cadence advances and narrow record remains until Required acknowledgement");
        fixture::hold.store(false);fixture::changed.notify_all();
        fixture::Wait([&]{return persistence.GetCompletedSaveCount()>beforeClear&&fixture::workerAtWait.load();});
        firmware_internal::UpdateHardwareTestPersistence(sys);
        fxe::SettingsStore clearedDisk(config);modules::BoardIdentity identityAfterClear;
        Check(actualProvisioning.State()==modules::FactoryProvisioningState::Required&&factory_provisioning_sdk::writes==nvsBefore+1U&&
              clearedDisk.Load()&&!firmware_internal::IsHardwareTestSavePending()&&!clearedDisk.Doc().containsKey(modules::HARDWARE_TEST_RECORD_SETTING_KEY)&&
              !source.Doc().containsKey(modules::HARDWARE_TEST_RECORD_SETTING_KEY)&&std::string_view(clearedDisk.ReadValue("FACTORY_CLEAR_USER_SENTINEL",""))=="keep-user-data"&&
              modules::ReadBoardIdentity(clearedDisk.Doc(),identityAfterClear)==modules::BoardIdentityRead::Current&&identityAfterClear==identityBeforeClear,
              "actual worker Required NVS acknowledgement precedes narrow deletion; user/identity survive");
        Check(command("hwtest status")&&consoleOutput.find("saved_record=none")!=std::string::npos&&
              consoleOutput.find("provisioning_state=required")!=std::string::npos,
              "successful clear/off overlap reports truthful Required and no saved factory record");
        // Force an ordinary later settings save through the real deferred owner:
        // old behavior must not hide record resurrection behind an idle receipt.
        const uint32_t beforeOrdinary=persistence.GetCompletedSaveCount();
        source.Doc()["FACTORY_CLEAR_POST_SAVE"]="ordinary-settings-kept";
        source.MarkMustSaveDirty();
        source.Update(); // Consume retained retry intent before aging its debounce.
        fixture::now.fetch_add(source.GetMustSaveDelayMs()+1U);source.Update();
        fixture::Wait([&]{return persistence.GetCompletedSaveCount()>beforeOrdinary&&fixture::workerAtWait.load();});
        firmware_internal::UpdateHardwareTestRadio(sys,fixture::now.load());
        fxe::SettingsStore afterOrdinary(config);modules::BoardIdentity postSaveIdentity;
        Check(afterOrdinary.Load()&&!source.Doc().containsKey(modules::HARDWARE_TEST_RECORD_SETTING_KEY)&&
              !afterOrdinary.Doc().containsKey(modules::HARDWARE_TEST_RECORD_SETTING_KEY)&&!sys.ble.enabled&&
              std::string_view(afterOrdinary.ReadValue("FACTORY_CLEAR_USER_SENTINEL",""))=="keep-user-data"&&
              std::string_view(afterOrdinary.ReadValue("FACTORY_CLEAR_POST_SAVE",""))=="ordinary-settings-kept"&&
              modules::ReadBoardIdentity(afterOrdinary.Doc(),postSaveIdentity)==modules::BoardIdentityRead::Current&&postSaveIdentity==identityBeforeClear,
              "ordinary save after clear/off cannot resurrect evidence and preserves user/identity data");
        firmware::PersistenceFixtureProvisioning postClearReboot;
        postClearReboot.Begin(hardware::BoardRevision::V3S3,afterOrdinary.Doc(),arbiter);
        Check(postClearReboot.State()==modules::FactoryProvisioningState::Required&&postClearReboot.ShouldAutoEnterHardwareTest(),
              "clear/off reboot retains Required through ordinary settings save");
        std::printf("[CLEAR_OFF_JOURNEY] phase=ordinary-save-reload off=%u ram_record=%u disk_record=%u reboot_required=%u\n",
                    !sys.ble.enabled,source.Doc().containsKey(modules::HARDWARE_TEST_RECORD_SETTING_KEY),
                    afterOrdinary.Doc().containsKey(modules::HARDWARE_TEST_RECORD_SETTING_KEY),postClearReboot.ShouldAutoEnterHardwareTest());
    }else{fixture::hold.store(false);fixture::changed.notify_all();}
    sys.factoryProvisioning.actual=nullptr;
    sys.factoryProvisioning.state=modules::FactoryProvisioningState::LegacyExempt;
    SelectFixtureBootProfile(sys, firmware::FactoryBootIntent::None);
    // Verification succeeds before final promotion. Force main->backup rename
    // to fail once, then let the same prepared transaction retry successfully.
    fxe::SettingsStore beforePromotionRetry(config);
    Check(beforePromotionRetry.Load(),
          "promotion-retry journey has a durable settings baseline");
    const uint32_t saveTotalBeforePromotionRetry = beforePromotionRetry.ReadValue(
        fxe::SETTINGS_SAVE_TOTAL_KEY, 0U);
    const uint32_t completedBeforePromotionRetry =
        persistence.GetCompletedSaveCount();
    const unsigned preparesBeforePromotionRetry = transaction.prepareCalls.load();
    const unsigned resultsBeforePromotionRetry = transaction.resultCalls.load();
    const unsigned savedBeforePromotionRetry = transaction.savedResults.load();
    const char* backupPath = "build/t_factory_receipt.json.bak";
    const char* backupBlockerPath = "build/t_factory_receipt.json.bak/block";
    std::remove(backupPath);
    Check(std::filesystem::create_directory(backupPath),
          "promotion-retry journey installs a non-removable backup target");
    std::FILE* backupBlocker = std::fopen(backupBlockerPath, "wb");
    Check(backupBlocker != nullptr,
          "promotion-retry journey makes the backup directory non-empty");
    if (backupBlocker != nullptr) std::fclose(backupBlocker);
    fixture::blocked.store(false);
    fixture::hold.store(false);
    fixture::retryDelayReached.store(false);
    fixture::holdRetry.store(true);
    firmware::SettingsPersistenceWorker::SaveReceipt promotionRetryReceipt = 0U;
    Check(persistence.EnqueueTracked(source.Doc(), promotionRetryReceipt) &&
              promotionRetryReceipt != 0U,
          "worker admits one transaction for a retryable promotion failure");
    fixture::Wait([] { return fixture::retryDelayReached.load(); });
    Check(std::remove(backupBlockerPath) == 0 &&
              std::filesystem::remove(backupPath),
          "promotion-retry journey removes the one-shot rename obstruction");
    fixture::holdRetry.store(false);
    fixture::changed.notify_all();
    fixture::Wait([&] {
        return persistence.GetCompletedSaveCount() >
                   completedBeforePromotionRetry &&
               fixture::workerAtWait.load();
    });
    fxe::SettingsStore afterPromotionRetry(config);
    Check(persistence.GetSaveOutcome(promotionRetryReceipt) ==
                  firmware::SettingsPersistenceWorker::SaveOutcome::Saved &&
              persistence.ReleaseSaveReceipt(promotionRetryReceipt) &&
              afterPromotionRetry.Load() &&
              afterPromotionRetry.ReadValue(
                  fxe::SETTINGS_SAVE_TOTAL_KEY, 0U) ==
                  saveTotalBeforePromotionRetry + 1U &&
              transaction.prepareCalls.load() ==
                  preparesBeforePromotionRetry + 1U &&
              transaction.resultCalls.load() ==
                  resultsBeforePromotionRetry + 1U &&
              transaction.savedResults.load() ==
                  savedBeforePromotionRetry + 1U,
          "post-verification rename retry prepares, reports, completes, and counts once");
    // A real terminal filesystem failure has a different outcome from a timeout.
    const uint32_t failuresBefore = persistence.GetFailureCount();
    const uint32_t completedBeforeFailure = persistence.GetCompletedSaveCount();
    const unsigned prepareBeforeFailure = transaction.prepareCalls.load();
    const unsigned resultsBeforeFailure = transaction.resultCalls.load();
    const unsigned failedBeforeFailure = transaction.failedResults.load();
    modules::EncodedHardwareTestRecord beforeFailure;
    Check(beforeFailure.Assign(source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, "")), "prior failure record fits bounded evidence");
    Check(modules::WriteBoardIdentity(source.Doc(), expectedIdentity), "failed save attempts carry matching identity");
    Check(!sys.boardIdentity.IsDurable(), "replacement remains unready before failed promotion");
    // Existing .tmp is preserved via .retry, so an empty .tmp directory alone
    // is recoverable. Block both staging paths to model terminal write failure.
    Check(std::filesystem::create_directory("build/t_factory_receipt.json.tmp") &&
              std::filesystem::create_directory("build/t_factory_receipt.json.retry"),
          "terminal failure fixture blocks both bounded settings staging paths");
    SelectFixtureBootProfile(sys, firmware::FactoryBootIntent::Test);
    Prepare(sys.hardwareTest, 3000U);
    firmware_internal::UpdateHardwareTestPersistence(sys);
    fixture::Wait([&] {
        return (persistence.GetFailureCount() >= failuresBefore + 3U ||
                persistence.GetCompletedSaveCount() > completedBeforeFailure) &&
               fixture::workerAtWait.load();
    });
    Check(persistence.GetFailureCount() == failuresBefore + 3U &&
              persistence.GetCompletedSaveCount() == completedBeforeFailure,
          "terminal failure fixture rejects all three attempts without a successful save");
    firmware_internal::UpdateHardwareTestPersistence(sys);
    Check(sys.hardwareTest.GetStage() == modules::ManufacturingTestStage::Failed &&
          beforeFailure.View() == source.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""),
          "terminal worker failure suppresses PASS and retains the last known record");
    Check(!sys.boardIdentity.IsDurable(), "real failed saves cannot acknowledge matching identity");
    const uint32_t terminalFailures = persistence.GetFailureCount();
    for (unsigned i = 0; i < 20U; ++i) { firmware_internal::UpdateHardwareTestPersistence(sys); }
    Check(persistence.GetFailureCount() == terminalFailures && persistence.IsIdle(),
          "terminal failure is reconciled once without an application retrying-write loop");
    Check(transaction.prepareCalls.load() == prepareBeforeFailure + 1U &&
              transaction.resultCalls.load() == resultsBeforeFailure + 1U &&
              transaction.failedResults.load() == failedBeforeFailure + 1U,
          "three settings write attempts prepare and report one current-line transaction");
    std::filesystem::remove("build/t_factory_receipt.json.tmp");
    std::filesystem::remove("build/t_factory_receipt.json.retry");
    std::printf("[MANUFACTURING_SAVE_JOURNEY] terminal_failure=true stage=%u previous_known_record=true next=restart\n",
                static_cast<unsigned>(sys.hardwareTest.GetStage()));
    const uint32_t rejectedCompleted = persistence.GetCompletedSaveCount();
    const uint32_t rejectedFailures = persistence.GetFailureCount();
    const unsigned rejectedResults = transaction.resultCalls.load();
    const unsigned rejectedFailed = transaction.failedResults.load();
    fxe::SettingsStore beforeRejected(config);
    Check(beforeRejected.Load(), "preparation rejection has a durable baseline");
    const unsigned durableMarker = beforeRejected.ReadValue(
        SnapshotTransactionProbe::KEY, 0U);
    transaction.rejectNext.store(true);
    firmware::SettingsPersistenceWorker::SaveReceipt rejectedReceipt = 0U;
    Check(persistence.EnqueueTracked(source.Doc(), rejectedReceipt) &&
              rejectedReceipt != 0U,
          "worker admits the exact snapshot before transaction preparation");
    fixture::Wait([&] {
        return persistence.GetFailureCount() > rejectedFailures &&
               fixture::workerAtWait.load();
    });
    fxe::SettingsStore afterRejected(config);
    Check(persistence.GetSaveOutcome(rejectedReceipt) ==
                  firmware::SettingsPersistenceWorker::SaveOutcome::Failed &&
              persistence.ReleaseSaveReceipt(rejectedReceipt) &&
              persistence.GetCompletedSaveCount() == rejectedCompleted &&
              transaction.resultCalls.load() == rejectedResults + 1U &&
              transaction.failedResults.load() == rejectedFailed + 1U &&
              afterRejected.Load() &&
              afterRejected.ReadValue(
                  SnapshotTransactionProbe::KEY, 0U) == durableMarker,
          "rejected current-line preparation reports once without changing settings");
    { std::lock_guard<std::mutex> lock(fixture::mutex); fixture::stopping = true; fixture::changed.notify_all(); }
    fixture::worker.join();
    RunCurrentGameLogWorkerJourney();
    RunCoalescedUndoFirstSnapshotJourney();
    RunLongCurrentGameSnapshotJourney();
    RunDurableLinePublicationOrdering();
    RunBoardFirstNewGameArchiveJourney();
    RunRecoveryProtectionSnapshot();
    RunLateSaveAfterSessionFallback();
    RunRuntimeStorageNewGameJourney(false);
    RunRuntimeStorageNewGameJourney(true);
    Check(rejectedCallbacks.load() == 0U,
          "rejected observer is never called; the original production observer survives Begin retry");
    Check(sys.factoryProvisioning.durableSnapshots > 0U,
          "the durable settings observer also advances factory provisioning");
    Check(transaction.wrongThreadCalls.load() == 0U &&
              transaction.malformedResults.load() == 0U &&
              transaction.publishedEarly.load() == 0U &&
              transaction.prepareCalls.load() == transaction.resultCalls.load(),
          "every transaction terminates on-worker before save success publishes");
    std::printf("[MANUFACTURING_PERSISTENCE_LIFECYCLE] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
