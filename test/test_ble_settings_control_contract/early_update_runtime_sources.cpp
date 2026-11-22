#include "ble_service_test_config.hpp"

#include <array>
#include <cstdio>
#include <string_view>

#include "firmware/ota_boot_health.hpp"
#include "firmware/ota_platform_adapter.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "modules/ble_service.hpp"
#include "modules/settings_store.hpp"
#include "runtime/boot_guard.hpp"

// Reuse the existing actual-source boot-health fixture technique. Only the
// hardware aggregate and SDK worker calls are substituted, not orchestration.
#include "firmware/firmware_app_internal.hpp"

namespace early_update_runtime_test {
std::array<char, 32U> events{};
size_t eventCount = 0U;
hardware::RmtFlashArbiter* boundArbiter = nullptr;
bool stateReadable = true;
unsigned initializationFailures = 0U;
bool workerAvailable = true;
modules::OtaPostBootRunningState runningState =
    modules::OtaPostBootRunningState::PendingVerify;

void Note(const char event) { events[eventCount++] = event; }
std::string_view Events() { return {events.data(), eventCount}; }
void Reset() {
    eventCount = 0U;
    boundArbiter = nullptr;
}

class Adapter final : public modules::OtaPlatformAdapter {
  public:
    bool Begin(uint32_t) override { ++imageOperations; return false; }
    bool Write(const uint8_t*, size_t) override { ++imageOperations; return false; }
    bool End() override { ++imageOperations; return false; }
    void Abort() override { ++imageOperations; }
    unsigned imageOperations = 0U;
} adapter;

struct Ble {
    modules::BleService& service;
    uint64_t nonce = 0U;
    bool supportBound = false;
    void SetBootGuard(runtime::BootGuard* guard) {
        Note('g'); service.SetBootGuard(guard);
    }
    void SetHardwareRevision(modules::ble_device_capabilities::HardwareRevision revision) {
        Note('h'); service.SetHardwareRevision(revision);
    }
    void SetConnectionSessionBootNonce(uint64_t value) {
        Note('n'); nonce = value; service.SetConnectionSessionBootNonce(value);
    }
    void SetOtaPlatformAdapter(modules::OtaPlatformAdapter* value) {
        Note('a'); service.SetOtaPlatformAdapter(value);
    }
    void BeginRecovery() { Note(supportBound ? 'b' : '!'); service.BeginRecovery(); }
    void DrainEventQueue() { Note('d'); service.DrainEventQueue(); }
    void Update(const modules::SettingsStore& settings) {
        Note('u'); service.Update(settings);
    }
};
struct Profile { hardware::BoardRevision revision = hardware::BoardRevision::V3_3S3; };
struct Hardware { Profile resolved; };
} // namespace early_update_runtime_test

namespace firmware_internal {
struct EarlyUpdaterRuntimeFixture {
    modules::SettingsStore& settings;
    early_update_runtime_test::Ble ble;
    hardware::RmtFlashArbiter& rmtFlashArbiter;
    runtime::BootGuard bootGuard;
    early_update_runtime_test::Hardware runtimeHardware;
    uint32_t rngSeed = 0x12345678U;
    bool earlyUpdaterInitialized = false;
    bool otaPlatformAdapterBound = false;
    bool recoveryUpdaterStarted = false;
    bool otaBootAwaitingHealth = false;
    uint32_t startupOtaWorkerMs = 0U;
    uint32_t startupOtaCheckMs = 0U;
};
void ConfigureSupportDiagnostics(EarlyUpdaterRuntimeFixture& sys) {
    early_update_runtime_test::Note('c');
    sys.ble.supportBound = true;
}
} // namespace firmware_internal

namespace firmware {
modules::OtaPlatformAdapter* EarlyUpdaterFixtureInitialize(
    hardware::RmtFlashArbiter& arbiter) {
    early_update_runtime_test::Note('w');
    early_update_runtime_test::boundArbiter = &arbiter;
    fxe::ManualMillis::SetNowMs(fxe::Millis() + 11U);
    early_update_runtime_test::workerAvailable =
        early_update_runtime_test::initializationFailures == 0U;
    if (!early_update_runtime_test::workerAvailable) {
        --early_update_runtime_test::initializationFailures;
        return nullptr;
    }
    return &early_update_runtime_test::adapter;
}
bool EarlyUpdaterFixtureReadState(modules::OtaPostBootRunningState& state) {
    early_update_runtime_test::Note('s');
    fxe::ManualMillis::SetNowMs(fxe::Millis() + 3U);
    state = early_update_runtime_test::runningState;
    return early_update_runtime_test::stateReadable &&
           early_update_runtime_test::workerAvailable;
}
OtaBootHealthCommitResult EarlyUpdaterFixtureCheck(hardware::RmtFlashArbiter&) {
    early_update_runtime_test::Note('p');
    fxe::ManualMillis::SetNowMs(fxe::Millis() + 2U);
    return early_update_runtime_test::workerAvailable
               ? OtaBootHealthCommitResult::AwaitingHealth
               : OtaBootHealthCommitResult::SnapshotUnavailable;
}
} // namespace firmware

#define ARDUINO 1
#define LITCH_ESP32S3 1
#define FirmwareSystem EarlyUpdaterRuntimeFixture
#define InitializeEsp32OtaPlatformAdapter EarlyUpdaterFixtureInitialize
#define ReadEsp32OtaRunningState EarlyUpdaterFixtureReadState
#define CheckPendingOtaAtBoot EarlyUpdaterFixtureCheck
#include "../../src/firmware_update_runtime.cpp"
#undef CheckPendingOtaAtBoot
#undef ReadEsp32OtaRunningState
#undef InitializeEsp32OtaPlatformAdapter
#undef FirmwareSystem
#undef LITCH_ESP32S3
#undef ARDUINO

int TestEarlyUpdaterRuntime(modules::SettingsStore& settings,
                            modules::BleService& service,
                            hardware::RmtFlashArbiter& arbiter) {
    using namespace early_update_runtime_test;
    using namespace firmware_internal;
    const uint32_t previousTime = fxe::Millis();
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    adapter.imageOperations = 0U;
    initializationFailures = 0U;
    int failures = 0;
    const auto check = [&](const bool ok, const char* why) {
        if (!ok) { ++failures; std::printf("[EARLY_UPDATER_FAIL] %s\n", why); }
    };
    for (const auto state : {modules::OtaPostBootRunningState::Valid,
                             modules::OtaPostBootRunningState::PendingVerify,
                             modules::OtaPostBootRunningState::Unknown}) {
        Reset();
        runningState = state;
        stateReadable = state != modules::OtaPostBootRunningState::Unknown;
        EarlyUpdaterRuntimeFixture sys{settings, {service}, arbiter};
        InitializeEarlyUpdater(sys);
        check(Events() == "ghncwasp", "identity, shared flash worker and read-only probes precede BLE admission");
        check(boundArbiter == &arbiter, "SDK worker binds the same flash/RMT arbiter");
        check(sys.otaBootAwaitingHealth == (state != modules::OtaPostBootRunningState::Valid),
              "pending and unavailable state require later health evidence");
        check(sys.startupOtaWorkerMs == 11U && sys.startupOtaCheckMs == 5U,
              "early work remains separately observable from normal services");
        const auto nonce = sys.ble.nonce;
        check(nonce != 0U, "recovery and normal startup have a boot identity");
        InitializeEarlyUpdater(sys);
        PumpRecoveryUpdater(sys);
        check(Events() == "ghncwasp" && sys.ble.nonce == nonce,
              "repeated init neither reconfigures BLE nor renews boot identity");
    }

    for (const bool recoveryEntry : {false, true}) {
        Reset();
        initializationFailures = 1U;
        stateReadable = true;
        runningState = modules::OtaPostBootRunningState::PendingVerify;
        EarlyUpdaterRuntimeFixture retry{settings, {service}, arbiter};
        InitializeEarlyUpdater(retry);
        const auto nonce = retry.ble.nonce;
        check(retry.earlyUpdaterInitialized && !retry.otaPlatformAdapterBound &&
                  retry.otaBootAwaitingHealth,
              "failed worker startup retains identity and unresolved health");
        if (recoveryEntry) BeginRecoveryUpdater(retry);
        else InitializeEarlyUpdater(retry);
        check(retry.otaPlatformAdapterBound && retry.ble.nonce == nonce &&
                  Events() == (recoveryEntry ? "ghncwaspwaspb" : "ghncwaspwasp"),
              "explicit normal/recovery entry retries worker without rebinding identity");
        InitializeEarlyUpdater(retry);
        check(Events() == (recoveryEntry ? "ghncwaspwaspb" : "ghncwaspwasp") &&
                  adapter.imageOperations == 0U && !service.IsOtaRuntimeAccepted(),
              "successful retry binds once without image mutation or acceptance");
    }

    Reset();
    stateReadable = true;
    runningState = modules::OtaPostBootRunningState::PendingVerify;
    EarlyUpdaterRuntimeFixture sys{settings, {service}, arbiter};
    BeginRecoveryUpdater(sys);
    BeginRecoveryUpdater(sys);
    check(Events() == "ghncwaspb", "fatal startup admits existing recovery transport once");
    PumpRecoveryUpdater(sys);
    check(Events() == "ghncwaspbdu", "recovery drives existing ingress and Update owners");
    check(!service.IsOtaRuntimeAccepted() && adapter.imageOperations == 0U,
          "initialization and recovery never accept or mutate an image");
    // The fixture has no settings-load, engine, game or sensor member/API. The
    // actual production helper therefore cannot depend on those normal owners.
    service.Stop();
    fxe::ManualMillis::SetNowMs(previousTime);
    std::printf("[EARLY_UPDATER_RUNTIME] failures=%d\n", failures);
    return failures;
}
