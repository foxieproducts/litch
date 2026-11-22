#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "hardware/rmt_flash_arbiter.hpp"
#include "modules/board_owner_persistence.hpp"
#include "modules/board_identity_persistence.hpp"
#include "modules/board_owner_pairing_runtime.hpp"

namespace owner_runtime_fixture {
int failures = 0;
TaskFunction_t task = nullptr;
void* taskContext = nullptr;
unsigned notifications = 0U;
bool inWorker = false;
bool refuseMutex = false;
hardware::RmtFlashArbiter* arbiter = nullptr;
struct Stop {};
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::printf("[FAIL] %s\n", message); }
}
void Render() {
    if (arbiter->TryBeginRender()) arbiter->EndRender();
}
void Run() {
    Render();
    inWorker = true;
    try { task(taskContext); } catch (const Stop&) {}
    inWorker = false;
}
}

TaskHandle_t OwnerRuntimeTaskCreate(TaskFunction_t task, const char*, uint32_t,
                                 void* context, uint32_t, StackType_t*, StaticTask_t*, int) {
    owner_runtime_fixture::task = task;
    owner_runtime_fixture::taskContext = context;
    return reinterpret_cast<TaskHandle_t>(1U);
}
void OwnerRuntimeDelay(uint32_t ticks) {
    fxe::ManualMillis::AdvanceMs(ticks);
    owner_runtime_fixture::Render();
}
void OwnerRuntimeDelete(TaskHandle_t) {}
uint32_t OwnerRuntimeNotifyTake(BaseType_t, TickType_t) {
    if (owner_runtime_fixture::notifications == 0U) throw owner_runtime_fixture::Stop{};
    owner_runtime_fixture::notifications = 0U;
    return 1U;
}
BaseType_t OwnerRuntimeNotifyGive(TaskHandle_t) {
    ++owner_runtime_fixture::notifications;
    return pdPASS;
}
BaseType_t OwnerRuntimeTake(SemaphoreHandle_t mutex, uint32_t ticks) {
    if (owner_runtime_fixture::refuseMutex) {
        owner_runtime_fixture::refuseMutex = false;
        return pdFALSE;
    }
    return xSemaphoreTake(mutex, ticks);
}

// Execute the actual worker and adapter, with only task scheduling controlled.
// SettingsStore retains its native filesystem transaction and verification.
#define ARDUINO 1
#define SettingsPersistenceWorker OwnerRuntimeSettingsWorker
#define BoardOwnerPersistenceRuntime OwnerRuntimePersistence
#define xTaskCreateStaticPinnedToCore OwnerRuntimeTaskCreate
#define vTaskDelay OwnerRuntimeDelay
#define vTaskDelete OwnerRuntimeDelete
#define ulTaskNotifyTake OwnerRuntimeNotifyTake
#define xTaskNotifyGive OwnerRuntimeNotifyGive
#define xSemaphoreTake OwnerRuntimeTake
#include "../../src/settings_persistence_worker.cpp"
#include "../../src/settings_persistence_worker_recovery.cpp"
#include "../../src/board_owner_persistence_runtime.cpp"
#undef xSemaphoreTake
#undef xTaskNotifyGive
#undef ulTaskNotifyTake
#undef vTaskDelete
#undef vTaskDelay
#undef xTaskCreateStaticPinnedToCore
#undef BoardOwnerPersistenceRuntime
#undef SettingsPersistenceWorker
#undef ARDUINO

namespace {
using owner_runtime_fixture::Check;
using Record = modules::BoardOwnerRecord;
using Runtime = firmware::OwnerRuntimePersistence;
using Outcome = Runtime::Outcome;
using Worker = firmware::OwnerRuntimeSettingsWorker;

struct Fixture {
    std::filesystem::path root;
    std::string path;
    fxe::SettingsStoreConfig config;
    fxe::SettingsStore source;
    hardware::RmtFlashArbiter arbiter;
    Worker worker;
    modules::BoardIdentityPersistence identity;
    Runtime runtime;
    unsigned clears = 0U;
    bool clearResult = true;
    unsigned rollbacks = 0U;
    bool rollbackResult = true;

    explicit Fixture(const char* name, bool recoveredOwner = false)
        : root(
#ifdef _WIN32
               std::filesystem::temp_directory_path() /
#else
               // macOS's default TMPDIR can exceed the production 64-byte
               // settings path limit before the PID-isolated fixture name.
               std::filesystem::path("/tmp") /
#endif
               (std::string(name) + "_" + std::to_string(
#ifdef _WIN32
                   _getpid()
#else
                   getpid()
#endif
               ))),
          path((root / "s.json").string()), config(Config(path)), source(config),
          worker(config), runtime(source, worker, identity) {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        owner_runtime_fixture::arbiter = &arbiter;
        owner_runtime_fixture::notifications = 0U;
        source.Doc()["BRIGHTNESS"] = 18U;
        source.Doc()["GAME_FEN"] = "prior game";
        if (recoveredOwner) {
            modules::BoardIdentity expected;
            expected.bytes.fill(0x41U);
            Check(modules::WriteBoardIdentity(source.Doc(), expected) &&
                      modules::WriteBoardOwner(source.Doc(), Record::Active),
                  "recovered fixture preserves an existing owner and identity");
            std::string bytes;
            serializeJson(source.Doc(), bytes);
            std::ofstream(path) << bytes;
            std::ofstream(path + ".tmp") << bytes;
            source.SetVerifiedPrimaryLoadCallback([](void* context, const JsonDocument& document) {
                static_cast<modules::BoardIdentityPersistence*>(context)->ObserveVerifiedPrimaryLoad(document);
            }, &identity);
            Check(source.Load() && source.IsLoadRepairPending() &&
                      identity.Prepare(source.Doc(), nullptr, nullptr) && !identity.IsDurable(),
                  "actual pending-file recovery cannot borrow primary identity proof");
            Check(worker.SetDurableSnapshotCallback([](void* context, const JsonDocument& document) {
                static_cast<modules::BoardIdentityPersistence*>(context)->OnDurableSettingsSnapshot(document);
            }, &identity), "actual verified worker callback owns identity durability");
        }
        Check(runtime.Begin(), "boot prepares the fixed private owner slot");
        Check(worker.Begin(source), "actual settings worker starts");
    }
    ~Fixture() {
        std::filesystem::remove_all(root);
    }
    static fxe::SettingsStoreConfig Config(const std::string& path) {
        fxe::SettingsStoreConfig config;
        config.path = path.c_str();
        config.allowSynchronousDeferredSaves = false;
        return config;
    }
    Runtime::NativeClearPort Port() { return {this, &Clear}; }
    Runtime::NativeRollbackPort RollbackPort() { return {this, &Rollback}; }
    static bool Rollback(void* context, const modules::BoardOwnerAddBaseline& baseline) {
        auto& fixture = *static_cast<Fixture*>(context);
        ++fixture.rollbacks;
        Check(owner_runtime_fixture::inWorker && baseline.Valid() &&
                  baseline == fixture.runtime.AddingBaseline() &&
                  modules::ReadBoardOwner(fixture.source.Doc()) == Record::Adding,
              "targeted rollback runs on settings_io with the exact durable baseline");
        return fixture.rollbackResult;
    }
    static bool Clear(void* context) {
        auto& fixture = *static_cast<Fixture*>(context);
        ++fixture.clears;
        Check(owner_runtime_fixture::inWorker, "native clear executes on settings_io");
        const bool renderEntered = fixture.arbiter.TryBeginRender();
        Check(renderEntered, "native clear acquires its host owner before individual NVS leaf leases");
        if (renderEntered) fixture.arbiter.EndRender();
        Check(modules::ReadBoardOwner(fixture.source.Doc()) == Record::Clearing,
              "native clear runs while live owner record remains Clearing");
        return fixture.clearResult;
    }
    void Save(Record record) {
        Runtime::Receipt receipt = 0U;
        Check(runtime.SaveOwnerRecord(record, receipt) && receipt != 0U,
              "owner transition receives exact worker receipt");
        owner_runtime_fixture::Run();
        runtime.Update();
        Check(runtime.GetOutcome(receipt) == Outcome::Saved && runtime.Release(receipt),
              "durable owner transition reconciles and releases");
    }
    Record DurableRecord() {
        std::ifstream file(path);
        StaticJsonDocument<fxe::SETTINGS_STORE_JSON_CAPACITY> document;
        Check(!deserializeJson(document, file), "native primary settings parse");
        return modules::ReadBoardOwner(document);
    }
    void BlockPendingRemoval() {
        const auto pending = path + ".tmp";
        std::filesystem::create_directories(pending);
        std::ofstream(pending + "/retain") << "unknown pending contents";
    }
};

void TestRecoveredIdentitySavedOwnerRepair() {
    Fixture fixture("litch_owner_identity_repair", true);
    modules::BoardOwnerPairingRuntime owner(fixture.runtime);
    using Controller = modules::BoardOwnerPairingController;
    const modules::BoardOwnerPairingRuntime::Connection connection{7U, 11U, 13U};
    const Controller::ConnectionSecurity security{{true, 1U}, true, true, true, true};
    owner.Boot(Record::Active, security.bonds, false);
    fxe::ManualMillis::AdvanceMs(10000U);
    fixture.source.Update();
    Check(fixture.worker.GetDiagnostics().requested == 0U &&
              owner.Status(connection).flags == modules::owner_pairing_session_wire::OwnerRetained,
          "idle recovered boot stays read-only and publishes retained owner without identity durability");
    for (unsigned failure = 0U; failure < 4U; ++failure) {
        auto rejected = security;
        if (failure == 0U) rejected.bonded = false;
        if (failure == 1U) rejected.peerMatches = false;
        if (failure == 2U) rejected.encrypted = false;
        if (failure == 3U) rejected.bonds.available = false;
        Check(!owner.RegisterOwnerConnection(connection, rejected) &&
                  fixture.worker.GetDiagnostics().requested == 0U,
              "incomplete, foreign or unbonded proof cannot admit a settings repair");
    }
    Check(!owner.RegisterOwnerConnection(connection, security) && !owner.OwnerReady(connection),
          "saved-owner recovery intent grants no product authority before verified promotion");
    Check(fixture.worker.GetDiagnostics().requested == 1U,
          "cryptographically verified existing owner admits one tracked recovery snapshot");
    auto successor = connection;
    ++successor.epoch;
    owner.Disconnected(connection);
    Check(!owner.RegisterOwnerConnection(successor, security) &&
              fixture.worker.GetDiagnostics().requested == 1U,
          "disconnect and successor security cannot duplicate the outstanding tracked repair");
    owner_runtime_fixture::Run();
    owner_runtime_fixture::refuseMutex = true;
    fixture.runtime.Update();
    Check(fixture.worker.GetDiagnostics().receipt != 0U &&
              !fixture.runtime.RequestIdentityRepair(
                  (static_cast<uint64_t>(successor.epoch) << 32U) | successor.sessionToken),
          "a refused receipt release retains the exact terminal repair and excludes its successor");
    fixture.runtime.Update();
    owner.UpdateDurableState(fixture.runtime.DurableOwnerRecord(), fixture.identity.IsDurable());
    Check(fixture.identity.IsDurable() && owner.RegisterOwnerConnection(successor, security) &&
              owner.OwnerReady(successor) && !owner.OwnerReady(connection) &&
              !std::filesystem::exists(fixture.path + ".tmp"),
          "verified promotion removes pending bytes and restores the same saved owner");
    modules::BoardIdentity actual;
    Check(fixture.identity.GetDurableIdentity(actual) && actual.bytes[0U] == 0x41U,
          "recovery preserves the exact prepared board identity");
    fxe::SettingsStore reopened(fixture.config);
    modules::BoardIdentityPersistence rebootedIdentity;
    reopened.SetVerifiedPrimaryLoadCallback([](void* context, const JsonDocument& document) {
        static_cast<modules::BoardIdentityPersistence*>(context)->ObserveVerifiedPrimaryLoad(document);
    }, &rebootedIdentity);
    Check(reopened.Load() && rebootedIdentity.Prepare(reopened.Doc(), nullptr, nullptr) &&
              rebootedIdentity.IsDurable() && rebootedIdentity.GetDurableIdentity(actual) &&
              actual.bytes[0U] == 0x41U && modules::ReadBoardOwner(reopened.Doc()) == Record::Active,
          "next ordinary boot proves the same canonical identity and retained owner without another repair");
    std::puts("OWNER_IDENTITY_RECOVERY source=tmp idle_writes=0 saved_peer=1 identity=unchanged");
}

void TestRecoveredIdentityRepairFailure() {
    StaticJsonDocument<256> legacyDocument;
    modules::BoardIdentityPersistence generatedIdentity;
    Check(generatedIdentity.Prepare(legacyDocument, [](void*, uint8_t* bytes, size_t size) {
              std::fill_n(bytes, size, uint8_t{0x41U}); return true;
          }, nullptr) && !generatedIdentity.MatchesExistingPreparedIdentity(legacyDocument),
          "newly generated legacy identity cannot borrow an existing saved owner's repair intent");
    Fixture fixture("litch_owner_identity_failed", true);
    modules::BoardOwnerPairingRuntime owner(fixture.runtime);
    const modules::BoardOwnerPairingRuntime::Connection connection{7U, 21U, 23U};
    const modules::BoardOwnerPairingController::ConnectionSecurity security{{true, 1U}, true, true, true, true};
    owner.Boot(Record::Active, security.bonds, false);
    auto changed = fixture.source.Doc()[modules::KEY_BOARD_IDENTITY_V1][1U];
    changed.set("42424242424242424242424242424242");
    Check(!owner.RegisterOwnerConnection(connection, security) &&
              fixture.worker.GetDiagnostics().requested == 0U,
          "even verified owner security cannot promote an identity different from the immutable prepared identity");
    changed.set("41414141414141414141414141414141");
    std::filesystem::remove(fixture.path + ".tmp");
    fixture.BlockPendingRemoval();
    Check(!owner.RegisterOwnerConnection(connection, security) &&
              fixture.worker.GetDiagnostics().requested == 1U,
          "failed-promotion scenario admits only the tracked repair");
    owner_runtime_fixture::Run(); fixture.runtime.Update();
    Check(!fixture.identity.IsDurable() && !owner.OwnerReady(connection) &&
              fixture.worker.GetDiagnostics().receipt == 0U,
          "failed pending cleanup never invokes identity durability and releases the tracked receipt");
    Check(!owner.RegisterOwnerConnection(connection, security) &&
              fixture.worker.GetDiagnostics().requested == 1U,
          "same security observation cannot retry a failed repair indefinitely");
    std::filesystem::remove_all(fixture.path + ".tmp");
    auto retry = connection; ++retry.epoch;
    owner.Disconnected(connection);
    Check(!owner.RegisterOwnerConnection(retry, security) &&
              fixture.worker.GetDiagnostics().requested == 2U,
          "a newly authenticated connection can own one new repair after failure");
    owner_runtime_fixture::Run(); fixture.runtime.Update();
    owner.UpdateDurableState(fixture.runtime.DurableOwnerRecord(), fixture.identity.IsDurable());
    Check(owner.RegisterOwnerConnection(retry, security) && owner.OwnerReady(retry),
          "new connection recovers only after its own successful verified promotion");
}

void TestTrackedAdmissionAndPreferences() {
    Fixture fixture("litch_owner_runtime_admission");
    Runtime::Receipt obstruction = 0U;
    Check(fixture.worker.EnqueueTracked(fixture.source.Doc(), obstruction),
          "another real tracked owner reserves the worker");
    const size_t usage = fixture.source.Doc().memoryUsage();
    const uint32_t revision = fixture.source.GetRevision();
    Runtime::Receipt rejected = 99U;
    Check(!fixture.runtime.SaveOwnerRecord(Record::Active, rejected) && rejected == 0U &&
              fixture.source.GetRevision() == revision &&
              fixture.source.Doc().memoryUsage() == usage &&
              modules::ReadBoardOwner(fixture.source.Doc()) == Record::Absent,
          "failed admission restores owner with no revision or arena drift");
    owner_runtime_fixture::Run();
    Check(fixture.worker.ReleaseSaveReceipt(obstruction), "obstruction releases");
    Check(!fixture.runtime.RequestNativeClear(fixture.Port()),
          "native deletion refuses before durable Clearing");

    Runtime::Receipt receipt = 0U;
    Check(fixture.runtime.SaveOwnerRecord(Record::Active, receipt), "active intent admits");
    Check(!fixture.runtime.Release(receipt), "pending receipt cannot release");
    fixture.source.Doc()["BRIGHTNESS"] = 49U;
    fixture.source.Doc()["GAME_FEN"] = "newest game";
    fixture.source.MarkMustSaveDirty();
    Check(!fixture.worker.Enqueue(fixture.source.Doc()),
          "tracked request blocks later whole-document snapshot replacement");
    owner_runtime_fixture::Run();
    Check(fixture.worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Saved &&
              fixture.runtime.GetOutcome(receipt) == Outcome::Pending,
          "worker durability remains hidden until cooperative reconciliation");
    fixture.runtime.Update();
    Check(fixture.runtime.GetOutcome(receipt) == Outcome::Saved &&
              fixture.runtime.GetOutcome(receipt + 1U) == Outcome::Unknown,
          "only exact retained owner receipt publishes Saved");
    owner_runtime_fixture::refuseMutex = true;
    Check(!fixture.runtime.Release(receipt) &&
              fixture.runtime.GetOutcome(receipt) == Outcome::Saved &&
              !fixture.runtime.SaveOwnerRecord(Record::Clearing, rejected),
          "failed worker release retains terminal evidence and blocks reuse");
    Check(fixture.runtime.Release(receipt) && !fixture.runtime.Release(receipt),
          "exact terminal receipt releases once");
    fxe::ManualMillis::AdvanceMs(100U);
    fixture.source.Update();
    owner_runtime_fixture::Run();
    fxe::SettingsStore reopened(fixture.config);
    Check(reopened.Load() && modules::ReadBoardOwner(reopened.Doc()) == Record::Active &&
              reopened.Doc()["BRIGHTNESS"] == 49U &&
              reopened.Doc()["GAME_FEN"] == "newest game",
          "subsequent deferred save keeps owner with latest preferences and game");
}

void TestPartialSaveRollsBackBeforeFailure() {
    Fixture fixture("litch_owner_runtime_partial");
    fixture.Save(Record::Absent);
    fixture.BlockPendingRemoval();
    Runtime::Receipt receipt = 0U;
    const size_t usage = fixture.source.Doc().memoryUsage();
    Check(fixture.runtime.SaveOwnerRecord(Record::Active, receipt), "partial save admits");
    // Production game persistence replaces the complete live document while
    // retaining private keys. Do the same here to reject cached arena handles.
    StaticJsonDocument<fxe::SETTINGS_STORE_JSON_CAPACITY> candidate;
    candidate.set(fixture.source.Doc());
    candidate["BRIGHTNESS"] = 53U;
    candidate["GAME_FEN"] = "latest game";
    fixture.source.Doc().set(candidate);
    fixture.source.MarkMustSaveDirty();
    fixture.source.Doc()["oversized"] = std::string(12000U, 'x');
    fixture.source.Doc().remove("oversized");
    Check(fixture.source.Doc().overflowed(),
          "later failed preference allocation sets sticky overflow while save is pending");
    const size_t pendingUsage = fixture.source.Doc().memoryUsage();
    owner_runtime_fixture::Run();
    Check(fixture.worker.GetSaveOutcome(receipt) == Worker::SaveOutcome::Failed &&
              fixture.DurableRecord() == Record::Active,
          "actual primary promotion can be visible despite terminal worker failure");
    const uint32_t beforeUpdate = fixture.source.GetRevision();
    Check(fixture.runtime.GetOutcome(receipt) == Outcome::Pending,
          "partial durability never leaks terminal success from the worker");
    fixture.runtime.Update();
    Check(fixture.runtime.GetOutcome(receipt) == Outcome::Failed &&
              modules::ReadBoardOwner(fixture.source.Doc()) == Record::Absent &&
              fixture.source.GetRevision() == beforeUpdate + 1U &&
              fixture.source.Doc().memoryUsage() == pendingUsage &&
              fixture.source.Doc()["BRIGHTNESS"] == 53U &&
              fixture.source.Doc()["GAME_FEN"] == "latest game",
          "terminal failure restores private intent in place despite arena rebuild and overflow");
    Check(fixture.runtime.Release(receipt), "partial failure retains releasable exact receipt");
    std::filesystem::remove_all(fixture.path + ".tmp");
    candidate.clear();
    candidate.set(fixture.source.Doc());
    fixture.source.Doc().set(candidate);
    Check(!fixture.source.Doc().overflowed() &&
              fixture.source.Doc().memoryUsage() <= usage + 64U,
          "ordinary complete-document compaction clears failed preference allocation");
    fixture.Save(Record::Clearing);
    Check(fixture.DurableRecord() == Record::Clearing,
          "recovery can overwrite partial Active with durable Clearing");
}

void TestNativeClearAndFailureRecovery() {
    Fixture fixture("litch_owner_runtime_clear");
    fixture.Save(Record::Clearing);
    const size_t usage = fixture.source.Doc().memoryUsage();
    const uint32_t revision = fixture.source.GetRevision();
    Runtime::Receipt obstruction = 0U;
    Check(fixture.worker.EnqueueTracked(fixture.source.Doc(), obstruction) &&
              !fixture.runtime.RequestNativeClear(fixture.Port()) && fixture.clears == 0U &&
              fixture.source.GetRevision() == revision && fixture.source.Doc().memoryUsage() == usage,
          "failed native job admission performs no native work or RAM mutation");
    owner_runtime_fixture::Run();
    Check(fixture.worker.ReleaseSaveReceipt(obstruction), "native job obstruction releases");
    fixture.clearResult = false;
    Check(fixture.runtime.RequestNativeClear(fixture.Port()) && fixture.clears == 0U,
          "native clear admission does no synchronous native work");
    Runtime::Receipt receipt = 0U;
    Check(!fixture.runtime.SaveOwnerRecord(Record::Absent, receipt) &&
              !fixture.runtime.ReleaseNativeClear(), "pending clear blocks final Absent");
    owner_runtime_fixture::Run();
    fixture.runtime.Update();
    Check(fixture.runtime.GetNativeClearOutcome() == Outcome::Failed && fixture.clears == 1U &&
              fixture.DurableRecord() == Record::Clearing &&
              fixture.source.Doc().memoryUsage() == usage && fixture.source.GetRevision() == revision,
          "failed native empty-store proof retains durable tombstone and unchanged RAM");
    Check(fixture.runtime.ReleaseNativeClear(), "failed native result releases for retry");

    fixture.clearResult = true;
    fixture.BlockPendingRemoval();
    Check(fixture.runtime.RequestNativeClear(fixture.Port()), "clear retry admits");
    owner_runtime_fixture::Run();
    fixture.runtime.Update();
    Check(fixture.runtime.GetNativeClearOutcome() == Outcome::Failed && fixture.clears == 2U,
          "native callback runs once even across three failed snapshot attempts");
    Check(fixture.runtime.ReleaseNativeClear(), "failed post-clear snapshot result releases");
    std::filesystem::remove_all(fixture.path + ".tmp");

    Check(fixture.runtime.RequestNativeClear(fixture.Port()), "native clear retries safely");
    owner_runtime_fixture::Run();
    fixture.runtime.Update();
    Check(fixture.runtime.GetNativeClearOutcome() == Outcome::Saved && fixture.clears == 3U &&
              !fixture.runtime.SaveOwnerRecord(Record::Absent, receipt),
          "successful empty-store proof still retains worker slot before release");
    owner_runtime_fixture::refuseMutex = true;
    Check(!fixture.runtime.ReleaseNativeClear() &&
              fixture.runtime.GetNativeClearOutcome() == Outcome::Saved,
          "failed native receipt release retains exact Saved evidence");
    Check(fixture.runtime.ReleaseNativeClear(), "native clear releases before controller ack");
    fixture.Save(Record::Absent);
    Check(fixture.DurableRecord() == Record::Absent &&
              fixture.source.Doc().memoryUsage() == usage,
          "final Absent becomes durable without growing private record arena");
}

void TestAddingJournalAndTargetedWorkerLifetime() {
    Fixture f("litch_owner_runtime_adding");
    f.Save(Record::Active);
    modules::BoardOwnerAddBaseline baseline{};
    baseline.saved.count = 2U;
    baseline.saved.peers[0U].address[0U] = 11U;
    baseline.saved.peers[1U].address[0U] = 12U;
    baseline.rpaCount = 1U;
    baseline.rpas[0U].rpa.address[0U] = 91U;
    baseline.rpas[0U].identity.address[0U] = 99U;
    Check(!f.runtime.RequestNativeRollback(f.RollbackPort()),
          "Add rollback refuses before the Adding journal is durable");
    Runtime::Receipt receipt = 0U;
    const size_t usage = f.source.Doc().memoryUsage();
    Check(f.runtime.SaveAddingJournal(baseline, receipt) &&
              !f.runtime.RequestNativeRollback(f.RollbackPort()) && f.rollbacks == 0U,
          "admitted journal still cannot authorize native work before its exact receipt");
    owner_runtime_fixture::Run(); f.runtime.Update();
    Check(f.runtime.GetOutcome(receipt) == Outcome::Saved && f.runtime.Release(receipt) &&
              f.runtime.DurableOwnerRecord() == Record::Adding && f.runtime.AddingBaseline() == baseline,
          "saved Adding record adopts exact identities and pre-existing RPA metadata");
    fxe::SettingsStore reopened(f.config);
    Check(reopened.Load() && modules::ReadBoardOwner(reopened.Doc()) == Record::Adding &&
              modules::ReadBoardOwnerAddBaseline(reopened.Doc()) == baseline,
          "actual settings file reopens with the complete recovery journal");
    Worker rebootedWorker(f.config);
    Runtime rebooted(reopened, rebootedWorker, f.identity);
    Check(rebooted.Begin() && rebooted.AddingBaseline() == baseline,
          "boot reconstruction preserves the durable baseline without native mutation");
    f.rollbackResult = false;
    Check(f.runtime.RequestNativeRollback(f.RollbackPort()) &&
              !f.runtime.RequestNativeClear(f.Port()) &&
              !f.runtime.ReleaseNativeRollback() && f.clears == 0U,
          "pending targeted rollback excludes all-bond clear and premature release");
    owner_runtime_fixture::Run(); f.runtime.Update();
    Check(f.runtime.GetNativeRollbackOutcome() == Outcome::Failed && f.runtime.ReleaseNativeRollback() &&
              f.runtime.AddingBaseline() == baseline && f.DurableRecord() == Record::Adding,
          "failed native proof keeps the durable recovery journal and baseline");
    f.rollbackResult = true;
    Check(f.runtime.RequestNativeRollback(f.RollbackPort()), "targeted rollback retries the same journal");
    owner_runtime_fixture::Run(); f.runtime.Update();
    Check(f.runtime.GetNativeRollbackOutcome() == Outcome::Saved &&
              !f.runtime.SaveOwnerRecord(Record::Active, receipt) && f.runtime.ReleaseNativeRollback(),
          "checked native rollback must release its slot before final Active admission");
    f.Save(Record::Active);
    Check(f.rollbacks == 2U && f.clears == 0U && f.DurableRecord() == Record::Active &&
              !f.runtime.AddingBaseline().Valid() && f.source.Doc().memoryUsage() == usage,
          "Add rollback restores Active, retains allocation bounds and never invokes clear-all");
}

void TestAddingPartialSaveAndTamperedIntent() {
    Fixture f("litch_owner_runtime_adding_partial");
    f.Save(Record::Active);
    modules::BoardOwnerAddBaseline baseline{};
    baseline.saved.count = 1U; baseline.saved.peers[0U].address[0U] = 15U;
    f.BlockPendingRemoval();
    Runtime::Receipt receipt = 0U;
    Check(f.runtime.SaveAddingJournal(baseline, receipt), "partial Adding save admits");
    owner_runtime_fixture::Run(); f.runtime.Update();
    Check(f.runtime.GetOutcome(receipt) == Outcome::Failed && f.runtime.Release(receipt) &&
              f.runtime.DurableOwnerRecord() == Record::Active && f.DurableRecord() == Record::Adding &&
              !f.runtime.RequestNativeRollback(f.RollbackPort()),
          "visible partial journal remains failure and cannot authorize native security or rollback");
    fxe::SettingsStore reopened(f.config);
    Check(reopened.Load() && modules::ReadBoardOwnerAddBaseline(reopened.Doc()) == baseline,
          "restart can reconstruct even a journal whose final save result was failure");
    std::filesystem::remove_all(f.path + ".tmp");
    Check(f.runtime.SaveAddingJournal(baseline, receipt), "explicit journal retry admits after storage repair");
    owner_runtime_fixture::Run(); f.runtime.Update();
    Check(f.runtime.GetOutcome(receipt) == Outcome::Saved && f.runtime.Release(receipt),
          "retry establishes durable journal proof");
    auto changed = baseline; changed.saved.peers[0U].address[0U] = 16U;
    Check(modules::WriteBoardOwnerAdding(f.source.Doc(), changed) &&
              !f.runtime.RequestNativeRollback(f.RollbackPort()) &&
              !f.runtime.SaveOwnerRecord(Record::Active, receipt) && f.rollbacks == 0U,
          "same state/count with changed identity cannot replace the durable rollback baseline");
}
} // namespace

int main() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    TestTrackedAdmissionAndPreferences();
    TestPartialSaveRollsBackBeforeFailure();
    TestNativeClearAndFailureRecovery();
    TestAddingJournalAndTargetedWorkerLifetime();
    TestAddingPartialSaveAndTamperedIntent();
    TestRecoveredIdentitySavedOwnerRepair();
    TestRecoveredIdentityRepairFailure();
    if (owner_runtime_fixture::failures == 0) std::puts("[BOARD_OWNER_RUNTIME] ok");
    return owner_runtime_fixture::failures == 0 ? 0 : 1;
}
