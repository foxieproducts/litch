#include "ble_service_test_config.hpp"

#include <cstdio>
#include <NimBLEDevice.h>

#include "firmware/ota_boot_health.hpp"
#include "firmware/sleep_controller.hpp"
#include "firmware/startup_led_sequence.hpp"
#include "bounded_hint_engine.hpp"
#include "virtual_litch_session.hpp"
#include "../../src/virtual_litch_session_internal.hpp"
#include "firmware/ota_platform_adapter.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "modules/ble_service.hpp"
#include "modules/settings_store.hpp"
#include "modules/litchcraft_frame_stream.hpp"

// Exclude only the hardware aggregate from this actual-source native build.
// The runtime policy below is compiled unchanged with real BleService state.
#include "firmware/firmware_app_internal.hpp"
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_tasks_internal.hpp"

namespace boot_health_runtime_test {
unsigned commits = 0U, failures = 0U, restarts = 0U, preparations = 0U;
firmware::OtaBootHealthCommitResult commitResult =
    firmware::OtaBootHealthCommitResult::Confirmed;
firmware::OtaBootHealthCommitResult failureResult =
    firmware::OtaBootHealthCommitResult::NoPendingImage;
modules::OtaPostBootRunningState runningState =
    modules::OtaPostBootRunningState::PendingVerify;
bool stateReadable = true;
struct RestartRequested {};
struct Platform {
    void restart() { ++restarts; throw RestartRequested{}; }
} platform;
struct Ble {
    modules::BleService& service;
    unsigned accepted = 0U;
    bool IsAdvertising() const { return service.IsAdvertising(); }
    bool IsConnected() const { return service.IsConnected(); }
    void ReserveBootFirmwareDigest() { service.ReserveBootFirmwareDigest(); }
    void ReleaseBootFirmwareDigest() { service.ReleaseBootFirmwareDigest(); }
    void PublishVerifiedRunningFirmwareDigest(const uint8_t* digest) {
        service.PublishVerifiedRunningFirmwareDigest(digest);
    }
    void PublishOtaRuntimeAccepted() {
        ++accepted;
        service.PublishOtaRuntimeAccepted();
    }
};
struct Guard {
    unsigned healthy = 0U;
    void MarkBootHealthy() { ++healthy; }
};
void Reset() {
    commits = failures = restarts = preparations = 0U;
    commitResult = firmware::OtaBootHealthCommitResult::Confirmed;
    failureResult = firmware::OtaBootHealthCommitResult::NoPendingImage;
    runningState = modules::OtaPostBootRunningState::PendingVerify;
    stateReadable = true;
}
} // namespace boot_health_runtime_test

namespace firmware_internal {
struct BootHealthRuntimeFixture {
    modules::SettingsStore& settings;
    boot_health_runtime_test::Ble ble;
    hardware::RmtFlashArbiter& rmtFlashArbiter;
    boot_health_runtime_test::Guard bootGuard;
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
} // namespace firmware_internal

namespace firmware {
inline auto BootHealthFixturePrepare = [](RuntimeBootHealthState& state) {
    ++boot_health_runtime_test::preparations;
    state.healthTicket = 1U;
    return OtaBootHealthCommitResult::Pending;
};
OtaBootHealthCommitResult BootHealthFixtureCommit(RuntimeBootHealthState& state) {
    ++boot_health_runtime_test::commits;
    if (boot_health_runtime_test::commitResult != OtaBootHealthCommitResult::Pending)
        state.healthTicket = 0U;
    return boot_health_runtime_test::commitResult;
}
OtaBootHealthCommitResult BootHealthFixtureFailure(RuntimeBootHealthState& state) {
    ++boot_health_runtime_test::failures;
    if (boot_health_runtime_test::failureResult != OtaBootHealthCommitResult::Pending)
        state.healthTicket = 0U;
    return boot_health_runtime_test::failureResult;
}
bool BootHealthFixtureReadEvidence(OtaBootEvidence& evidence) {
    evidence.available = boot_health_runtime_test::stateReadable;
    evidence.runningState = boot_health_runtime_test::runningState;
    return true;
}
} // namespace firmware

void BootHealthFixtureDelay(unsigned long) {}
#define ARDUINO 1
#define LITCH_ESP32S3 1
#define FirmwareSystem BootHealthRuntimeFixture
#define PreparePendingOtaBootHealth BootHealthFixturePrepare
#define StartupPresentationComplete(sys) ((sys).ledAnimation.startupCompletedAtMs != UINT32_MAX)
#define CommitPendingOtaAtHealthBoundary BootHealthFixtureCommit
#define ReportPendingOtaBootFailure BootHealthFixtureFailure
#define CopyEsp32OtaBootEvidence BootHealthFixtureReadEvidence
#define delay BootHealthFixtureDelay
#define ESP boot_health_runtime_test::platform
#include "../../src/firmware_boot_health_runtime.cpp"
#undef ESP
#undef delay
#undef CopyEsp32OtaBootEvidence
#undef ReportPendingOtaBootFailure
#undef CommitPendingOtaAtHealthBoundary
#undef StartupPresentationComplete
#undef PreparePendingOtaBootHealth
#undef FirmwareSystem
#undef LITCH_ESP32S3
#undef ARDUINO

int TestRuntimeBootHealthRecovery(modules::SettingsStore& settings,
                                  modules::BleService& service,
                                  hardware::RmtFlashArbiter& arbiter,
                                  NimBLEAdvertising& advertising) {
    using namespace boot_health_runtime_test;
    using firmware_internal::BootHealthRuntimeFixture;
    using firmware_internal::PollRuntimeBootHealth;
    int failed = 0;
    const auto check = [&](bool ok, const char* why) {
        if (!ok) { ++failed; std::printf("[BOOT_HEALTH_FAIL] %s\n", why); }
    };
    const auto poll = [&](BootHealthRuntimeFixture& sys) {
        try { return PollRuntimeBootHealth(sys); }
        catch (const RestartRequested&) { return false; }
    };
    const uint32_t priorTime = fxe::Millis();

    // The production poll must not begin a full image job while essential mode
    // authority or completed nonzero output is missing. Only worker completion
    // and the renderer receipt are controlled; both owners remain unchanged.
    {
        Reset();
        fxe::ManualMillis::SetNowMs(0U);
        advertising.SetActive(true);
        modules::litchcraft_frame_stream::FrameStream mode;
        mode.BeginInitialRecovery();
        BootHealthRuntimeFixture lighting{settings, {service}, arbiter};
        lighting.gamePersistence.session.AttachFrameStream(&mode);
        check(!poll(lighting), "startup begins without a full image job");
        fxe::ManualMillis::SetNowMs(250U);
        lighting.ledAnimation.startupCompletedAtMs = 200U;
        check(!poll(lighting) && preparations == 0U && commits == 0U &&
                  lighting.runtimeBootHealth.startupCompletedAtMs == UINT32_MAX,
              "an early Off/empty completion cannot bypass pending mode or start the health budget");
        lighting.bootFirstNonzeroRenderLogged = true;
        fxe::ManualMillis::SetNowMs(500U);
        check(!poll(lighting) && preparations == 0U,
              "nonzero controls alone cannot bypass essential mode recovery");
        mode.CompleteInitialRecovery();
        lighting.ledAnimation.startupCompletedAtMs = UINT32_MAX;
        lighting.bootFirstNonzeroRenderLogged = false;
        fxe::ManualMillis::SetNowMs(750U);
        check(!poll(lighting) && preparations == 0U,
              "resolved mode alone cannot start hashing before completed lighting");
        lighting.bootFirstNonzeroRenderLogged = true;
        fxe::ManualMillis::SetNowMs(1000U);
        check(!poll(lighting) && preparations == 1U && commits == 0U && lighting.ble.accepted == 0U,
              "resolved mode plus completed nonzero output starts preparation without premature acceptance");
        std::printf("[BOOT_HASH_LIGHTING] pending_mode=deferred black=deferred nonzero_and_mode=prepare preparations=%u commits=%u\n",
                    preparations, commits);

        Reset();
        fxe::ManualMillis::SetNowMs(2000U);
        BootHealthRuntimeFixture dark{settings, {service}, arbiter};
        check(!poll(dark), "valid dark startup initially defers verification");
        dark.ledAnimation.startupCompletedAtMs = 2200U;
        fxe::ManualMillis::SetNowMs(2250U);
        check(poll(dark) && preparations == 1U && commits == 1U && dark.ble.accepted == 1U,
              "confirmed complete dark/Off presentation permits full verification and existing commit authority");
        std::printf("[BOOT_HASH_LIGHTING] complete_dark=fallback preparations=%u commits=%u accepted=%u\n",
                    preparations, commits, dark.ble.accepted);

        Reset();
        fxe::ManualMillis::SetNowMs(3000U);
        BootHealthRuntimeFixture factory{settings, {service}, arbiter};
        factory.factoryBootOnly = true;
        check(!poll(factory), "factory remains outside ordinary lighting admission");
        fxe::ManualMillis::SetNowMs(3250U);
        check(!poll(factory) && preparations == 1U && commits == 0U,
              "existing factory preparation is retained without ordinary startup acceptance");
    }

    // Named EarlyBootSetupSleep journey: real startup output sequencing, boot
    // health poll, typed control admission, sleep owner and VirtualLitch output.
    // Transfer, worker completion and the sleep SDK are controlled boundaries.
    {
        Reset();
        fxe::ManualMillis::SetNowMs(0U);
        advertising.SetActive(true);
        BootHealthRuntimeFixture early{settings, {service}, arbiter};
        early.otaBootAwaitingHealth = false;
        commitResult = firmware::OtaBootHealthCommitResult::Pending;
        virtual_litch_config config{};
        virtual_litch_default_config(&config);
        config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
        test_support::BoundedHintEngine engine;
        runtime::virtual_litch_internal::Session board(config,
            runtime::virtual_litch_internal::SessionMode::EngineBackedJourney, nullptr, &engine);
        firmware::StartupLedSequence startup;
        startup.Begin(false);
        startup.Update([&](const uint16_t scale) {
            board.CaptureStartupFrame(fxe::Millis(), scale);
            return true;
        }, [] {});
        early.ledAnimation.startupCompletedAtMs = startup.FadeCompletedMs();
        check(startup.IsReady() && !poll(early),
              "actual startup output is ready while boot health remains pending");
        firmware::SleepController sleep;
        modules::ControlMutationCallbacks callbacks;
        callbacks.context = &sleep;
        callbacks.requestSleep = [](void* context) {
            return static_cast<firmware::SleepController*>(context)->RequestSleep(
                firmware::SleepRequestOrigin::Menu, fxe::Millis(), false)
                ? modules::ControlCoordinatorResult::Applied : modules::ControlCoordinatorResult::Busy;
        };
        modules::ControlMutationService control;
        control.Configure(settings, callbacks);
        fxe::ManualMillis::SetNowMs(100U);
        board.StepUntil(100U);
        check(control.MutateAction(modules::ControlAction::Sleep, 55U).status ==
                  modules::ControlMutationStatus::Applied &&
                  board.RequestSleep(VIRTUAL_LITCH_SLEEP_ORIGIN_MENU) == VIRTUAL_LITCH_OK,
              "typed Setup Sleep enters the existing shared owner once just after boot");
        uint32_t fadeEnergy = 0U;
        for (const uint32_t now : {100U, 3100U, 3350U, 3600U}) {
            fxe::ManualMillis::SetNowMs(now);
            const bool healthy = poll(early);
            const bool suppressed = firmware::IsSleepSuppressedByHigherPriority(
                {.otaHealthConfirmationPending = !healthy}, sleep.GetRequestOrigin());
            sleep.Update(settings, now, suppressed ? firmware::SleepSuppression::HigherPriority
                                                  : firmware::SleepSuppression::Allow);
            board.StepUntil(now);
            check(!healthy && unsigned(sleep.GetState()) == board.SleepLifecycle().state,
                  "pending real boot health retains the authoritative cue and fade phase");
            uint32_t energy = 0U;
            const auto& frame = board.PhysicalFrame();
            for (uint16_t pixel = 0U; pixel < frame.led_count; ++pixel) {
                const auto color = frame.pixels[pixel];
                energy += color.r + color.g + color.b + color.w;
            }
            if (now == 3100U) { fadeEnergy = energy; check(energy > 0U, "early fade is physically visible"); }
            if (now == 3350U) check(energy > 0U && energy < fadeEnergy, "early fade has a visible intermediate mapped frame");
            if (now == 3600U) check(energy == 0U, "early preparation remains physically black while startup/save finishes");
            const auto text = board.DisplayText();
            std::printf("[EARLY_BOOT_MENU_JOURNEY] t=%u health=%u sleep=%u fade=%u occupancy=%016llx physical_energy=%u text=%.*s next=wait_for_startup_or_save\n",
                now, healthy, unsigned(sleep.GetState()), sleep.GetFadeScale256(),
                static_cast<unsigned long long>(board.State().physical_occupied_mask), energy,
                static_cast<int>(text.size()), text.data());
        }
        struct Save {
            BootHealthRuntimeFixture& boot;
            bool ready = false;
            unsigned starts = 0U;
        } save{early};
        firmware::SleepPreparationHooks hooks{&save,
            [](void* context) { ++static_cast<Save*>(context)->starts; return true; },
            [](void* context) {
                const auto& value = *static_cast<Save*>(context);
                return value.ready && value.boot.runtimeBootHealth.commitFinished
                    ? firmware::SleepPersistenceStatus::Succeeded : firmware::SleepPersistenceStatus::Pending;
            }, [](void*) { return true; }, [](void*) { return true; }};
        check(sleep.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::Pending,
              "startup wait begins one cooperative preparation transaction");
        fxe::ManualMillis::SetNowMs(4999U);
        check(!poll(early) && sleep.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::Pending,
              "pending full verification prevents early teardown despite completed startup output");
        fxe::ManualMillis::SetNowMs(5000U);
        commitResult = firmware::OtaBootHealthCommitResult::Confirmed;
        check(poll(early) && sleep.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::Pending,
              "health completion alone cannot bypass the pending save owner");
        save.ready = true;
        check(sleep.RunSleepPreparation(hooks) == firmware::SleepPreparationResult::Authorized &&
                  sleep.ConsumeSleepAuthorization() && !sleep.ConsumeSleepAuthorization() && save.starts == 1U &&
                  board.ResolveSleepPreflights(true, true) == VIRTUAL_LITCH_OK,
              "actual readiness and save completion release the original request exactly once");
        std::printf("[EARLY_BOOT_MENU_JOURNEY] t=5000 health=1 save=ready starts=%u sleep=%u next=wake_input\n",
            save.starts, unsigned(sleep.GetState()));
    }

    // Initial unreadable state is suspicion, not a failed running image. Even a
    // later cheap Valid observation cannot substitute for durable confirmation.
    Reset();
    stateReadable = false;
    fxe::ManualMillis::SetNowMs(100000U);
    BootHealthRuntimeFixture uncertain{settings, {service}, arbiter};
    check(!poll(uncertain), "uncertain boot starts unaccepted");
    commitResult = firmware::OtaBootHealthCommitResult::EvidenceChanged;
    fxe::ManualMillis::SetNowMs(105000U);
    uncertain.ledAnimation.startupCompletedAtMs = 105000U;
    check(!poll(uncertain) && failures == 0U && restarts == 0U &&
              commits == 1U && uncertain.ble.accepted == 0U,
          "unreadable authority retries without declaring a failed boot");
    fxe::ManualMillis::SetNowMs(105249U);
    check(!poll(uncertain) && commits == 1U,
          "uncertain authority does not busy-loop on the boot worker");
    stateReadable = true;
    runningState = modules::OtaPostBootRunningState::Valid;
    fxe::ManualMillis::SetNowMs(105250U);
    check(!poll(uncertain) && !uncertain.otaBootAwaitingHealth &&
              failures == 0U && restarts == 0U &&
              commits == 2U && uncertain.ble.accepted == 0U,
          "fresh Valid clears stale suspicion but cannot publish acceptance");
    commitResult = firmware::OtaBootHealthCommitResult::Confirmed;
    fxe::ManualMillis::SetNowMs(105500U);
    check(poll(uncertain) && commits == 3U &&
              uncertain.ble.accepted == 1U && restarts == 0U,
          "only verified confirmation publishes acceptance after recovery");
    std::printf("[BOOT_HEALTH] authority_recovery failed_reports=%u resets=%u commits=%u accepted=%u\n",
                failures, restarts, commits, uncertain.ble.accepted);

    Reset();
    fxe::ManualMillis::SetNowMs(0U);
    BootHealthRuntimeFixture sys{settings, {service}, arbiter};
    check(!poll(sys), "new boot cannot publish acceptance before health");
    BootHealthRuntimeFixture validBoot{settings, {service}, arbiter};
    validBoot.otaBootAwaitingHealth = false;
    check(!poll(validBoot), "VALID history still awaits current runtime health");
    advertising.SetActive(false);
    fxe::ManualMillis::SetNowMs(4999U);
    service.OnConnected(7U, "health-boundary", 247U, 1U);
    check(!service.IsAdvertising() && !service.IsConnected(),
          "real SDK callback precedes main-loop connection activation");
    fxe::ManualMillis::SetNowMs(5000U);
    validBoot.ledAnimation.startupCompletedAtMs = 5000U;
    sys.ledAnimation.startupCompletedAtMs = 5000U;
    check(!poll(validBoot) && commits == 0U && validBoot.ble.accepted == 0U,
          "already VALID image cannot publish accepted while configured BLE is unavailable");
    check(!poll(sys) && failures == 0U && restarts == 0U && commits == 0U,
          "healthy connection transition must defer, not reject or reset");
    service.DrainEventQueue();
    fxe::ManualMillis::SetNowMs(5250U);
    check(poll(sys) && failures == 0U && restarts == 0U &&
              commits == 1U && sys.ble.accepted == 1U,
          "drained connection permits one verified acceptance without reset");
    std::printf("[BOOT_HEALTH] callback_recovery failed_reports=%u resets=%u commits=%u accepted=%u\n",
                failures, restarts, commits, sys.ble.accepted);
    fxe::ManualMillis::SetNowMs(90000U);
    check(poll(sys) && sys.bootGuard.healthy == 1U,
          "healthy normal runtime clears crash evidence at its boundary");
    fxe::ManualMillis::SetNowMs(90001U);
    check(poll(sys) && sys.bootGuard.healthy == 1U && sys.ble.accepted == 1U,
          "normal runtime clears crash evidence and publishes acceptance once");
    // A real initialization failure still reaches the existing verifier. If
    // that verifier cannot act, recovery keeps running with bounded retries.
    Reset();
    fxe::ManualMillis::SetNowMs(200000U);
    BootHealthRuntimeFixture failedBoot{settings, {service}, arbiter};
    failedBoot.safeModeActive = true;
    check(!poll(failedBoot), "safe-mode boot never reports normal acceptance");
    failureResult = firmware::OtaBootHealthCommitResult::SnapshotUnavailable;
    fxe::ManualMillis::SetNowMs(200250U);
    check(!poll(failedBoot) && failures == 1U && commits == 0U &&
              failedBoot.ble.accepted == 0U && restarts == 0U,
          "failed-boot evidence read failure must not cause a bare reset");
    fxe::ManualMillis::SetNowMs(200251U);
    check(!poll(failedBoot) && failures == 1U,
          "safe-mode rollback attempts respect the retry interval");
    failureResult = firmware::OtaBootHealthCommitResult::NoPendingImage;
    fxe::ManualMillis::SetNowMs(200500U);
    check(!poll(failedBoot) && failures == 2U && commits == 0U &&
              failedBoot.ble.accepted == 0U && restarts == 0U,
          "NoAction with a pending running image proves neither success nor rollback");
    failureResult = firmware::OtaBootHealthCommitResult::RollbackRequested;
    fxe::ManualMillis::SetNowMs(200750U);
    check(!poll(failedBoot) && failures == 3U && restarts == 0U,
          "failed boot retains the verifier-owned rollback request without claiming completed rollback");
    fxe::ManualMillis::SetNowMs(290000U);
    check(!poll(failedBoot) && failures == 3U && commits == 0U &&
              failedBoot.ble.accepted == 0U && failedBoot.bootGuard.healthy == 0U,
          "safe mode never confirms firmware or clears retained crash evidence");
    std::printf("[BOOT_HEALTH] failed_boot failed_reports=%u resets=%u commits=%u accepted=%u crash_clears=%u\n",
                failures, restarts, commits, failedBoot.ble.accepted,
                failedBoot.bootGuard.healthy);
    fxe::ManualMillis::SetNowMs(priorTime);
    return failed;
}
