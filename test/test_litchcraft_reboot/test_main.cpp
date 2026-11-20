#include <cstdio>
#include <filesystem>
#include <string>

#include "console.hpp"
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_internal.hpp"
#include "firmware/sleep_transition.hpp"
#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "runtime/system_context.hpp"
#include "logger.hpp"
#include "modules/volatile_persistence_session.hpp"
#include "modules/factory_provisioning_policy.hpp"
#include "modules/board_owner_pairing_runtime.hpp"
#include "modules/board_identity_persistence.hpp"
#include "owner_pairing_console_test_transport.hpp"

namespace reboot_fixture {
int failures = 0;
unsigned restarts = 0;
unsigned bootloaderRestarts = 0;
std::string rebootEvents;
constexpr const char* PATH = "build/test_litchcraft_reboot.json";
void Check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] %s\n", message); }
}
modules::SettingsStoreConfig Config() {
    modules::SettingsStoreConfig config;
    config.path = PATH;
    config.scrubFunc = &modules::ScrubLitchcraftRecord;
    return config;
}
// Only scheduling/receipt completion is controlled. Accepted bytes are frozen
// and committed through the real file-backed SettingsStore transaction.
struct Worker {
    using SaveReceipt = uint32_t;
    enum class SaveOutcome { Unknown, Pending, Saved, Failed };
    using Admission = fxe::SettingsStore::AsyncSaveAdmission;
    bool started = true;
    Admission admission = Admission::Accepted;
    SaveOutcome outcome = SaveOutcome::Pending;
    uint32_t retained = 0U, next = 0U;
    unsigned enqueues = 0U;
    std::string frozen;
    struct StoreSave {
        bool dirty = false;
        bool immediate = false;
        bool HasUnpersistedEdit() const { return dirty; }
        void RequestImmediateSave() { if (dirty) immediate = true; }
    } storeSave;
    StoreSave& LitchcraftFrameStream() { return storeSave; }
    const StoreSave& LitchcraftFrameStream() const { return storeSave; }
    bool LitchcraftFrameModeSavePending() const { return false; }
    bool LitchcraftFrameCommitPending() const { return false; }
    bool IsStarted() const { return started; }
    enum class StorageReformatState { Idle, Pending, Running, Succeeded, Failed };
    StorageReformatState reformat = StorageReformatState::Idle;
    unsigned formats = 0U;
    bool clearPending = false;
    bool IsIdle() const { return retained == 0U &&
        reformat != StorageReformatState::Pending && reformat != StorageReformatState::Running; }
    StorageReformatState GetStorageReformatState() const { return reformat; }
    bool LitchcraftFrameClearPending() const { return clearPending; }
    bool RequestStorageReformat(const JsonDocument& safe) {
        if (!IsIdle() || reformat != StorageReformatState::Idle) return false;
        ++formats; frozen.clear(); serializeJson(safe, frozen);
        reformat = StorageReformatState::Pending; return true;
    }
    bool ReleaseStorageReformatResult() {
        if (reformat != StorageReformatState::Failed && reformat != StorageReformatState::Succeeded) return false;
        reformat = StorageReformatState::Idle; return true;
    }
    bool EnqueueTracked(const JsonDocument& document, uint32_t& receipt,
                        Admission* result = nullptr) {
        const auto disposition = retained != 0U ? Admission::Deferred : admission;
        if (result != nullptr) *result = disposition;
        if (disposition != Admission::Accepted) return false;
        receipt = retained = ++next;
        ++enqueues;
        frozen.clear(); serializeJson(document, frozen);
        outcome = SaveOutcome::Pending;
        return true;
    }
    SaveOutcome GetSaveOutcome(uint32_t receipt) const {
        Check(receipt == retained, "poll uses retained exact receipt");
        return outcome;
    }
    bool ReleaseSaveReceipt(uint32_t receipt) {
        Check(receipt == retained, "release uses retained exact receipt");
        retained = 0U; return true;
    }
    void Complete(bool saved = true) {
        Check(retained != 0U, "completion requires an admitted snapshot");
        if (saved) {
            modules::SettingsStore writer(Config());
            Check(!deserializeJson(writer.Doc(), frozen), "frozen settings parse");
            saved = writer.SaveNow();
            Check(saved, "file-backed settings transaction succeeds");
        }
        outcome = saved ? SaveOutcome::Saved : SaveOutcome::Failed;
    }
};
struct OwnerPersistence final : modules::BoardOwnerPersistence {
    modules::SettingsStore& settings;
    Worker& worker;
    modules::BoardOwnerRecord durable = modules::BoardOwnerRecord::Absent;
    modules::BoardOwnerRecord requested = durable;
    Receipt receipt = 0U;
    OwnerPersistence(modules::SettingsStore& s, Worker& w) : settings(s), worker(w) {}
    bool SaveOwnerRecord(modules::BoardOwnerRecord record, Receipt& out) override {
        if (receipt != 0U) return false;
        const auto previous = modules::ReadBoardOwner(settings.Doc());
        if (!modules::WriteBoardOwner(settings.Doc(), record) ||
            !worker.EnqueueTracked(settings.Doc(), out)) {
            modules::WriteBoardOwner(settings.Doc(), previous); return false;
        }
        requested = record; receipt = out; return true;
    }
    Outcome GetOutcome(Receipt value) const override {
        if (value != receipt || value == 0U) return Outcome::Unknown;
        return worker.outcome == Worker::SaveOutcome::Pending ? Outcome::Pending :
            worker.outcome == Worker::SaveOutcome::Saved ? Outcome::Saved : Outcome::Failed;
    }
    bool Release(Receipt value) override {
        if (value != receipt || !worker.ReleaseSaveReceipt(value)) return false;
        receipt = 0U; return true;
    }
    void Update() {
        if (receipt != 0U && worker.outcome == Worker::SaveOutcome::Saved) durable = requested;
    }
    modules::BoardOwnerRecord DurableOwnerRecord() const { return durable; }
};
struct BootGuard {
    bool armed = false;
    unsigned healthy = 0U, retries = 0U;
    bool ConsumeRetryRebootArm() { const bool value = armed; armed = false; return value; }
    void PrepareForRetryReboot() { ++retries; }
    void MarkBootHealthy() { ++healthy; }
};
struct FirmwareSystem {
    bool rebootToBootloader = false;
    bool factoryBootOnly = false;
    bool factoryPersistenceStartFailed = false;
    firmware::FactoryBootIntent pendingFactoryBootIntent = firmware::FactoryBootIntent::None;
    firmware::FactoryCompletionReboot factoryCompletion;
    struct { modules::FactoryProvisioningState State() const {
        return modules::FactoryProvisioningState::LegacyExempt;
    } } factoryProvisioning;
    struct Radio {
        bool HasPendingTerminalReply() const { return false; }
        template<class Ble> void PollTerminalReply(Ble&, uint32_t) {}
    } factoryRadio;
    modules::SettingsStore settings{Config()};
    modules::LitchMaxEngine engine;
    runtime::GameStateMachine stateMachine;
    modules::GamePersistenceCoordinator gamePersistence{settings, engine, stateMachine};
    Worker settingsPersistence;
    OwnerPersistence boardOwnerPersistence{settings, settingsPersistence};
    modules::BoardOwnerPairingRuntime boardOwnerPairing{boardOwnerPersistence};
    bool settingsClearPending = false;
    uint32_t settingsClearReceipt = 0U;
    bool settingsClearPrimarySaved = false;
    firmware::BoardStorageRecoveryPublication storageRecoveryPublication;
    BootGuard bootGuard;
    struct {
        modules::BoardOwnerPairingRuntime* pairing = nullptr;
        bool nativeProceed = false, nativeFailure = false;
        unsigned nativeKeys = 1U, nativeDeletes = 0U;
        modules::BoardOwnerPairingRuntime::Connection owner{12U,31U,71U};
        uint64_t ConsoleInputGeneration() const {
            return (static_cast<uint64_t>(owner.epoch) << 32U) | owner.sessionToken;
        }
        bool RequestOwnerSettingsClear() {
            return pairing != nullptr && pairing->RequestOwnerClear(owner);
        }
        void DrainEventQueue() {}
        void UpdateOwnerPairing() {
            if (pairing == nullptr) return;
            pairing->Poll();
            auto& custody = pairing->Custody();
            const auto effect = custody.PendingEffect();
            using Effect = modules::BoardOwnerPairingController::EffectType;
            if (effect.type == Effect::QuiesceAndRejectWork && nativeProceed) {
                pairing->Disconnected(owner);
                custody.AcknowledgeQuiesce(effect.token, true);
            }
            else if (effect.type == Effect::DeleteAllBonds && nativeProceed) {
                ++nativeDeletes;
                if (!nativeFailure) nativeKeys = 0U;
                custody.AcknowledgeBondDelete(effect.token, !nativeFailure);
            } else if (effect.type == Effect::UnpairSucceeded)
                custody.AcknowledgeSuccess(effect.token);
        }
        bool ota = false;
        bool otaRuntimeAccepted = false;
        std::array<uint8_t, 3U> recovery{};
        uint32_t recoveryRevision = 0U;
        bool IsOtaInProgress() const { return ota; }
        bool IsOtaRuntimeAccepted() const { return otaRuntimeAccepted; }
        void PublishOtaRuntimeAccepted() { otaRuntimeAccepted = true; }
        bool PublishConnectionState(uint16_t key, uint32_t revision, const uint8_t* data, size_t size) {
            Check(key == 38U && size == 3U, "recovery uses reliable bounded State 38");
            recoveryRevision = revision;
            std::copy(data, data + size, recovery.begin()); return true;
        }
    } ble;
    modules::ControlMutationService controlMutation;
    modules::PersistenceDegradation persistenceDegradation = modules::PersistenceDegradation::None;
    struct {
        bool active = false;
        bool overridePending = false;
        bool IsActive() const { return active; }
        bool IsOverridePending() const { return overridePending; }
        void Stop() { active = false; }
    } hardwareTest, amptest, amptestVerify, comptest;
    struct Leds {
        bool complete = true;
        unsigned clears = 0U, shows = 0U;
        void Clear() { ++clears; rebootEvents += 'C'; }
        void Show() { ++shows; rebootEvents += 'S'; }
        bool LastShowCompleted() const { return complete; }
        void RestoreOutputPinRouting() {}
    } leds;
    // Only task scheduling and physical transfer completion are controlled;
    // the production rail owner still requires a confirmed black Show.
    struct LedAnimation {
        struct Leds& output;
        bool quiescent = false;
        bool IsStarted() const { return true; }
        bool IsQuiescent() const { return quiescent; }
        bool Quiesce() {
            if (quiescent) return true;
            output.Clear(); output.Show();
            quiescent = output.LastShowCompleted();
            return quiescent;
        }
        void Resume() { quiescent = false; }
    } ledAnimation{leds};
    Leds& Leds() { return leds; }
    struct {
        bool enabled = true;
        unsigned disables = 0U;
        bool Is5vEnableRequested() const { return enabled; }
        bool Is5vEnabled() const { return enabled; }
        void Enable5v() { enabled = true; }
        void Disable5v() { enabled = false; ++disables; rebootEvents += 'P'; }
    } powerRail;
    struct {
        firmware::SleepState state = firmware::SleepState::Awake;
        firmware::SleepState GetState() const { return state; }
    } sleep;
    bool otaBootAwaitingHealth = false;
    bool safeModeActive = false, rebootPersistencePending = false;
    bool rebootPersistenceStarted = false, litchcraftStorageRecovered = false;
    bool startupBoardSnapshotObserved = false;
    bool recoveryFrameInitialized = false;
    fxe::ElapsedTime rebootPersistenceElapsed;
    fxe::ElapsedTime sleepPersistenceElapsed;
    struct { bool commitFinished = true; } runtimeBootHealth;
};
FirmwareSystem* system = nullptr;
runtime::SystemContext context;
FirmwareSystem& GetSystem() { return *system; }
bool ClearLedOutput(FirmwareSystem& sys) {
    if (sys.ledAnimation.IsQuiescent() && !sys.powerRail.Is5vEnabled()) return true;
    if (sys.ledAnimation.IsQuiescent()) return false;
    sys.Leds().Clear(); sys.Leds().Show();
    return sys.Leds().LastShowCompleted();
}
runtime::SystemContext& GetSystemContext() { return context; }
namespace console_commands {
bool RebootCommand(Console&, const Console::Args&);
bool NativeRebootCommand(Console&, const Console::Args&);
}
// Factory entry/delivery is outside this normal reboot fixture. Default-normal
// fields keep actual lifecycle/GPC/store behavior; an unexpected factory helper
// is a failure rather than a synthetic successful boot or persistence path.
void ArmFactoryBootIntent(firmware::FactoryBootIntent intent) {
    Check(intent == firmware::FactoryBootIntent::None, "normal reboot never arms a factory boot request");
}
bool hardwareTestSavePending = false;
bool hardwareTestReceiptReady = false;
unsigned hardwareTestReceiptPolls = 0U;
bool IsHardwareTestSavePending() { return hardwareTestSavePending; }
void UpdateHardwareTestPersistence(FirmwareSystem&) {
    ++hardwareTestReceiptPolls;
    if (hardwareTestReceiptReady) hardwareTestSavePending = false;
}
bool RequestFactoryClear(FirmwareSystem&) { Check(false, "factory clear outside normal reboot fixture"); return false; }
bool PrepareFactoryPersistence(FirmwareSystem&) { Check(false, "factory persistence outside normal reboot fixture"); return false; }
void ReleaseHardwareTestInput(FirmwareSystem&) { Check(false, "factory input release outside normal reboot fixture"); }
void delay(unsigned ms) { if (ms == 1000U) rebootEvents += 'E'; }
struct { void restart() { ++restarts; } } ESP;
constexpr unsigned RESTART_BOOTLOADER = 1U;
void usb_persist_restart(unsigned mode) {
    Check(mode == RESTART_BOOTLOADER, "ROM entry uses the pinned Arduino bootloader primitive");
    ++bootloaderRestarts;
    rebootEvents += 'B';
}
}

namespace firmware { using RebootFixtureWorker = reboot_fixture::Worker; }
// Preloaded native firmware headers intentionally contain no SDK root object.
// Rename only the root wiring namespace/worker type to avoid aggregate ODR
// collisions; actual command, lifecycle helper, GPC and persistence execute.
#define ARDUINO 1
#define LITCH_REBOOT_TEST_STORE 1
#define firmware_internal reboot_fixture
#define SettingsPersistenceWorker RebootFixtureWorker
#include "../../src/firmware_litchcraft_persistence_runtime.cpp"
#include "../../src/firmware_led_rail_quiesce.cpp"
#define LITCH_ESP32S3 1
#include "../../src/firmware_console_reboot.cpp"
// Compile the same production owner for the isolated native USB profile too.
// Scheduling/SDK leaves remain controlled; both paths use the real lifecycle
// snapshot and file-backed durable transaction above.
#define ARDUINO_USB_MODE 1
#define SETTINGS_REBOOT_FLUSH_TIMEOUT_MS NATIVE_SETTINGS_REBOOT_FLUSH_TIMEOUT_MS
#define USB_ROM_RAIL_SETTLE_MS NATIVE_USB_ROM_RAIL_SETTLE_MS
#define AbortReboot NativeAbortReboot
#define Restart NativeRestart
#define RequestFactoryBoot NativeRequestFactoryBoot
#define RebootCommand NativeRebootCommand
#define PollPendingReboot NativePollPendingReboot
#define RequestSettingsClear NativeRequestSettingsClear
#define PollSettingsClear NativePollSettingsClear
#include "../../src/firmware_console_reboot.cpp"
#undef PollPendingReboot
#undef RequestSettingsClear
#undef PollSettingsClear
#undef RebootCommand
#undef RequestFactoryBoot
#undef Restart
#undef AbortReboot
#undef USB_ROM_RAIL_SETTLE_MS
#undef SETTINGS_REBOOT_FLUSH_TIMEOUT_MS
#undef ARDUINO_USB_MODE
#undef LITCH_ESP32S3
#include "../../src/firmware_board_storage_recovery.cpp"
#undef SettingsPersistenceWorker
#undef firmware_internal
#undef ARDUINO
#undef LITCH_REBOOT_TEST_STORE

namespace reboot_fixture {
using Art = modules::LitchcraftSession;
using Persistence = modules::LitchcraftPersistence;
struct OwnerResetFixture {
    FirmwareSystem sys;
    test_support::OwnerPairingConsoleTransport transport;
    Console console{transport.hybrid};
    OwnerResetFixture() {
        std::filesystem::create_directories("build");
        for (const char* suffix : {"", ".tmp", ".bak", ".retry"})
            std::remove((std::string(PATH) + suffix).c_str());
        restarts = 0U; fxe::ManualMillis::SetNowMs(100U);
        system = &sys; context.console = &console;
        modules::BoardIdentity identity; identity.bytes.fill(0x42U);
        Check(modules::WriteBoardIdentity(sys.settings.Doc(), identity), "reset fixture identity");
        modules::WriteBoardOwner(sys.settings.Doc(), modules::BoardOwnerRecord::Active);
        sys.settings.Doc()["LED_BROWNOUT_LIMIT_LATCH"] = true;
        sys.settings.Doc()["RGBW_COOL_TO_WARM_THRESHOLD"] = 100U;
        sys.settings.Doc()["BRIGHTNESS"] = 8U;
        sys.settings.Doc()[modules::KEY_GAME_FEN] = "old saved game";
        Check(sys.settings.SaveNowBlocking(), "reset starts with durable owner/game/preferences");
        sys.boardOwnerPersistence.durable = modules::BoardOwnerRecord::Active;
        sys.boardOwnerPairing.Boot(modules::BoardOwnerRecord::Active, {true,1U}, true);
        Check(sys.boardOwnerPairing.RegisterOwnerConnection({12U,31U,71U},
            {{true,1U},true,true,true,true}), "current session proves exact native owner");
        sys.ble.pairing = &sys.boardOwnerPairing;
        console.SetSettingsStore(&sys.settings);
        console.SetSettingsClearCallback([this]() { return RequestSettingsClear(console); });
    }
    void Ble(const char* line) { transport.ble.input += line; console.Update(); }
    void Usb(const char* line) { transport.usb.input += line; console.Update(); }
    void Poll() { Check(PollPendingReboot(sys), "accepted reset stays with existing reboot owner"); }
    void FinishPairing() {
        sys.settingsPersistence.Complete(); Poll();
        Check(sys.ble.nativeDeletes == 0U, "durable Clearing still waits for actual native quiescence");
        sys.ble.nativeProceed = true; Poll(); Poll();
        Check(sys.ble.nativeDeletes == 1U && sys.ble.nativeKeys == 0U,
              "all native keys removed through checked custody effect");
        sys.settingsPersistence.Complete(); Poll();
        Check(sys.boardOwnerPairing.Custody().Pairable() && sys.settingsClearReceipt != 0U,
              "checked Absent and released receipt precede replacement settings");
    }
};
void OwnerConsoleResetJourney() {
    OwnerResetFixture f;
    constexpr const char* artworkPath = "build/test_owner_console_reset_artwork.bin";
    auto* artwork = std::fopen(artworkPath, "wb");
    Check(artwork != nullptr, "separate artwork fixture opens");
    if (artwork != nullptr) { std::fputs("retained artwork",artwork); std::fclose(artwork); }
    f.Usb("settings clear\n");
    f.Ble("settings "); f.Usb("clear\n");
    Check(!f.sys.settingsClearPending && f.sys.settingsPersistence.enqueues == 0U,
          "USB and mixed lines cannot borrow connected owner's remote authority");
    auto stale = modules::BoardOwnerPairingRuntime::Connection{12U,31U,72U};
    Check(!f.sys.boardOwnerPairing.RequestOwnerClear(stale), "stale shared session cannot reset");
    f.Ble("settings clear\n");
    if (!f.sys.settingsClearPending) std::printf("[OWNER_RESET_ADMISSION] %s\n", f.transport.usb.output.c_str());
    Check(f.sys.settingsClearPending && restarts == 0U &&
              f.sys.ble.nativeDeletes == 0U && f.sys.settings.Doc().containsKey(modules::KEY_GAME_FEN),
          "owner Console admits Clearing without raw deletion, early reset or live-game loss");
    const auto accepted = f.sys.settingsPersistence.enqueues;
    f.Ble("settings clear\n");
    Check(f.sys.settingsPersistence.enqueues == accepted, "duplicate clear cannot repeat destructive work");
    fxe::ManualMillis::AdvanceMs(20000U); f.Poll();
    Check(restarts == 0U && f.sys.settingsClearPending,
          "slow storage cannot masquerade as completed reset or time out into reboot");
    f.FinishPairing();
    modules::SettingsStore before(Config()); before.Load();
    Check(before.Doc().containsKey(modules::KEY_GAME_FEN), "old saved game remains until replacement verifies");
    const unsigned beforeResetSaves = f.sys.settingsPersistence.enqueues;
    f.sys.settingsPersistence.Complete(); f.Poll();
    Check(f.sys.boardOwnerPairing.Presentation().cue == modules::BoardOwnerPairingRuntime::PhysicalCue::Waiting,
          "whole reset still presents wait after Absent and first verified settings save");
    Check(restarts == 0U && f.sys.settingsClearPending,
          "verified primary alone cannot complete reset while old backup remains");
    f.Poll();
    Check(f.sys.settingsPersistence.enqueues == beforeResetSaves + 1U,
          "fallback qualification enqueues a second actual transaction");
    f.sys.settingsPersistence.Complete(); f.Poll();
    modules::SettingsStore reopened(Config()); reopened.Load();
    modules::BoardIdentity identity;
    Check(restarts == 1U && !f.sys.settingsClearPending &&
              !reopened.Doc().containsKey(modules::KEY_GAME_FEN) &&
              !reopened.Doc().containsKey("BRIGHTNESS") &&
              modules::ReadBoardOwner(reopened.Doc()) == modules::BoardOwnerRecord::Absent &&
              modules::ReadBoardIdentity(reopened.Doc(), identity) == modules::BoardIdentityRead::Current &&
              identity.bytes[0U] == 0x42U && reopened.ReadValue("LED_BROWNOUT_LIMIT_LATCH", false) &&
              reopened.ReadValue("RGBW_COOL_TO_WARM_THRESHOLD",0U) == 100U,
          "verified reset clears game/preferences/pairing and retains identity/safety before one reboot");
    artwork = std::fopen(artworkPath, "rb");
    char retained[32]{};
    if (artwork != nullptr) { std::fread(retained,1U,sizeof(retained)-1U,artwork); std::fclose(artwork); }
    Check(std::string(retained) == "retained artwork" && f.sys.settingsPersistence.formats == 0U &&
              !f.sys.settingsPersistence.clearPending,
          "settings reset preserves separate artwork bytes without format or Store clear");
    std::remove(artworkPath);
    std::puts("[OWNER_RESET_JOURNEY] owner-Console -> Clearing -> native-quiesce/delete -> Absent -> verified-settings -> one-reboot");
}
void PartialRemoteLineCannotAuthorizeReplacementOwner() {
    OwnerResetFixture f;
    const uint64_t admitted = f.transport.ble.generation;
    f.transport.ble.generation = 0U;
    f.Ble("settings clear\n");
    Check(!f.sys.settingsClearPending && f.sys.settingsPersistence.enqueues == 0U,
          "unknown/default-zero remote lease cannot authorize reset");
    f.transport.ble.generation = admitted;
    f.Ble("settings clear");
    f.sys.boardOwnerPairing.Disconnected(f.sys.ble.owner);
    f.transport.ble.generation = 0U;
    f.console.Update();
    f.sys.ble.owner = {17U,55U,91U};
    f.sys.boardOwnerPairing.Boot(modules::BoardOwnerRecord::Active,{true,1U},true);
    Check(f.sys.boardOwnerPairing.RegisterOwnerConnection(f.sys.ble.owner,
        {{true,1U},true,true,true,true}), "replacement owner authenticates new exact connection");
    f.transport.ble.generation = (uint64_t{55U} << 32U) | 91U;
    f.Ble("\n");
    Check(!f.sys.settingsClearPending && f.sys.settingsPersistence.enqueues == 0U,
          "old owner's partial destructive command cannot execute on a later owner's newline");
    if (f.sys.settingsClearPending) return;
    f.Ble("settings clear\n");
    Check(f.sys.settingsClearPending && f.sys.settingsPersistence.enqueues == 1U,
          "fresh whole command from current owner remains admitted");
}
void ResetFallbackCannotResurrectOldGame() {
    OwnerResetFixture f;
    f.Ble("settings clear\n"); f.FinishPairing();
    f.sys.settingsPersistence.Complete(); f.Poll();
    if (f.sys.settingsClearPending) { f.Poll(); f.sys.settingsPersistence.Complete(); f.Poll(); }
    for (const bool backupFault : {false,true}) {
        for (const bool corrupt : {false,true}) {
            const std::string path = std::string(PATH) + ".fallback-case";
            for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) std::remove((path+suffix).c_str());
            std::filesystem::copy_file(PATH,path);
            std::filesystem::copy_file(std::string(PATH)+".bak",path+".bak");
            const std::string fault = path + (backupFault ? ".bak" : "");
            if (corrupt) {
                auto* file = std::fopen(fault.c_str(),"wb");
                if (file != nullptr) { std::fputs("invalid JSON",file); std::fclose(file); }
            } else std::remove(fault.c_str());
            auto cfg = Config(); cfg.path = path.c_str();
            modules::SettingsStore reopened(cfg);
            Check(reopened.Load() && !reopened.Doc().containsKey(modules::KEY_GAME_FEN) &&
                      !reopened.Doc().containsKey("BRIGHTNESS") &&
                      modules::ReadBoardOwner(reopened.Doc()) == modules::BoardOwnerRecord::Absent,
                  "fresh load with missing/corrupt primary or backup cannot resurrect old game/preferences");
            for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) std::remove((path+suffix).c_str());
        }
    }
}

void OwnerConsoleResetFailureTruth() {
    for (const bool nativeFailure : {true,false}) {
        OwnerResetFixture f;
        f.Ble("settings clear\n");
        if (nativeFailure) {
            f.sys.settingsPersistence.Complete(); f.Poll();
            f.sys.ble.nativeProceed = true; f.sys.ble.nativeFailure = true;
            f.Poll(); f.Poll();
        } else {
            f.FinishPairing(); f.sys.settingsPersistence.Complete(false); f.Poll();
        }
        modules::SettingsStore reopened(Config()); reopened.Load();
        const auto presentation = f.sys.boardOwnerPairing.Presentation();
        Check(presentation.state == modules::BoardOwnerPairingRuntime::State::Failed &&
                  !presentation.active &&
                  presentation.cue == modules::BoardOwnerPairingRuntime::PhysicalCue::None &&
                  std::string_view(presentation.text).empty() &&
                  std::string_view(presentation.nextAction) == "ordinary-gameplay",
              "failed replacement retains app failure while releasing local text, LEDs and controls for play");
        Check(restarts == 0U && !f.sys.settingsClearPending &&
                  reopened.Doc().containsKey(modules::KEY_GAME_FEN) &&
                  (f.transport.usb.output.find("reset incomplete") != std::string::npos ||
                   (nativeFailure && f.sys.boardOwnerPairing.Custody().GetPhase() ==
                       modules::BoardOwnerPairingController::Phase::Failed)),
              "native or replacement-save failure preserves saved game and cannot reboot/claim reset success");
    }
}

void Send(Art& art, Art::Op op, uint16_t id,
          std::initializer_list<uint8_t> payload = {}) {
    Art::Packet packet;
    packet.bytes[0] = Art::MAGIC; packet.bytes[1] = uint8_t(op);
    const uint32_t session = op == Art::Op::Enter ? 0U : art.SessionId();
    for (unsigned i = 0U; i < 4U; ++i) packet.bytes[2U + i] = session >> (i * 8U);
    packet.bytes[6] = id; packet.bytes[7] = id >> 8U; packet.size = 8U;
    for (const auto byte : payload) packet.bytes[packet.size++] = byte;
    art.HandlePacket(packet.bytes.data(), packet.size, true, 0U);
    while (art.PeekPacket(packet)) art.AcceptPacket();
}
struct Fixture {
    FirmwareSystem sys;
    MemoryConsoleTransport transport;
    Console console{transport};
    std::string output;
    bool accepted = false;
    bool nativeHostEntry = false;
    Fixture() {
        std::filesystem::create_directories("build");
        for (const char* suffix : {"", ".tmp", ".bak"}) {
            std::remove((std::string(PATH) + suffix).c_str());
        }
        fxe::ManualMillis::SetNowMs(0U); restarts = 0U; bootloaderRestarts = 0U;
        rebootEvents.clear();
        Check(sys.settings.Load(), "initial settings load");
        Check(sys.gamePersistence.LoadOrInitialize(), "real GPC initializes game snapshot");
        sys.gamePersistence.Litchcraft().Configure(3U, 1000U);
        sys.gamePersistence.Litchcraft().BeginConnection();
        ConfigureLitchcraftPersistence(sys);
        system = &sys; context.console = &console;
        transport.SetOutputCallback([this](const char* bytes, size_t size) { output.append(bytes, size); });
        console.RegisterCommand("reboot", "test production reboot", [this](const Console::Args& args) {
            accepted = nativeHostEntry
                ? console_commands::NativeRebootCommand(console, args)
                : console_commands::RebootCommand(console, args);
            return accepted;
        });
        Send(sys.gamePersistence.Litchcraft(), Art::Op::Enter, 1U);
        fxe::ManualMillis::SetNowMs(200U);
        sys.gamePersistence.UpdateLitchcraft();
        sys.settingsPersistence.Complete(); sys.gamePersistence.UpdateLitchcraft();
        Send(sys.gamePersistence.Litchcraft(), Art::Op::Paint, 2U, {1, 0, 3, 0});
        Check(sys.gamePersistence.ArtPersistence().IsDirty(), "paint before five-second idle is dirty");
        sys.settingsPersistence.enqueues = 0U;
    }
    void Command(const char* line = "reboot") {
        transport.PushBytes(line, std::char_traits<char>::length(line));
        transport.PushChar('\n'); console.Update();
    }
    bool Poll() { return nativeHostEntry ? NativePollPendingReboot(sys) : PollPendingReboot(sys); }
    Persistence& Owner() { return sys.gamePersistence.ArtPersistence(); }
};
void FactoryBootAdmissionPreservesPendingStorage() {
    for (const auto intent : {firmware::FactoryBootIntent::Test,
                              firmware::FactoryBootIntent::Verifier}) {
        Fixture f;
        f.sys.otaBootAwaitingHealth = true;
        Check(!RequestFactoryBoot(f.sys, intent) &&
                  !f.sys.rebootPersistencePending &&
                  f.sys.pendingFactoryBootIntent == firmware::FactoryBootIntent::None &&
                  restarts == 0U,
              "factory boot cannot arm RTC intent before OTA trial health acceptance");
        f.sys.ble.PublishOtaRuntimeAccepted();
        Check(RequestFactoryBoot(f.sys, intent) && f.sys.rebootPersistencePending &&
                  f.sys.pendingFactoryBootIntent == intent && restarts == 0U,
              "accepted OTA trial releases factory boot admission without clearing initial boot state");
    }
    for (const char* pendingOwner : {"factory-result", "factory-clear"}) {
        Fixture f;
        hardwareTestSavePending = true; // External real receipt owner is in the manufacturing suite.
        Check(!RequestFactoryBoot(f.sys, firmware::FactoryBootIntent::None) &&
                  !f.sys.rebootPersistencePending &&
                  f.sys.pendingFactoryBootIntent == firmware::FactoryBootIntent::None &&
                  restarts == 0U,
              "actual clean-boot owner rejects off during pending factory storage without intent");
        std::printf("[FACTORY_BOOT_ADMISSION] pending=%s accepted=0 intent=none reset=0\n", pendingOwner);
        hardwareTestSavePending = false;
    }
    {
        Fixture f;
        f.sys.rebootPersistencePending = true;
        hardwareTestSavePending = true;
        hardwareTestReceiptReady = false;
        hardwareTestReceiptPolls = 0U;
        Check(f.Poll() && hardwareTestReceiptPolls == 1U &&
                  !f.sys.rebootPersistenceStarted && restarts == 0U,
              "actual pending reboot keeps polling the factory receipt instead of starving its owner");
        hardwareTestReceiptReady = true;
        Check(f.Poll() && hardwareTestReceiptPolls == 2U &&
                  !hardwareTestSavePending && f.sys.rebootPersistenceStarted,
              "completed factory receipt releases normal lifecycle flush while reboot holds the loop");
        hardwareTestReceiptReady = false;
    }
    for (const auto intent : {firmware::FactoryBootIntent::None,
                              firmware::FactoryBootIntent::Test,
                              firmware::FactoryBootIntent::Verifier}) {
        Fixture f;
        Check(RequestFactoryBoot(f.sys, intent) && f.sys.rebootPersistencePending &&
                  !f.sys.rebootPersistenceStarted && f.sys.pendingFactoryBootIntent == intent &&
                  !f.sys.factoryBootOnly && !f.sys.hardwareTest.IsActive() && restarts == 0U,
              "actual idle clean-boot admission records intent without initializing factory or delivering reset");
    }
}
void BootloaderWaitsForExactDurableSave() {
    Fixture f;
    f.Command("reboot bootloader");
    f.Poll();
    Check(f.accepted && f.sys.rebootPersistencePending && restarts == 0U &&
              bootloaderRestarts == 0U && f.sys.settingsPersistence.retained != 0U,
          "governed ROM request retains the existing exact save receipt");
    fxe::ManualMillis::AdvanceMs(9000U); f.Poll();
    Check(restarts == 0U && bootloaderRestarts == 0U,
          "delayed accepted save cannot race ROM entry");
    f.sys.settingsPersistence.Complete(); f.Poll();
    Check(restarts == 0U && bootloaderRestarts == 1U &&
              f.output.find("reboot bootloader ready: state saved; entering ROM") != std::string::npos,
          "only exact durable save permits one acknowledged bootloader destination");
    Check(rebootEvents == "CSPEB" && !f.sys.powerRail.enabled &&
              f.sys.ledAnimation.IsQuiescent() &&
              f.sys.powerRail.disables == 1U && !f.sys.rebootToBootloader,
          "durable ROM entry clears once, closes the rail, settles, then invokes SDK once");
}
void NativeBootloaderRejectsBeforeChangingRuntime() {
    Fixture f;
    f.nativeHostEntry = true;
    std::string before;
    serializeJson(f.sys.settings.Doc(), before);
    const auto revision = f.sys.settings.GetRevision();
    const auto session = f.sys.gamePersistence.Litchcraft().SessionId();
    f.Command("reboot bootloader");
    Check(!f.accepted && !f.sys.rebootPersistencePending &&
              !f.sys.rebootToBootloader && f.sys.settingsPersistence.enqueues == 0U,
          "unqualified native bootloader request rejects before admitting any save or hold");
    // On the old implementation, finish the accepted save to reproduce the
    // actual stranded rail-off state rather than only checking the reply.
    if (f.sys.rebootPersistencePending) {
        f.Poll();
        if (f.sys.settingsPersistence.retained != 0U) f.sys.settingsPersistence.Complete();
        f.Poll();
    }
    fxe::ManualMillis::AdvanceMs(60000U);
    Check(!f.Poll(), "rejected native entry cannot block normal input and BLE lifecycle servicing");
    std::string after;
    serializeJson(f.sys.settings.Doc(), after);
    Check(after == before && f.sys.settings.GetRevision() == revision &&
              f.sys.gamePersistence.Litchcraft().SessionId() == session && f.Owner().IsDirty() &&
              f.sys.powerRail.enabled && f.sys.powerRail.disables == 0U && rebootEvents.empty() &&
              restarts == 0U && bootloaderRestarts == 0U && f.sys.bootGuard.healthy == 0U &&
              f.output.find("reboot bootloader rejected: native USB entry unavailable") != std::string::npos &&
              f.output.find("reboot bootloader ready:") == std::string::npos,
          "rejected native entry preserves live state and LEDs without promising a handoff or reset");
}
void NativeBootloaderDestinationCannotQuiesceOrReset() {
    Fixture f;
    f.nativeHostEntry = true;
    f.sys.rebootToBootloader = true;
    NativeRestart(f.console, f.sys, false);
    Check(!f.sys.rebootToBootloader && !f.sys.rebootPersistencePending &&
              f.sys.powerRail.enabled && f.sys.powerRail.disables == 0U && rebootEvents.empty() &&
              restarts == 0U && bootloaderRestarts == 0U && f.sys.bootGuard.healthy == 0U &&
              f.output.find("reboot bootloader rejected: native USB entry unavailable") != std::string::npos,
          "a stale native bootloader destination also rejects before rail or SDK effects");
}
void NativeApplicationRebootStillDrainsSavedState() {
    Fixture f;
    f.nativeHostEntry = true;
    f.Command(); f.Poll();
    Check(f.accepted && f.sys.rebootPersistencePending &&
              f.sys.settingsPersistence.retained != 0U && restarts == 0U,
          "ordinary native application reboot retains the exact durable-save owner");
    f.sys.settingsPersistence.Complete(); f.Poll();
    Check(!f.sys.rebootPersistencePending && !f.Owner().IsDirty() &&
              f.sys.settingsPersistence.retained == 0U && f.sys.powerRail.enabled &&
              rebootEvents.empty() && restarts == 1U && bootloaderRestarts == 0U,
          "ordinary native application reboot saves then invokes one normal restart");
}
void BootloaderFailureNeverCutsPowerOrResets() {
    for (unsigned mode = 0U; mode < 3U; ++mode) {
        Fixture f;
        f.Command("reboot bootloader"); f.Poll();
        if (mode == 0U) f.sys.settingsPersistence.Complete(false);
        if (mode == 1U) fxe::ManualMillis::AdvanceMs(10000U);
        if (mode == 2U) {
            f.sys.leds.complete = false;
            f.sys.settingsPersistence.Complete();
        }
        f.Poll();
        Check(!f.sys.rebootPersistencePending && !f.sys.rebootToBootloader &&
                  restarts == 0U && bootloaderRestarts == 0U &&
                  f.sys.powerRail.enabled && f.sys.powerRail.disables == 0U &&
                  f.output.find("reboot bootloader aborted:") != std::string::npos &&
                  f.output.find("reboot bootloader ready:") == std::string::npos,
              "failed save, bounded save timeout or denied LED output rejects ROM without rail cut/reset");
    }
    Fixture f;
    f.sys.settingsPersistence.storeSave.dirty = true;
    f.Command("reboot bootloader"); f.Poll();
    f.sys.settingsPersistence.Complete(); f.Poll();
    Check(bootloaderRestarts == 0U && f.sys.powerRail.enabled && rebootEvents.empty(),
          "settings receipt cannot lend durability to an accepted pending Store edit");
    f.sys.settingsPersistence.storeSave.dirty = false; f.Poll();
    Check(bootloaderRestarts == 1U && rebootEvents == "CSPEB",
          "final Store commit releases the same governed ROM request");
}
void BootloaderAdmissionRejectsUnsafeContexts() {
    for (unsigned mode = 0U; mode < 11U; ++mode) {
        Fixture f;
        if (mode == 0U) f.sys.settings.DisablePersistenceForSession();
        if (mode == 1U) f.sys.safeModeActive = true;
        if (mode == 2U) f.sys.factoryBootOnly = true;
        if (mode == 3U) f.sys.settingsPersistence.started = false;
        if (mode == 4U) f.sys.ble.ota = true;
        if (mode == 5U) f.sys.sleep.state = firmware::SleepState::Preparing;
        if (mode == 6U) f.sys.hardwareTest.active = true;
        if (mode == 7U) f.sys.hardwareTest.overridePending = true;
        if (mode == 8U) f.sys.comptest.active = true;
        if (mode == 9U) f.sys.otaBootAwaitingHealth = true;
        f.Command(mode == 10U ? "reboot bootloader nosave" : "reboot bootloader");
        Check(!f.accepted && !f.sys.rebootPersistencePending && !f.sys.rebootToBootloader &&
                  restarts == 0U && bootloaderRestarts == 0U &&
                  f.sys.settingsPersistence.enqueues == 0U && rebootEvents.empty() &&
                  f.output.find("reboot bootloader rejected:") != std::string::npos,
              "unsafe runtime, hardware/storage owner and extra/nosave arguments reject before persistence/ROM mutation");
    }
    Fixture f;
    f.Command();
    f.Command("reboot bootloader");
    Check(!f.accepted && f.sys.rebootPersistencePending && !f.sys.rebootToBootloader &&
              f.sys.settingsPersistence.enqueues == 0U,
          "bootloader request cannot replace a previously admitted application reboot");
}
void RecoveryAndSessionOnlySleep() {
    FirmwareSystem sys;
    Check(sys.settings.Load() && sys.gamePersistence.LoadOrInitialize(), "recovery adapter starts live production game");
    sys.settings.Doc()["RAM_MARKER"] = 19U;
    modules::ControlMutationCallbacks callbacks;
    callbacks.context = &sys;
    callbacks.requestStorageReformat = &RequestBoardStorageRecovery;
    callbacks.pollStorageReformat = &PollBoardStorageRecovery;
    sys.controlMutation.Configure(sys.settings, callbacks);
    using Status = fxe::Console::StorageReformatStatus;
    using State = Worker::StorageReformatState;
    Check(BoardStorageRecoveryStatus(sys) == Status::Unavailable &&
              ConfirmBoardStorageRecovery(sys) == Status::Unavailable &&
              sys.settingsPersistence.formats == 0U && !sys.settings.IsDisabled(),
          "healthy or merely unsaved state cannot authorize destructive recovery");
    sys.settings.ReportAsyncSaveFailure(fxe::SettingsStoreError::VerifyFailed,
        fxe::SettingsStore::StorageFailureEvidence::Confirmed);
    Check(BoardStorageRecoveryStatus(sys) == Status::Idle, "recovery initially idle");
    sys.ble.ota = true;
    Check(ConfirmBoardStorageRecovery(sys) == Status::Busy && sys.settingsPersistence.formats == 0U,
          "existing OTA refuses recovery without erasure or RAM disable");
    sys.ble.ota = false;
    const auto admitted = sys.controlMutation.MutateAction(modules::ControlAction::ReformatStorage, 991U);
    sys.controlMutation.Update();
    Check(admitted.status == modules::ControlMutationStatus::Accepted && sys.settings.IsDisabled() &&
              sys.settingsPersistence.formats == 1U && BoardStorageRecoveryBlocksOta(&sys),
          "real control adapter admits exactly one recovery and blocks reverse OTA Begin");
    Check(BeginPersistenceBeforeSleep(&sys) &&
              PollPersistenceBeforeSleep(&sys) == firmware::SleepPersistenceStatus::Pending,
          "explicit session-only sleep waits for pending format owner");
    sys.settingsPersistence.reformat = State::Running;
    Check(BoardStorageRecoveryBlocksOta(&sys) &&
              ConfirmBoardStorageRecovery(sys) == Status::Running && sys.settingsPersistence.formats == 1U,
          "running recovery retains owner and cannot erase twice");
    fxe::ManualMillis::AdvanceMs(10000U);
    Check(PollPersistenceBeforeSleep(&sys) == firmware::SleepPersistenceStatus::Failed,
          "bounded explicit sleep timeout cannot tear down active storage");
    sys.settingsPersistence.reformat = State::Failed;
    sys.controlMutation.Update();
    modules::ControlMutationOutcome terminal;
    Check(sys.controlMutation.TryTakeCompletion(991U, terminal) &&
              terminal.reason == modules::ControlMutationReason::StorageUnavailable &&
              !sys.controlMutation.TryTakeCompletion(991U, terminal), "failed exact recovery action completes once");
    PublishBoardStorageRecovery(sys);
    Check(sys.ble.recovery[1U] == uint8_t(Status::Failed) && sys.ble.recovery[2U] == 0U,
          "failed format publishes reliable failure without restart-success claim");
    const uint32_t firstFailureRevision = sys.ble.recoveryRevision;
    const auto firstFailureBody = sys.ble.recovery;
    const uint32_t firstAttempt = sys.storageRecoveryPublication.attempt;
    PublishBoardStorageRecovery(sys);
    Check(sys.ble.recoveryRevision == firstFailureRevision,
          "ordinary retained status republish does not invent an attempt");
    sys.ble.ota = true;
    Check(ConfirmBoardStorageRecovery(sys) == Status::Failed &&
              sys.storageRecoveryPublication.attempt == firstAttempt,
          "blocked confirmation cannot reserve a fresh destructive attempt");
    PublishBoardStorageRecovery(sys);
    Check(sys.ble.recoveryRevision == firstFailureRevision,
          "refused admission with unchanged status retains the same revision");
    sys.ble.ota = false;
    Check(ConfirmBoardStorageRecovery(sys) == Status::Pending && sys.settingsPersistence.formats == 2U,
          "new explicit confirmation releases known failure and retries once");
    // The accepted worker can fail before main publishes Pending/Running.
    sys.settingsPersistence.reformat = State::Failed;
    PublishBoardStorageRecovery(sys);
    Check(sys.ble.recovery == firstFailureBody &&
              static_cast<int32_t>(sys.ble.recoveryRevision - firstFailureRevision) > 0,
          "second immediate failure advances terminal revision despite identical wire body");
    Check(ConfirmBoardStorageRecovery(sys) == Status::Pending && sys.settingsPersistence.formats == 3U,
          "known second failure still permits another fresh confirmation");
    sys.settingsPersistence.reformat = State::Succeeded;
    PublishBoardStorageRecovery(sys);
    Check(ConfirmBoardStorageRecovery(sys) == Status::Succeeded && sys.settingsPersistence.formats == 3U &&
              sys.ble.recovery[1U] == uint8_t(Status::Succeeded) && sys.ble.recovery[2U] == 1U &&
              sys.settings.Doc()["RAM_MARKER"].as<unsigned>() == 19U && sys.settings.IsDisabled(),
          "verified terminal truth is retained without replay or changing RAM session");
    Check(BeginPersistenceBeforeSleep(&sys) &&
              PollPersistenceBeforeSleep(&sys) == firmware::SleepPersistenceStatus::Succeeded &&
              sys.settingsPersistence.enqueues == 0U, "idle RAM-only sleep succeeds without fake saved receipt");
    firmware::SleepController controller;
    Check(controller.RequestSleep(firmware::SleepRequestOrigin::Console, fxe::Millis(), false),
          "real manual request enters RAM-only preparation");
    fxe::ManualMillis::AdvanceMs(3500U);
    controller.Update(sys.settings, fxe::Millis(), firmware::SleepSuppression::Allow);
    firmware::SleepPreparationHooks hooks{&sys, &BeginPersistenceBeforeSleep,
        &PollPersistenceBeforeSleep, [](void*) { return true; }, [](void*) { return true; }};
    Check(controller.GetState() == firmware::SleepState::Preparing &&
              controller.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::Pending &&
              controller.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::Authorized &&
              controller.GetState() == firmware::SleepState::Sleeping,
          "common preparation after the fade and disabled persistence hooks authorize explicit power-off");
    sys.settingsPersistence.clearPending = true;
    Check(BoardStorageRecoveryBlocksOta(&sys), "existing Store clear OTA exclusion retained");
}

void EarlyBootSleepWaitsForHealthJourney() {
    for (const bool finishHealth : {true, false}) {
        Fixture f;
        f.sys.runtimeBootHealth.commitFinished = false;
        firmware::SleepController sleep;
        const uint32_t requestedAt = fxe::Millis();
        Check(sleep.RequestSleep(firmware::SleepRequestOrigin::Menu, requestedAt, false),
              "early Setup Sleep request is accepted once");
        const auto update = [&](const uint32_t elapsed) {
            fxe::ManualMillis::SetNowMs(requestedAt + elapsed);
            const bool suppressed = firmware::IsSleepSuppressedByHigherPriority(
                {.otaHealthConfirmationPending = !f.sys.runtimeBootHealth.commitFinished},
                sleep.GetState() == firmware::SleepState::Awake ? firmware::SleepRequestOrigin::None
                                                               : sleep.GetRequestOrigin());
            sleep.Update(f.sys.settings, fxe::Millis(), suppressed
                ? firmware::SleepSuppression::HigherPriority : firmware::SleepSuppression::Allow);
        };
        update(0U);
        Check(sleep.GetState() == firmware::SleepState::Countdown,
              "startup health does not silently cancel a directed request");
        update(3250U);
        Check(sleep.GetState() == firmware::SleepState::Fading && sleep.GetFadeScale256() == 128U,
              "early request still completes the shared visible fade");
        update(3500U);
        firmware::SleepPreparationHooks hooks{&f.sys, &BeginPersistenceBeforeSleep,
            &PollPersistenceBeforeSleep, [](void*) { return true; }, [](void*) { return true; }};
        Check(sleep.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::Pending,
              "early request begins the real bounded lifecycle save once");
        f.sys.gamePersistence.UpdateLitchcraft();
        Check(f.sys.settingsPersistence.enqueues == 1U, "existing worker owns one frozen lifecycle snapshot");
        f.sys.settingsPersistence.Complete();
        f.sys.gamePersistence.UpdateLitchcraft();
        Check(sleep.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::Pending &&
                  sleep.GetState() == firmware::SleepState::Preparing,
              "even a saved snapshot cannot authorize teardown before startup health");
        if (finishHealth) {
            f.sys.runtimeBootHealth.commitFinished = true;
            update(5000U);
            Check(sleep.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::Authorized &&
                      sleep.ConsumeSleepAuthorization() && !sleep.ConsumeSleepAuthorization() &&
                      f.sys.settingsPersistence.enqueues == 1U,
                  "existing readiness completion releases the original request and save exactly once");
        } else {
            update(13499U);
            Check(sleep.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::Pending,
                  "startup wait shares the existing save deadline through 9999ms");
            update(13500U);
            Check(sleep.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::Failed &&
                      sleep.GetState() == firmware::SleepState::Awake && !sleep.ConsumeSleepAuthorization(),
                  "startup timeout restores awake without teardown or an indefinite black wait");
        }
        std::printf("[EARLY_BOOT_SLEEP] health_completed=%u origin=Menu save_receipts=%u state=%u teardown_authorized=%u deadline_ms=10000\n",
            finishHealth, f.sys.settingsPersistence.enqueues, unsigned(sleep.GetState()),
            sleep.GetState() == firmware::SleepState::Sleeping);
    }
}

void RecoveryWaitsForVerifiedBootHealth() {
    FirmwareSystem sys;
    Check(sys.settings.Load() && sys.gamePersistence.LoadOrInitialize(),
          "boot-health recovery starts the live production game");
    sys.settings.Doc()["RAM_MARKER"] = 23U;
    std::string before;
    serializeJson(sys.settings.Doc(), before);
    modules::ControlMutationCallbacks callbacks;
    callbacks.context = &sys;
    callbacks.requestStorageReformat = &RequestBoardStorageRecovery;
    callbacks.pollStorageReformat = &PollBoardStorageRecovery;
    sys.controlMutation.Configure(sys.settings, callbacks);
    using Status = fxe::Console::StorageReformatStatus;
    sys.settings.ReportAsyncSaveFailure(fxe::SettingsStoreError::VerifyFailed,
        fxe::SettingsStore::StorageFailureEvidence::Confirmed);
    Check(BoardStorageRecoveryStatus(sys) == Status::Idle,
          "non-pending valid boot does not require a new acceptance flag");
    sys.otaBootAwaitingHealth = true;
    PublishBoardStorageRecovery(sys);
    Check(sys.ble.recovery[1U] == uint8_t(Status::Busy) &&
              sys.ble.recovery[2U] == 0U,
          "pending boot health publishes Busy without reboot success");
    Check(ConfirmBoardStorageRecovery(sys) == Status::Busy,
          "explicit confirmation cannot erase unresolved boot authority");
    const auto action = sys.controlMutation.MutateAction(
        modules::ControlAction::ReformatStorage, 990U);
    sys.controlMutation.Update();
    modules::ControlMutationOutcome terminal;
    Check(action.status == modules::ControlMutationStatus::Accepted &&
              sys.controlMutation.TryTakeCompletion(990U, terminal) &&
              terminal.reason == modules::ControlMutationReason::Busy,
          "real Ordered action completes with pending-boot Busy refusal");
    std::string after;
    serializeJson(sys.settings.Doc(), after);
    Check(sys.settingsPersistence.formats == 0U &&
              sys.storageRecoveryPublication.attempt == 0U &&
              !sys.settings.IsDisabled() && before == after,
          "boot-health refusal preserves RAM, persistence and erase admission");
    sys.ble.PublishOtaRuntimeAccepted();
    Check(sys.otaBootAwaitingHealth &&
              BoardStorageRecoveryStatus(sys) == Status::Idle,
          "verified running-image acceptance releases boot-health exclusion");
    Check(ConfirmBoardStorageRecovery(sys) == Status::Pending &&
              sys.settingsPersistence.formats == 1U,
          "fresh consent admits one reformat after verified acceptance");
}

void SessionOnlyReboot() {
    Fixture f;
    Check(f.sys.settings.SaveNowBlocking(), "RAM-only reboot fixture has verified baseline");
    const uint32_t saved = f.sys.settings.GetSaveCount();
    f.sys.settings.Doc()["UNSAVED_RAM_VALUE"] = 29U;
    f.sys.settings.ReportAsyncSaveFailure(fxe::SettingsStoreError::PreparationFailed);
    Check(!f.sys.settings.HasConfirmedStorageFailure(),
          "session-only reboot retains unsaved consequences without invented flash evidence");
    f.sys.settings.DisablePersistenceForSession();
    f.Command();
    Check(restarts == 1U && !f.sys.rebootPersistencePending &&
              f.sys.settingsPersistence.enqueues == 0U &&
              f.sys.settings.GetSaveCount() == saved &&
              f.output.find("discards this session") != std::string::npos,
          "explicit RAM-only reboot works without false saved acknowledgement");
    modules::SettingsStore disk(Config()); Check(disk.Load(), "reboot fixture baseline remains readable");
    Check(!disk.Doc().containsKey("UNSAVED_RAM_VALUE"),
          "session-only reboot returns the last verified persisted state");
}
void PendingAndLatestSnapshot() {
    Fixture f;
    f.Command(); f.Command();
    Check(f.accepted && f.sys.rebootPersistencePending && restarts == 0U &&
              f.sys.settingsPersistence.enqueues == 0U && f.output.find("rebooting") == std::string::npos,
          "repeated command queues one pending reboot without success or early snapshot");
    Send(f.sys.gamePersistence.Litchcraft(), Art::Op::Paint, 3U, {2, 0, 3, 7});
    f.sys.settings.Doc()["CONSOLE_BATCH_VALUE"] = 19U;
    Check(f.Poll() && f.sys.settingsPersistence.enqueues == 1U && restarts == 0U,
          "first poll queues one latest snapshot after console input batch");
    for (unsigned i = 0U; i < 3U; ++i) { f.Command(); f.Poll(); }
    Check(f.sys.settingsPersistence.enqueues == 1U && restarts == 0U,
          "pending worker receipt prevents duplicate flush and restart");
    f.sys.settingsPersistence.Complete(); f.Poll();
    Check(restarts == 1U && !f.sys.rebootPersistencePending && !f.Owner().IsDirty() &&
              f.output.find("rebooting") != std::string::npos,
          "only Saved receipt permits reboot success and reset");
    modules::SettingsStore reload(Config()); Check(reload.Load(), "fresh process reloads file");
    modules::LitchcraftRecord saved;
    Check(modules::ReadLitchcraftRecord(reload.Doc(), saved) == modules::LitchcraftRecordStatus::Valid &&
              saved.active && saved.canvas[0].r != 0U && saved.canvas[28].r != 0U &&
              reload.ReadValue("CONSOLE_BATCH_VALUE", 0U) == 19U,
          "latest artwork and following console settings survive actual file reload");
    Check(!f.Poll() && restarts == 1U, "completed reboot transition cannot reset twice");
}
void FailureAndRetry() {
    for (unsigned mode = 0U; mode < 3U; ++mode) {
        Fixture f;
        if (mode == 1U) f.sys.settingsPersistence.admission = Worker::Admission::Rejected;
        f.Command();
        if (mode == 2U) fxe::ManualMillis::SetNowMs(6200U);
        f.Poll();
        if (mode == 0U) { f.sys.settingsPersistence.Complete(false); f.Poll(); }
        if (mode == 2U) {
            Check(f.sys.rebootPersistencePending, "long console batch does not consume worker save deadline");
            fxe::ManualMillis::SetNowMs(16199U); f.Poll();
            Check(f.sys.rebootPersistencePending, "9999ms after first poll retains pending reboot");
            fxe::ManualMillis::SetNowMs(16200U); f.Poll();
        }
        Check(!f.sys.rebootPersistencePending && restarts == 0U && f.Owner().IsDirty() &&
                  f.output.find("reboot aborted") != std::string::npos &&
                  f.output.find("rebooting") == std::string::npos,
              "failed rejected or timed-out save aborts reset and retains dirty artwork");
        if (mode == 2U) {
            f.Command(); f.Poll();
            Check(!f.sys.rebootPersistencePending && restarts == 0U,
                  "retry cannot borrow an older still-pending explicit flush");
            f.sys.settingsPersistence.Complete(false); f.sys.gamePersistence.UpdateLitchcraft();
        }
        f.sys.settingsPersistence.admission = Worker::Admission::Accepted;
        f.Command(); f.Poll(); f.sys.settingsPersistence.Complete(); f.Poll();
        Check(restarts == 1U && !f.Owner().IsDirty(), "explicit retry saves dirty artwork before reboot");
    }
}
void StoreEditMustFinishBeforeReboot() {
    Fixture f;
    f.sys.settingsPersistence.storeSave.dirty = true;
    f.Command(); f.Poll();
    Check(f.sys.settingsPersistence.storeSave.immediate &&
              f.sys.rebootPersistencePending && restarts == 0U,
          "reboot requests immediate Store save without five-second delay");
    f.sys.settingsPersistence.Complete(); f.Poll();
    Check(f.sys.rebootPersistencePending && restarts == 0U,
          "settings receipt alone cannot reboot before Store edit commits");
    f.sys.settingsPersistence.storeSave.dirty = false; f.Poll();
    Check(restarts == 1U && !f.sys.rebootPersistencePending,
          "Store commit and settings receipt together permit reboot");
}
void FailedStoreEditAbortsReboot() {
    Fixture f;
    f.sys.settingsPersistence.storeSave.dirty = true;
    f.Command(); f.Poll();
    f.sys.settingsPersistence.Complete();
    fxe::ManualMillis::SetNowMs(10200U); f.Poll();
    Check(restarts == 0U && !f.sys.rebootPersistencePending &&
              f.output.find("reboot aborted") != std::string::npos,
          "uncommitted Store edit times out without discarding artwork");
}
void ExistingFlushCannotAuthorizeReboot() {
    Fixture f;
    f.Owner().RequestFlush(); f.sys.gamePersistence.UpdateLitchcraft();
    const uint32_t oldReceipt = f.sys.settingsPersistence.retained;
    f.Command(); f.Poll();
    Check(!f.sys.rebootPersistencePending && restarts == 0U &&
              f.sys.settingsPersistence.enqueues == 1U,
          "pending explicit flush cannot lend its prepared snapshot to a newer reboot");
    f.sys.settingsPersistence.Complete(); f.sys.gamePersistence.UpdateLitchcraft();
    Check(!f.Poll() && restarts == 0U, "old flush Saved cannot authorize an aborted reboot");
    Send(f.sys.gamePersistence.Litchcraft(), Art::Op::Paint, 3U, {2, 0, 3, 7});
    f.Command(); f.Poll();
    Check(f.sys.settingsPersistence.retained != oldReceipt &&
              f.sys.settingsPersistence.enqueues == 2U && restarts == 0U,
          "retry after old terminal outcome acquires a new exact receipt");
    f.sys.settingsPersistence.Complete(); f.Poll();
    Check(restarts == 1U && !f.Owner().IsDirty(), "new reboot receipt saves the newer artwork before reset");
}
void ArtworkSurvivesRepeatedGamePreparation() {
    Fixture f;
    for (uint8_t square = 0U; square < 64U; ++square) {
        Send(f.sys.gamePersistence.Litchcraft(), Art::Op::Paint,
             3U + square, {2, 0, 3, square});
    }
    Check(f.sys.gamePersistence.Litchcraft().Artwork()[255].r != 0U,
          "complete-board paint fixture reaches the final semantic emitter");
    f.Owner().RequestFlush(); f.sys.gamePersistence.UpdateLitchcraft();
    f.sys.settingsPersistence.Complete(); f.sys.gamePersistence.UpdateLitchcraft();
    std::array<char, modules::GAME_PERSISTENCE_COMPACT_CAPACITY> scratch{};
    Check(modules::CompactDocRemovingGameState(f.sys.settings.Doc(), scratch.data(), scratch.size()),
          "game compaction accepts a complete 256-RGB persisted artwork");
    Check(f.sys.gamePersistence.PrepareSettingsSnapshotForLifecycle(),
          "first game snapshot prepares alongside maximum-size artwork");
    const size_t firstMemory = f.sys.settings.Doc().memoryUsage();
    for (unsigned i = 0U; i < 20U; ++i) {
        Check(f.sys.gamePersistence.PrepareSettingsSnapshotForLifecycle(),
              "repeated lifecycle game snapshot never exhausts document with artwork");
    }
    modules::LitchcraftRecord retained;
    Check(modules::ReadLitchcraftRecord(f.sys.settings.Doc(), retained) == modules::LitchcraftRecordStatus::Valid &&
              retained.active && retained.canvas[0].r != 0U && retained.canvas[255].r != 0U,
          "repeated game preparation preserves complete valid artwork and mode");
    Check(f.sys.settings.Doc().memoryUsage() == firstMemory,
          "repeated game preparation compacts instead of accumulating dead JSON pool memory");
    std::printf("[REBOOT_COMPACTION] preparations=21 scratch=%zu first_memory=%zu final_memory=%zu capacity=%zu artwork_valid=%u\n",
                scratch.size(), firstMemory, f.sys.settings.Doc().memoryUsage(),
                f.sys.settings.Doc().capacity(),
                modules::ReadLitchcraftRecord(f.sys.settings.Doc(), retained) == modules::LitchcraftRecordStatus::Valid);
}
void BypassAndRefusal() {
    for (unsigned mode = 0U; mode < 3U; ++mode) {
        Fixture f;
        f.sys.safeModeActive = mode != 0U; f.sys.bootGuard.armed = mode == 2U;
        f.Command(mode == 0U ? "reboot nosave" : "reboot");
        Check(f.accepted && restarts == 1U && f.sys.settingsPersistence.enqueues == 0U && f.Owner().IsDirty(),
              "explicit nosave and safe-mode reboot preserve bypass without a persistence write");
        Check(f.sys.bootGuard.retries == (mode == 2U ? 1U : 0U), "only armed retry clears recovery latch");
    }
    for (unsigned mode = 0U; mode < 6U; ++mode) {
        Fixture f;
        if (mode == 0U) f.sys.ble.ota = true;
        if (mode == 1U) f.sys.sleep.state = firmware::SleepState::Preparing;
        if (mode == 2U) f.sys.settingsPersistence.started = false;
        if (mode >= 3U) f.sys.settings.DisablePersistenceForSession();
        if (mode == 4U) f.sys.settingsPersistence.reformat = Worker::StorageReformatState::Pending;
        if (mode == 5U) f.sys.settingsPersistence.reformat = Worker::StorageReformatState::Running;
        f.Command(); f.Poll();
        Check(!f.sys.rebootPersistencePending && restarts == (mode == 3U ? 1U : 0U) &&
                  f.sys.settingsPersistence.enqueues == 0U && f.Owner().IsDirty(),
              "reboot waits for OTA sleep or active recovery and allows idle RAM-session discard");
    }
}
void CoordinatorDrainsCapturedChordBeforeWallClockUpdate() {
    Fixture f;
    fxe::ManualMillis::SetNowMs(4000U);
    f.sys.gamePersistence.ObserveLitchcraftButton(
        {modules::UserButton::P1X, fxe::Button::Event::Press,
         modules::UiOwner::P1},
        500U);
    Check(f.sys.gamePersistence.Litchcraft().IsActive(),
          "first delayed captured edge remains pending until its queue batch drains");
    f.sys.gamePersistence.ObserveLitchcraftButton(
        {modules::UserButton::P1O, fxe::Button::Event::Press,
         modules::UiOwner::P1},
        530U);
    Check(f.sys.gamePersistence.Litchcraft().IsCanvasVisible() &&
              f.sys.gamePersistence.Litchcraft().IsExitHoldActive(),
          "coordinator preserves an exact-30ms captured exit hold despite delayed dispatch");
    f.sys.gamePersistence.ObserveLitchcraftButton(
        {modules::UserButton::P1X, fxe::Button::Event::Release,
         modules::UiOwner::P1},
        2000U);
    Check(!f.sys.gamePersistence.Litchcraft().IsCanvasVisible(),
          "completed captured exit hides the canvas only on release");
    f.sys.gamePersistence.UpdateLitchcraft();
    f.sys.settingsPersistence.Complete();
    f.sys.gamePersistence.UpdateLitchcraft();
    Check(f.sys.stateMachine.IsPlacementModeActive(),
          "post-batch update consumes exit and enters shared chess recovery");
}
}

int main() {
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    reboot_fixture::PartialRemoteLineCannotAuthorizeReplacementOwner();
    reboot_fixture::ResetFallbackCannotResurrectOldGame();
    reboot_fixture::OwnerConsoleResetJourney();
    reboot_fixture::OwnerConsoleResetFailureTruth();
    reboot_fixture::FactoryBootAdmissionPreservesPendingStorage();
    reboot_fixture::BootloaderWaitsForExactDurableSave();
    reboot_fixture::NativeBootloaderRejectsBeforeChangingRuntime();
    reboot_fixture::NativeBootloaderDestinationCannotQuiesceOrReset();
    reboot_fixture::NativeApplicationRebootStillDrainsSavedState();
    reboot_fixture::BootloaderFailureNeverCutsPowerOrResets();
    reboot_fixture::BootloaderAdmissionRejectsUnsafeContexts();
    reboot_fixture::PendingAndLatestSnapshot();
    reboot_fixture::SessionOnlyReboot();
    reboot_fixture::RecoveryAndSessionOnlySleep();
    reboot_fixture::RecoveryWaitsForVerifiedBootHealth();
    reboot_fixture::EarlyBootSleepWaitsForHealthJourney();
    reboot_fixture::FailureAndRetry();
    reboot_fixture::StoreEditMustFinishBeforeReboot();
    reboot_fixture::FailedStoreEditAbortsReboot();
    reboot_fixture::ExistingFlushCannotAuthorizeReboot();
    reboot_fixture::ArtworkSurvivesRepeatedGamePreparation();
    reboot_fixture::BypassAndRefusal();
    reboot_fixture::CoordinatorDrainsCapturedChordBeforeWallClockUpdate();
    for (const char* suffix : {"", ".tmp", ".bak"}) {
        std::remove((std::string(reboot_fixture::PATH) + suffix).c_str());
    }
    std::printf("[LITCHCRAFT_REBOOT] failures=%d\n", reboot_fixture::failures);
    return reboot_fixture::failures == 0 ? 0 : 1;
}
