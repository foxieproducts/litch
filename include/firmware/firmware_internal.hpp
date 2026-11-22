#pragma once

#include "firmware/factory_boot_intent.hpp"
#include "firmware/factory_completion_reboot.hpp"
#include "modules/physical_palette_shortcut.hpp"
#include "modules/board_style_preview.hpp"
#include "modules/control_led_feedback.hpp"

#include "firmware/board_storage_recovery.hpp"
#include "firmware/ota_boot_health.hpp"
#include "firmware/periodic_task_cadence.hpp"
#include "modules/board_led_frame.hpp"
#include "modules/litchcraft_physical_presentation.hpp"

#ifdef ARDUINO

#include <array>
#include <atomic>
#include <cstdint>
#include <new>

#include "console.hpp"
#include "firmware/amptest_session.hpp"
#include "firmware/compliance_test.hpp"
#include "firmware/console_led_diagnostic_frame.hpp"
#include "firmware/current_game_log_service.hpp"
#include "firmware/current_game_log_storage.hpp"
#include "firmware/current_game_sync_runtime.hpp"
#include "firmware/factory_radio_coordinator.hpp"
#include "firmware/factory_provisioning.hpp"
#include "firmware/hardware_test_mode.hpp"
#include "firmware/local_control_requests.hpp"
#include "firmware/led_runtime_admission.hpp"
#include "firmware/motion_wake_controller.hpp"
#include "modules/water_slosh_session.hpp"
#include "firmware/power_rail.hpp"
#include "firmware/recovery_led_frame.hpp"
#include "firmware/recovery_policy.hpp"
#include "firmware/sleep_controller.hpp"
#include "firmware/settings_persistence_worker.hpp"
#include "modules/board_health_notification.hpp"
#include "modules/support_runtime_stats.hpp"
#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/performance_telemetry.hpp"
#include "hardware/ambient_light_sensors.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/chessboard_hardware.hpp"
#include "hardware/gpio_user_input.hpp"
#include "hardware/hardware.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/lis2dw12_wire.hpp"
#include "hardware/neopixel_led_renderer.hpp"
#include "hardware/pca9555_board_input.hpp"
#include "hardware/pca9555_input_scan.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "hardware/usb_cc_sense.hpp"
#include "hardware/usb_cc_sampler.hpp"
#include "modules/ambient_lux_averager.hpp"
#include "modules/beginner_mode_controller.hpp"
#include "modules/ble_service.hpp"
#include "firmware/startup_led_sequence.hpp"
#include "modules/startup_piece_accent.hpp"
#include "modules/boot_display.hpp"
#include "modules/budget_aware_led_pipeline.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/console_jokes.hpp"
#include "modules/quick_settings_orchestration.hpp"
#include "modules/edge_led_renderer.hpp"
#include "modules/first_player_migration.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/game_persistence_scrub.hpp"
#include "modules/home_state_notification.hpp"
#include "modules/led_power_envelope.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/menu_controller.hpp"
#include "modules/quick_settings_overlay_controller.hpp"
#include "modules/hint_button_gesture_controller.hpp"
#include "modules/reset_hold_gesture_controller.hpp"
#include "modules/rgb.hpp"
#include "modules/runtime_health.hpp"
#include "modules/settings_registry.hpp"
#include "modules/settings_store.hpp"
#include "modules/usb_cc_source_policy.hpp"
#include "modules/upward_output_slew.hpp"
#include "modules/board_identity_persistence.hpp"
#include "modules/board_owner_pairing_feature.hpp"
#include "modules/board_owner_pairing_runtime.hpp"
#include "firmware/board_owner_persistence_runtime.hpp"
#include "runtime/boot_guard.hpp"
#include "runtime/button_input_event_queue.hpp"
#include "runtime/event_bus.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/system_context.hpp"

#ifdef LITCH_TRACE
#include "trace/trace_recorder.hpp"
#endif

#include "firmware/led_animation_task.hpp"

namespace firmware_internal {

inline constexpr const char* LED_BROWNOUT_LIMIT_LATCH_SETTING_KEY =
    "LED_BROWNOUT_LIMIT_LATCH";

inline fxe::SettingsStoreConfig MakeFirmwareSettingsStoreConfig(
    bool* litchcraftStorageRecovered = nullptr) {
    fxe::SettingsStoreConfig cfg;
    cfg.path = "/settings.json";
    cfg.allowSynchronousDeferredSaves = false;
    cfg.migrateFunc = [](JsonDocument& doc) -> bool {
        bool mutated = modules::first_player_migration::CanonicalizeAndCleanup(doc);
        return modules::settings_registry::CanonicalizeLoadedSettings(doc) ||
               mutated;
    };
    cfg.scrubFunc = [litchcraftStorageRecovered](JsonDocument& doc, fxe::SettingsRemovedKeys* removed) -> bool {
        const bool artworkScrubbed = modules::ScrubLitchcraftRecord(doc, removed);
        if (artworkScrubbed && litchcraftStorageRecovered != nullptr) {
            *litchcraftStorageRecovered = true;
        }
        return modules::ScrubGamePersistenceSettings(doc, removed) || artworkScrubbed;
    };
    return cfg;
}

struct FirmwareSystem {
    // Selected once before normal owners are initialized. It never changes in place.
    bool factoryBootOnly = false;
    bool factoryVerifierBoot = false;
    bool factoryRadioReleasedForSave = false;
    bool factoryPersistenceStartFailed = false;
    firmware::FactoryCompletionReboot factoryCompletion;
    firmware::FactoryBootIntent pendingFactoryBootIntent = firmware::FactoryBootIntent::None;
    firmware::LedRuntimeAdmission ledRuntimeAdmission;
    runtime::BootGuard bootGuard;
    bool safeModeActive = false;
    bool otaBootAwaitingHealth = false;
    bool earlyUpdaterInitialized = false;
    bool otaPlatformAdapterBound = false;
    bool recoveryUpdaterStarted = false;
    uint32_t startupOtaWorkerMs = 0U;
    uint32_t startupOtaCheckMs = 0U;
    firmware::RuntimeBootHealthState runtimeBootHealth;
    firmware::RecoveryPolicy recoveryPolicy;
    PeriodicTaskCadence recoveryConsoleCadence{5U};
    bool recoveryControlsConfigured = false;
    bool recoveryFrameInitialized = false;
    firmware::RecoveryLedFrame recoveryLastFrame{};

#if defined(LITCH_HW_VARIANT_S3)
    hardware::RuntimeBoardHardwareProfile runtimeHardware{};
#else
    hardware::RuntimeBoardHardwareProfile runtimeHardware =
        hardware::RuntimeProfileForC3();
#endif

    // Boot RNG seed (best-effort). Used to seed trace and VS-CPU move selection.
    uint32_t rngSeed = 0;
    modules::BoardIdentityPersistence boardIdentity;
    firmware::FactoryProvisioning factoryProvisioning;

    firmware::PowerRail powerRail;
    bool powerRailInitialized = false;
    firmware::SleepController sleep;
    firmware::HardwareTestMode hardwareTest;
    firmware::FactoryRadioCoordinator factoryRadio;
    bool hardwareTestMotionInitialized = false;
    uint32_t hardwareTestMotionBaseline = 0U;
    uint32_t hardwareTestMotionRunId = 0U;
    hardware::Lis2dw12Wire motionWire{Wire};
    firmware::MotionWakeController motionWake;
    modules::WaterSloshSession waterSlosh;
    modules::BleIdentifyPresentation identifyPresentation;
    uint32_t waterSloshSampleMs = 0U;
    uint32_t litchcraftShakeSampleMs = 0U;
    firmware::compliance::Session comptest;
    firmware::amptest::Session amptest;
    firmware::amptest::VerifySession amptestVerify;
    modules::ConsoleJokes consoleJokes;

    hardware::RmtFlashArbiter rmtFlashArbiter;
    bool litchcraftStorageRecovered = false;
    fxe::SettingsStore settings{MakeFirmwareSettingsStoreConfig(&litchcraftStorageRecovered)};
    bool settingsLoadAttempted = false;
    bool settingsLoadSucceeded = false;
    modules::BoardHealthLine lastBoardHealthLine;
    uint8_t bootStorageError = 0;
    modules::PersistenceDegradation persistenceDegradation =
        modules::PersistenceDegradation::None;
    // Written by the settings worker's durable-snapshot callback and consumed
    // on the main loop before publishing board health.
    std::atomic<bool> durableSettingsSnapshotObserved{false};
    modules::SupportRuntimeStats supportRuntimeStats;
    firmware::SettingsPersistenceWorker settingsPersistence{
        MakeFirmwareSettingsStoreConfig()};
    firmware::BoardOwnerPersistenceRuntime boardOwnerPersistence{
        settings, settingsPersistence, boardIdentity};
    modules::BoardOwnerPairingRuntime boardOwnerPairing{
        boardOwnerPersistence};
    bool ownerPairingPrepared = false;
    firmware::BoardStorageRecoveryPublication storageRecoveryPublication;
    firmware::CurrentGameLogStorage currentGameLogStorage{
        settingsPersistence};
    firmware::CurrentGameLogService currentGameLogService{
        currentGameLogStorage.Ops()};
    firmware::CurrentGameSyncRuntime currentGameSync{
        settingsPersistence,
        currentGameLogService,
        currentGameLogStorage.Ops(),
        settings};
    fxe::ElapsedTime sleepPersistenceElapsed;
    bool rebootPersistencePending = false;
    bool rebootPersistenceStarted = false;
    bool rebootToBootloader = false;
    bool settingsClearPending = false;
    uint32_t settingsClearReceipt = 0U;
    bool settingsClearPrimarySaved = false;
    fxe::ElapsedTime rebootPersistenceElapsed;

    modules::LitchMaxEngine engine;

    runtime::GameStateMachine stateMachine = runtime::GameStateMachine();
    runtime::AppEventQueue appEventQueue = runtime::AppEventQueue();

    modules::GamePersistenceCoordinator gamePersistence =
        modules::GamePersistenceCoordinator(
            settings,
            engine,
            stateMachine,
            &appEventQueue);
    fxe::PerformanceTelemetry telemetry;

    hardware::Pca9555InputScan inputScan = hardware::Pca9555InputScan(Wire);
    hardware::Pca9555BoardInput boardInput = hardware::Pca9555BoardInput(inputScan);
    hardware::ChessboardHardware board = hardware::ChessboardHardware(boardInput);
    runtime::RuntimeHealthReducer runtimeHealth;
    modules::BoardInputRuntimeHealth boardInputHealth{runtimeHealth};

    hardware::AmbientLightSensors ambientLights = hardware::AmbientLightSensors();
    modules::AmbientLuxAverager ambientLux = modules::AmbientLuxAverager();

    modules::physical_palette_shortcut::EditPreview paletteEditPreview;
    modules::BoardStylePreview boardStylePreview;
    hardware::GpioUserInput gpioUserInput = hardware::GpioUserInput();
    runtime::ButtonInputEventQueue buttonInputQueue = runtime::ButtonInputEventQueue();

    modules::MenuLevelOverlay menuOverlay = modules::MenuLevelOverlay();
    uint32_t setupPresentationObservationSequence = 0U;
    bool setupPresentationObservationValid = false;
    modules::EdgeLedCueController edgeCue;


    firmware::StartupLedSequence startupLeds;
    modules::BootDisplay bootDisplay;
    bool startupBoardSnapshotObserved = false;
    uint32_t startupEntryMs = 0U;
    uint32_t startupSettingsMs = 0U;
    bool startupHardwareTestRequested = false;
    modules::BleService ble{&rmtFlashArbiter};
    modules::HomeStateNotificationPublisher homeStatePublisher;

    // Reset-game hold gesture state.
    bool gameResetHoldActive = false;
    uint32_t gameResetHoldStartMs = 0;
    modules::UiOwner gameResetHoldOwner = modules::UiOwner::None;
    bool gameResetHoldTriggered = false;
    bool gameResetHoldRejected = false;

    bool quickSettingsOverlayActive = false;
    modules::UiOwner quickSettingsOverlayOwner = modules::UiOwner::None;
    uint8_t quickSettingsOverlayPhysicalEnd = 0U;
    uint8_t quickSettingsOverlaySelectedIndex = 0;
    fxe::ElapsedTime quickSettingsOverlayIdleElapsed;
    modules::QuickSettingsOverlayController quickSettingsOverlayController = modules::QuickSettingsOverlayController();
    modules::ResetHoldGestureController resetHoldController{};
    modules::HintButtonGestureController hintButtonController{};
    modules::ControlLedFeedback controlFeedback;
    modules::BeginnerModeController beginnerModeController{settings};
    modules::ControlMutationService controlMutation;
    LocalControlRequestTracker localControlRequests;
    bool beginnerModeForcedHintDisplay = false;
    modules::UiOwner beginnerModeStingerOwner = modules::UiOwner::None;
    uint32_t beginnerModeStingerStartMs = 0;
    bool beginnerModeStingerEnabling = false;

    bool hallMapActive = false;
    uint32_t hallMapSeq = 0;
    uint32_t hallMapLastUpdate = 0;

    modules::BudgetAwareLedPipelineStats ledPowerStats = modules::BudgetAwareLedPipelineStats();
    uint16_t ledPowerLimitMa = 0;
    uint16_t lastUsbCc1 = 0;
    uint16_t lastUsbCc2 = 0;
    uint32_t lastUsbCcSampleMs = 0;
    uint32_t lastUsbCcMonitorSampleMs = 0;
    uint16_t lastUsbCcMonitorRaw = 0;
    bool usbCcSampleValid = false;
    modules::UsbCcSourcePolicyState usbCcSourcePolicy;
    hardware::UsbCcSampler usbCcSampler;
    modules::UpwardOutputSlew sourceOutputLimit;
    hardware::UsbCcThresholds usbCcThresholdPolicy{};
    bool usbCcThresholdPolicyValid = false;
    hardware::UsbCcLevel lastUsbCcAdvertisedLevel = hardware::UsbCcLevel::Unknown;

    // Active I2C bus clock in Hz (Wire.setClock).
    uint32_t i2cClockHz = 100000U;

    // NeoPixelBus allocates internally; keep this out of the FirmwareSystem constructor path
    // so settings/LittleFS can mount before any LED driver heap activity.
    alignas(hardware::NeoPixelLedRenderer) uint8_t m_ledsStorage[sizeof(hardware::NeoPixelLedRenderer)] = {};
    bool m_ledsConstructed = false;
    bool ledOutputInitialized = false;
    bool bootFirstNonzeroRenderLogged = false;

    hardware::NeoPixelLedRenderer& Leds() {
        if (!m_ledsConstructed) {
            const hardware::BoardHardwareProfile& profile =
                runtimeHardware.resolved;
            // V1 has 32 unused pixels after the 192 board pixels on the same
            // chain. Keep clearing them while exposing only the semantic board
            // pixels to the renderer.
            const uint16_t physicalStatusCount =
                (!profile.isS3 &&
                 profile.statusLedPin == profile.boardLedPin)
                    ? static_cast<uint16_t>(modules::EDGE_LED_COUNT)
                    : profile.leds.physicalStatusLedCount;
            new (m_ledsStorage) hardware::NeoPixelLedRenderer(
                profile.leds.physicalBoardLedCount,
                profile.boardLedPin,
                physicalStatusCount,
                profile.statusLedPin,
                profile.revision == hardware::BoardRevision::V3_3S3);
            reinterpret_cast<hardware::NeoPixelLedRenderer*>(m_ledsStorage)
                ->SetPowerEnabledState(powerRail.LedOutputEnabledState());
            m_ledsConstructed = true;
        }

        return *reinterpret_cast<hardware::NeoPixelLedRenderer*>(m_ledsStorage);
    }

    const hardware::NeoPixelLedRenderer& Leds() const {
        return *reinterpret_cast<const hardware::NeoPixelLedRenderer*>(m_ledsStorage);
    }

    firmware::LedAnimationTask ledAnimation;
    uint32_t ledSourceResetEpoch = 0U;
    uint16_t ledSourceResetLimitMa = 0U;
    uint32_t ledBootCancelEpoch = 0U;
    bool ledLastCommandCompleted = false;

    // Physical-order canvas accumulated by repeated `setled board` commands.
    firmware::ConsoleLedDiagnosticFrame consoleLedDiagnosticFrame;


#ifdef LITCH_TRACE
    trace::TraceRecorder recorder = trace::TraceRecorder();
#endif

    runtime::SystemContext ctx;
};

using PhysicalLedFrame =
    std::array<modules::Rgb, hardware::MAX_PHYSICAL_LED_COUNT>;

using AcceptedPhysicalLedFrame = hardware::AcceptedPhysicalLedFrame;

FirmwareSystem& GetSystem();
runtime::SystemContext& GetSystemContext();

bool GetFlipSidesSetting(const fxe::SettingsStore& settings);
bool GetFirstPlayerIsP2Setting(const fxe::SettingsStore& settings);
bool GetVsCpuPlayerIsP2Setting(const fxe::SettingsStore& settings);
bool IsLedBrownoutLimitLatched(const modules::SettingsStore& settings);
bool SetLedBrownoutLimitLatched(modules::SettingsStore& settings, bool latched);
hardware::UsbCcThresholds LoadUsbCcThresholds(const modules::SettingsStore& settings);
void SetSystemI2cClock(FirmwareSystem& sys, uint32_t hz);
uint16_t GetActiveLedCurrentLimitMa(const FirmwareSystem& sys);
uint16_t GetActiveTotalCurrentLimitMa(const FirmwareSystem& sys);
uint16_t GetFixedSystemCurrentMa();
void InitializeLedPowerBudgetAfterSettingsLoad(FirmwareSystem& sys);
void RefreshLedPowerSourceStatus(FirmwareSystem& sys, uint32_t nowMs, bool forceSample = false);
void PollLedPowerSourceStatus(FirmwareSystem& sys, uint32_t nowMs);
// Applies a normalized runtime-health fact and immediately forwards the
// canonical snapshot to the game/clock owner.
bool ApplyRuntimeHealthFactAndForward(
    FirmwareSystem& sys, const runtime::RuntimeHealthFact& fact);
// Reset source confirmation and blank output before a blocking operation.
void ResetUsbCcSourceAuthorization(FirmwareSystem& sys);
// Performs the single validated clear sequence used by `ledpower clear`.
bool ClearLedPowerProtectionLatch(FirmwareSystem& sys, uint32_t nowMs);
uint16_t EstimatePhysicalFrameLedCurrentMa(FirmwareSystem& sys,
                                           const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& physicalFrame,
                                           uint16_t brightnessScale256);
uint16_t EstimatePhysicalFrameLedCurrentMa(FirmwareSystem& sys,
                                           const PhysicalLedFrame& physicalFrame,
                                           uint16_t brightnessScale256);
void ReleaseHardwareTestInput(FirmwareSystem& sys);

void RenderPhysicalFrameWithPowerBudget(FirmwareSystem& sys,
                                        const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& physicalFrame,
                                        uint16_t brightnessScale256);
void RenderPhysicalFrameWithPowerBudget(FirmwareSystem& sys,
                                        const PhysicalLedFrame& physicalFrame,
                                        uint16_t brightnessScale256,
                                        const modules::LedFrameAccent& accent = {});
// Pre-initialization boot visuals must not unlock the normal source envelope.
void RenderPhysicalFrameWithConservativePowerBudget(
    FirmwareSystem& sys,
    const std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>& physicalFrame,
    uint16_t brightnessScale256);
void RenderPhysicalFrameWithConservativePowerBudget(
    FirmwareSystem& sys,
    const PhysicalLedFrame& physicalFrame,
    uint16_t brightnessScale256);
// Recovery is capped independently of the normal/source-confirmed envelope on
// every board, including the C3 virtual-perimeter adapter.
void RenderPhysicalFrameWithRecoveryPowerBudget(
    FirmwareSystem& sys,
    const PhysicalLedFrame& physicalFrame,
    uint16_t brightnessScale256);

// Brightness scale (Q8.8) used by the normal renderer.
// Exposed so sleep fade-out can match the visible output.
uint16_t ComputeEffectiveBrightnessScale256(const fxe::SettingsStore& settings,
                                            const modules::BootFadeIn& bootFade);

void RecordBootRenderReturn(FirmwareSystem& sys,
                            const fxe::ElapsedTime& bootRender,
                            uint16_t brightnessScale256);
void EnsureSettingsLoaded();
void EnsureConsoleInitialized();
bool QuiesceLedRail(FirmwareSystem& sys);

// On ESP32-C3 native USB CDC, early Serial writes can crash or vanish.
// This keeps console/log output muted until the host has interacted.
void MaybeEnableConsoleOutput();

void ConfigureCadencesOnce(bool& configured,
                           PeriodicTaskCadence& ioCadence,
                           PeriodicTaskCadence& engineCadence,
                           PeriodicTaskCadence& settingsCadence,
                           PeriodicTaskCadence& bleCadence,
                           PeriodicTaskCadence& sensorCadence,
                           PeriodicTaskCadence& ledCadence
#ifdef LITCH_TRACE
                           ,
                           PeriodicTaskCadence& traceCadence
#endif
);

void InitCadences(PeriodicTaskCadence& ioCadence,
                  PeriodicTaskCadence& engineCadence,
                  PeriodicTaskCadence& settingsCadence,
                  PeriodicTaskCadence& bleCadence,
                  PeriodicTaskCadence& sensorCadence,
                  PeriodicTaskCadence& ledCadence
#ifdef LITCH_TRACE
                  ,
                  PeriodicTaskCadence& traceCadence
#endif
);

void PollButtonsAndDispatch();
void PollSensorsAndDispatch();
firmware::LedRenderAttemptResult RenderLeds(bool waitForCompletion = false);
void ConsumeLedPresentation(FirmwareSystem& sys);
void ConfigureLitchcraftPresentationAdmission(FirmwareSystem& sys);
bool BeginPhysicalLitchcraftPresentation(FirmwareSystem& sys,
    modules::LitchcraftSession::PresentationAccess access);
void CancelBootPresentation(FirmwareSystem& sys);
bool ClearLedOutput(FirmwareSystem& sys);
bool EnableLedRail(FirmwareSystem& sys);
bool LedOutputCompleted(const FirmwareSystem& sys);
firmware::LedOutputPresentation CaptureLedOutputPresentation(FirmwareSystem& sys, bool conservative = false);

void SleepUntilNextWork(uint32_t remainingMs);

uint32_t ComputeNextWakeMs(const PeriodicTaskCadence& ioCadence,
                           const PeriodicTaskCadence& engineCadence,
                           const PeriodicTaskCadence& settingsCadence,
                           const PeriodicTaskCadence& bleCadence,
                           const PeriodicTaskCadence& sensorCadence,
                           const PeriodicTaskCadence& ledCadence
#ifdef LITCH_TRACE
                           ,
                           const PeriodicTaskCadence& traceCadence
#endif
);

#ifdef LITCH_TRACE
trace::TraceRecorder& GetTraceRecorder();
trace::TraceSettingsSnapshot GetTraceSettingsSnapshot(const fxe::SettingsStore& settings);
#endif

} // namespace firmware_internal

#endif
