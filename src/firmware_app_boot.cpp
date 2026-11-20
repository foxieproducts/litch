#include "hardware/board_led_output_mapping.hpp"
#ifdef ARDUINO

#include <Arduino.h>
#if defined(ESP32)
#include <esp_system.h>
#endif

#include "firmware/firmware_app_internal.hpp"
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_recovery_internal.hpp"
#include "firmware/native_usb_reset_recovery.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/board_revision_runtime.hpp"
#include "hardware/hardware_test_boot_entry.hpp"
#include "logger.hpp"
#include "modules/brightness_scale_policy.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/orientation.hpp"
#include "modules/startup_choreography.hpp"
#include "modules/volatile_persistence_session.hpp"
#include "modules/white_squares_setting.hpp"
#include "runtime/progress_watchdog.hpp"

namespace {
#if defined(ESP32)
RTC_NOINIT_ATTR firmware::FactoryBootRequest g_factoryBootRequest;
#else
firmware::FactoryBootRequest g_factoryBootRequest;
#endif
#if defined(LITCH_HW_VARIANT_S3)
class ArduinoRevisionStrapGpio final {
  public:
    void ConfigureInputPullup(const uint8_t pin) {
        pinMode(pin, INPUT_PULLUP);
    }

    bool IsLow(const uint8_t pin) const {
        return digitalRead(pin) == LOW;
    }
};
#endif

uint32_t ComputeBootRngSeed() {
#if defined(ESP32)
    return static_cast<uint32_t>(esp_random());
#else
    return fxe::Millis() ^ 0x9e3779b9U;
#endif
}

} // namespace

namespace firmware_internal {

void ArmFactoryBootIntent(const firmware::FactoryBootIntent intent) {
    firmware::ArmFactoryBootRequest(g_factoryBootRequest, intent);
}

void ApplyRequestedFactoryBoot(FirmwareSystem& sys) {
    // The existing OTA preflight must finish before consuming the one-shot
    // request: its worker cannot report running state before initialization.
    const auto requestedFactoryBoot = firmware::ConsumeFactoryBootRequest(
        g_factoryBootRequest,
#if defined(ESP32)
        esp_reset_reason() == ESP_RST_SW,
#else
        false,
#endif
        sys.safeModeActive || sys.otaBootAwaitingHealth,
        hardware::SupportsProductionHardwareTest(sys.runtimeHardware.detectedRevision));
    modules::HardwareTestRecord savedFactoryRecord;
    sys.factoryVerifierBoot = !sys.startupHardwareTestRequested &&
        requestedFactoryBoot == firmware::FactoryBootIntent::Verifier &&
        sys.factoryProvisioning.AllowsCompletedHardwareTestResult() &&
        modules::DecodeHardwareTestRecord(sys.settings.ReadValue(
            modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""), savedFactoryRecord) &&
        modules::IsFactoryVerifierEligible(savedFactoryRecord) &&
        savedFactoryRecord.revision == sys.runtimeHardware.detectedRevision;
    sys.startupHardwareTestRequested = sys.startupHardwareTestRequested ||
        (requestedFactoryBoot == firmware::FactoryBootIntent::Test &&
         sys.factoryProvisioning.State() != modules::FactoryProvisioningState::Passed);
    sys.factoryBootOnly = firmware::FactoryBootProfileFor(
        sys.startupHardwareTestRequested, sys.factoryVerifierBoot) !=
            firmware::FactoryBootProfile::Normal;
}

void SetupApp() {
    const auto setupElapsed = fxe::ElapsedTime::StartNow();
    BeginNativeUsbResetRecovery();
    Serial.begin(115200);
    InitializeBoardIdentityEntropy();
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT)
    // HybridConsoleTransport checks capacity before each write. Do not let
    // CDC mutex contention add a second wait inside the cooperative loop.
    Serial.setTxTimeoutMs(0U);
#endif

    // Install the non-blocking logger handoff before GetSystem() and settings
    // recovery can emit any diagnostics.
    ConfigureConsoleLogger();
    auto& sys = GetSystem();
    sys.startupEntryMs = setupElapsed.StartMs();

#if defined(LITCH_HW_VARIANT_S3)
    ArduinoRevisionStrapGpio revisionGpio;
    sys.runtimeHardware =
        hardware::SampleS3RuntimeHardwareProfile(revisionGpio);
#else
    sys.runtimeHardware = hardware::RuntimeProfileForC3();
#endif
    const hardware::BoardHardwareProfile& hardwareProfile =
        sys.runtimeHardware.resolved;
    sys.powerRail.Configure(hardwareProfile);
    sys.sleep.ConfigureHardwareProfile(hardwareProfile);
    sys.gpioUserInput.ConfigureHardwareProfile(hardwareProfile);
    sys.inputScan.ConfigureHardwareProfile(hardwareProfile);
    // Factory evidence requires a known identity; ordinary peripherals keep
    // the resolved hardware profile when an unsupported strap falls back.
    sys.ble.SetFactoryVerifierSupported(
        hardware::SupportsProductionHardwareTest(
            sys.runtimeHardware.detectedRevision));

    sys.bootGuard.OnBootStart();
    // Capture per-boot identity and initialize the read-only updater before a
    // crash-loop or unavailable storage can bypass normal service startup.
    sys.rngSeed = ComputeBootRngSeed();
    InitializeEarlyUpdater(sys);
    sys.safeModeActive = sys.bootGuard.IsSafeModeActive();
    if (sys.safeModeActive) {
        ArmFactoryBootIntent(firmware::FactoryBootIntent::None);
        EnterSafeModeRecovery(sys, DetectBootRecoveryCause(sys));
    }

    runtime::NoteProgress(0U, runtime::ProgressStage::SettingsLoad);
    auto phaseElapsed = fxe::ElapsedTime::StartNow();
    EnsureSettingsLoaded();
    sys.startupSettingsMs = phaseElapsed.ElapsedMs();
    runtime::FeedProgressWatchdog();

    logger::Info("Litch firmware boot");
    logger::Trace(
        "hardware: detected_revision=%u resolved_revision=%u straps_sampled=%u",
        static_cast<unsigned>(sys.runtimeHardware.detectedRevision),
        static_cast<unsigned>(hardwareProfile.revision),
        static_cast<unsigned>(sys.runtimeHardware.revisionSampled ? 1U : 0U));
    {
        const auto snap = sys.bootGuard.GetSnapshot();
        logger::Info("boot: reset_reason=%lu boots=%lu early_crashes=%lu safemode=%u",
                     static_cast<unsigned long>(snap.lastResetReason),
                     static_cast<unsigned long>(snap.bootCount),
                     static_cast<unsigned long>(snap.earlyCrashCount),
                     static_cast<unsigned>(snap.safeModeActive ? 1U : 0U));
    }
    if (sys.safeModeActive) {
        logger::Error("boot: safe mode active (crash loop detected)");
    }

    if (sys.safeModeActive) {
        EnsureConsoleInitialized();
        return;
    }

    sys.bootStorageError = static_cast<uint8_t>(sys.settings.GetLastError());
    if (sys.settingsLoadSucceeded && sys.settings.IsLoadRepairPending()) {
        logger::Warn("boot: usable settings retained; storage recovery will retry through settings worker");
    }
    if (sys.settingsLoadAttempted &&
        !(sys.settingsLoadSucceeded && sys.settings.IsLoadRepairPending()) &&
        (!sys.settingsLoadSucceeded || sys.settings.IsDisabled() ||
         sys.settings.GetLastError() != fxe::SettingsStoreError::None)) {
        logger::Error(
            "boot: settings persistence unavailable; continuing with volatile defaults err=%s",
            fxe::SettingsStoreErrorToString(sys.settings.GetLastError()));
        // Preserve the failed durable state on flash. The cleared document is
        // an in-memory default store for this boot, and disabled saves cannot
        // overwrite evidence that support/rework may need.
        sys.persistenceDegradation = modules::QuarantineUnavailableSettings(
            sys.settings, sys.settingsLoadSucceeded);
    }

    sys.factoryProvisioning.Begin(
        sys.runtimeHardware.detectedRevision, sys.settings.Doc(), sys.rmtFlashArbiter);

    sys.sleep.Begin();
    MaybeRunFactoryResetBootFlow(sys);
    if (sys.safeModeActive) {
        EnsureConsoleInitialized();
        return;
    }
    // Settings/latch and reset gestures govern admission. No source sampling,
    // radio, storage worker, gameplay restore or sensor work runs in the fade.
    InitializeLedPowerBudgetAfterSettingsLoad(sys);
    EnsurePowerRailInitialized(sys);
    auto& leds = sys.Leds();
    leds.SetRmtFlashArbiter(&sys.rmtFlashArbiter);
    if (!sys.ledOutputInitialized) {
        leds.Begin();
        sys.ledOutputInitialized = true;
    }
    const uint32_t rgbwThreshold = sys.settings.ReadValue(
        "RGBW_COOL_TO_WARM_THRESHOLD", uint32_t{leds.GetRgbwCoolToWarmThreshold()});
    leds.SetRgbwCoolToWarmThreshold(
        static_cast<uint8_t>(rgbwThreshold > 255U ? 255U : rgbwThreshold));

    const bool squareRotate = modules::ComputeControlBankSwap(
        GetFlipSidesSetting(sys.settings), GetFirstPlayerIsP2Setting(sys.settings),
        false);
    sys.gpioUserInput.SetRotate180(squareRotate);
    sys.gpioUserInput.Begin();
    sys.sleep.UpdateIgnoreGate();
    const bool factoryUpHeld = !sys.sleep.IsDeepSleepWake() &&
        hardware::ShouldEnterHardwareTestFromBoot(
            sys.runtimeHardware.detectedRevision, hardwareProfile.hasP2Controls,
            digitalRead(hardwareProfile.p1.up) == LOW,
            hardwareProfile.hasP2Controls && hardwareProfile.p2.up != hardware::NO_GPIO && digitalRead(hardwareProfile.p2.up) == LOW);
    const auto factoryState = sys.factoryProvisioning.State();
    if (factoryUpHeld && factoryState == modules::FactoryProvisioningState::Override) {
        const bool requiredSaved = sys.factoryProvisioning.RequireFactoryTest();
        // Required NVS dominates a surviving old record on every future boot.
        // User settings/identity and the independent Down reset path are kept.
        if (requiredSaved) {
            sys.settings.Doc().remove(modules::HARDWARE_TEST_RECORD_SETTING_KEY);
            sys.settings.MarkMustSaveDirty();
        }
        logger::Info("factory: override Up re-entry required_saved=%u", static_cast<unsigned>(requiredSaved));
    }
    sys.startupHardwareTestRequested = sys.factoryProvisioning.ShouldAutoEnterHardwareTest() ||
        (factoryUpHeld && sys.factoryProvisioning.AllowsPhysicalUpEntry());
    sys.factoryBootOnly = firmware::FactoryBootProfileFor(
        sys.startupHardwareTestRequested, sys.factoryVerifierBoot) !=
            firmware::FactoryBootProfile::Normal;
    sys.startupLeds.Begin(
        firmware::StartupLedSequence::UsesPreServiceGrid(hardwareProfile.revision) &&
        modules::white_squares_setting::ReadWhiteSquaresOn(sys.settings) &&
        modules::white_squares_setting::ReadBrightness0To8(sys.settings) != 0U);
    AdvanceStartup();
}

void AdvanceStartup() {
    auto& sys = GetSystem();
    sys.startupLeds.Update(
        [&](const uint16_t fadeScale256) {
            runtime::NoteProgress(0U, runtime::ProgressStage::Render);
            const uint16_t scale256 =
                modules::brightness_scale_policy::ComputeEffectiveScale256(
                    modules::white_squares_setting::ReadBrightness0To8(sys.settings),
                    false, 0.0f, 256U,
                    sys.settings.ReadValue("AUTO_BRIGHTNESS", true));
            const auto renderElapsed = fxe::ElapsedTime::StartNow();
            modules::BoardLedFrame startupFrame{};
            if (firmware::StartupLedSequence::UsesPreServiceGrid(sys.runtimeHardware.resolved.revision))
                modules::led_frame_composer::ComposeStartupGrid(sys.settings, startupFrame);
            auto physicalFrame = hardware::MapSemanticLedFrame(
                {sys.runtimeHardware.resolved.revision, false, false},
                startupFrame,
                std::array<modules::Rgb, modules::EDGE_LED_COUNT>{});
            for (uint8_t square = 0U; square < 64U; ++square) {
                const uint16_t squareScale =
                    modules::startup_choreography::GridScale256(
                        fadeScale256, square);
                for (const auto slot : {hardware::SquareLedSlot::Square0,
                                        hardware::SquareLedSlot::Square2}) {
                    const auto targets = hardware::MapSemanticSquareLedTargets(
                        sys.runtimeHardware.resolved.revision, square,
                        slot, false);
                    for (uint8_t target = 0U; target < targets.count; ++target) {
                        const uint16_t index = targets.indices[target];
                        physicalFrame[index] = modules::ScaleRgbSaturatingBy256(
                            physicalFrame[index], squareScale);
                    }
                }
            }
            RenderPhysicalFrameWithConservativePowerBudget(
                sys, physicalFrame, scale256);
            if (!sys.Leds().LastShowCompleted()) {
                if (sys.startupLeds.RejectedFrames() == 0U) {
                    logger::Warn("boot: LED transfer incomplete; services waiting");
                }
                return false;
            }
            RecordBootRenderReturn(sys, renderElapsed, scale256);
            return true;
        },
        [&]() {
            logger::Trace(
                "boot: fade_complete_ms=%lu accepted=%lu rejected=%lu max_gap_ms=%lu fade_start=%lu entry=%lu settings_ms=%lu",
                static_cast<unsigned long>(sys.startupLeds.FadeCompletedMs()),
                static_cast<unsigned long>(sys.startupLeds.AcceptedFrames()),
                static_cast<unsigned long>(sys.startupLeds.RejectedFrames()),
                static_cast<unsigned long>(sys.startupLeds.MaxFrameGapMs()),
                static_cast<unsigned long>(sys.startupLeds.FadeStartedMs()),
                static_cast<unsigned long>(sys.startupEntryMs),
                static_cast<unsigned long>(sys.startupSettingsMs));
            FinishStartup();
        });
    if (sys.startupLeds.NeedsOutputRecovery()) {
        // A failed output path must retain diagnostics without admitting game,
        // settings or rail mutations on partially initialized owners.
        MaybeEnableConsoleOutput();
        if (auto* console = GetSystemContext().console) {
            console->Update();
        }
    }
}

} // namespace firmware_internal

#endif
