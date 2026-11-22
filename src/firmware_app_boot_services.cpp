#ifdef ARDUINO
#include "firmware/ota_boot_health.hpp"

#include <Arduino.h>

#include <string_view>
#include <cstring>

#include "firmware/firmware_app_internal.hpp"
#include "firmware/board_storage_recovery.hpp"
#include "firmware/litchcraft_persistence_runtime.hpp"
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_recovery_internal.hpp"
#include "firmware/boot_platform_timing.hpp"
#include "firmware/usb_boot_probe.hpp"

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/millis.hpp"

#include "hardware/board_revision_runtime.hpp"
#include "hardware/hardware_test_boot_entry.hpp"
#include "hardware/i2c_clock_settings.hpp"
#include "logger.hpp"
#include "modules/orientation.hpp"
#include "modules/volatile_persistence_session.hpp"
#include "runtime/progress_watchdog.hpp"
#include "runtime/square_mapping.hpp"

namespace {

bool StopBleBeforeSleep(void* context) {
    if (context == nullptr) {
        return false;
    }

    return static_cast<modules::BleService*>(context)->Stop();
}

void SuspendNormalLedPowerBeforeBlockingBleDiagnostic(void* context) {
    if (context == nullptr) {
        return;
    }

    auto* sys = static_cast<firmware_internal::FirmwareSystem*>(context);
    firmware_internal::ResetUsbCcSourceAuthorization(*sys);
}

void OnBleSettingsMutationCommitted(void* context, const char* key) {
    (void)context;
    firmware_internal::BeforeSettingsMutationCallback(
        key != nullptr ? std::string_view(key) : std::string_view{});
}

modules::ControlCoordinatorResult CheckControlStartOnlyGate(
    void* context,
    const char* canonicalKey) {
    return static_cast<firmware_internal::FirmwareSystem*>(context)
        ->gamePersistence.AdmitControlStartOnlyMutation(canonicalKey);
}

void BeforeControlSettingMutation(void* context, const char* key) {
    if (key != nullptr && (std::strcmp(key, "VS_CPU_LEVEL") == 0 ||
                           std::strcmp(key, "VS_CPU_PLAYER") == 0)) {
        auto& gestures = static_cast<firmware_internal::FirmwareSystem*>(context)->hintButtonController;
        gestures.Cancel(modules::UiOwner::P1);
        gestures.Cancel(modules::UiOwner::P2);
    }
    firmware_internal::BeforeSettingsMutationCallback(
        key != nullptr ? std::string_view(key) : std::string_view{});
}


void CancelControlCpuWork(void* context) {
    firmware_internal::CancelBootPresentation(
        *static_cast<firmware_internal::FirmwareSystem*>(context));
    static_cast<firmware_internal::FirmwareSystem*>(context)
        ->gamePersistence.RequestCancelCpuWork();
}

void CancelControlHintWork(void* context) {
    firmware_internal::CancelBootPresentation(
        *static_cast<firmware_internal::FirmwareSystem*>(context));
    static_cast<firmware_internal::FirmwareSystem*>(context)
        ->gamePersistence.RequestCancelHintWorkAndRestart();
}

modules::ControlCoordinatorResult RequestControlHint(void* context,
                                                      uint16_t requestId) {
    firmware_internal::CancelBootPresentation(
        *static_cast<firmware_internal::FirmwareSystem*>(context));
    return static_cast<firmware_internal::FirmwareSystem*>(context)
        ->gamePersistence.RequestControlHint(requestId);
}

modules::ControlCoordinatorResult RequestControlUndo(void* context,
                                                      uint16_t requestId,
                                                      uint8_t moveCount) {
    firmware_internal::CancelBootPresentation(
        *static_cast<firmware_internal::FirmwareSystem*>(context));
    return static_cast<firmware_internal::FirmwareSystem*>(context)
        ->gamePersistence.RequestControlUndo(requestId, moveCount);
}

modules::ControlCoordinatorResult RequestControlNewGame(void* context,
                                                         uint16_t requestId) {
    firmware_internal::CancelBootPresentation(
        *static_cast<firmware_internal::FirmwareSystem*>(context));
    return static_cast<firmware_internal::FirmwareSystem*>(context)
        ->gamePersistence.RequestControlNewGame(requestId);
}

modules::ControlCoordinatorResult RequestControlSleep(void* context) {
    firmware_internal::CancelBootPresentation(
        *static_cast<firmware_internal::FirmwareSystem*>(context));
    auto& sys = *static_cast<firmware_internal::FirmwareSystem*>(context);
    if (sys.safeModeActive || sys.hardwareTest.IsActive() ||
        sys.amptest.IsActive() || sys.amptestVerify.IsActive() ||
        sys.comptest.IsActive() || sys.rebootPersistencePending ||
        sys.ble.IsOtaInProgress() ||
        runtime::IsRuntimeHealthPlayPaused(sys.runtimeHealth.Snapshot())) {
        return modules::ControlCoordinatorResult::Busy;
    }
    return sys.sleep.RequestSleep(firmware::SleepRequestOrigin::App, fxe::Millis(), false)
        ? modules::ControlCoordinatorResult::Applied
        : modules::ControlCoordinatorResult::Busy;
}

modules::ControlCoordinatorResult PollControlAction(void* context,
                                                     uint16_t requestId) {
    return static_cast<firmware_internal::FirmwareSystem*>(context)
        ->gamePersistence.PollControlAction(requestId);
}

modules::ControlActionState GetControlActionState(void* context) {
    const auto& sys =
        *static_cast<firmware_internal::FirmwareSystem*>(context);
    return sys.gamePersistence.IsUndoActionPending()
               ? modules::ControlActionState::UndoPending
               : modules::ControlActionState::Idle;
}

void OnControlCoachingChanged(void* context,
                             modules::UiOwner owner,
                             bool enabled) {
    firmware_internal::CancelBootPresentation(
        *static_cast<firmware_internal::FirmwareSystem*>(context));
    auto& sys = *static_cast<firmware_internal::FirmwareSystem*>(context);
    sys.beginnerModeStingerOwner = owner;
    sys.beginnerModeStingerEnabling = enabled;
    sys.beginnerModeStingerStartMs = fxe::Millis();
    sys.waterSlosh.NoteControl(sys.beginnerModeStingerStartMs);
}

modules::ControlMutationCallbacks MakeControlMutationCallbacks(
    firmware_internal::FirmwareSystem& sys) {
    modules::ControlMutationCallbacks callbacks;
    callbacks.context = &sys;
    callbacks.allowVolatileUserSettings =
        sys.persistenceDegradation != modules::PersistenceDegradation::None;
    callbacks.beforeSettingMutation = &BeforeControlSettingMutation;
    callbacks.settingProjected = &firmware_internal::ProjectControlFeedbackSetting;
    callbacks.checkStartOnlyGate = &CheckControlStartOnlyGate;
    callbacks.cancelCpuWork = &CancelControlCpuWork;
    callbacks.cancelHintWorkAndRestart = &CancelControlHintWork;
    callbacks.requestHint = &RequestControlHint;
    callbacks.requestUndo = &RequestControlUndo;
    callbacks.requestNewGame = &RequestControlNewGame;
    callbacks.requestSleep = &RequestControlSleep;
    callbacks.requestStorageReformat = &firmware_internal::RequestBoardStorageRecovery;
    callbacks.pollStorageReformat = &firmware_internal::PollBoardStorageRecovery;
    callbacks.pollAction = &PollControlAction;
    callbacks.getActionState = &GetControlActionState;
    callbacks.coachingChanged = &OnControlCoachingChanged;
    return callbacks;
}

void ConfigureMotionInterruptInput(void*, const uint8_t pin) {
    pinMode(pin, INPUT);
}

bool ReadMotionInterruptHigh(void*, const uint8_t pin) {
    return digitalRead(pin) != LOW;
}

bool PrepareMotionWakeSource(void* context) {
    auto* sys = static_cast<firmware_internal::FirmwareSystem*>(context);
    return sys != nullptr &&
           sys->motionWake.PrepareForSleep(sys->settings);
}

bool IsMotionWakeSourceArmed(void* context) {
    auto* sys = static_cast<firmware_internal::FirmwareSystem*>(context);
    return sys != nullptr && sys->motionWake.IsWakeReady();
}

bool RecheckRuntimeHealthPowerProtection(void* context) {
    auto* const sys =
        static_cast<firmware_internal::FirmwareSystem*>(context);
    return sys != nullptr &&
           firmware_internal::ClearLedPowerProtectionLatch(
               *sys, fxe::Millis());
}


hardware::Pca9555InputScan* g_expanderScan = nullptr;

void IRAM_ATTR OnExpanderInt0() {
    if (g_expanderScan) {
        g_expanderScan->NotifyExpanderInterrupt(0);
    }
    ++firmware::g_expanderIntCount[0];
}

void IRAM_ATTR OnExpanderInt1() {
    if (g_expanderScan) {
        g_expanderScan->NotifyExpanderInterrupt(1);
    }
    ++firmware::g_expanderIntCount[1];
}

void IRAM_ATTR OnExpanderInt2() {
    if (g_expanderScan) {
        g_expanderScan->NotifyExpanderInterrupt(2);
    }
    ++firmware::g_expanderIntCount[2];
}

void IRAM_ATTR OnExpanderInt3() {
    if (g_expanderScan) {
        g_expanderScan->NotifyExpanderInterrupt(3);
    }
    ++firmware::g_expanderIntCount[3];
}

void ConfigureExpanderInterrupts(firmware_internal::FirmwareSystem& sys) {
    const hardware::BoardHardwareProfile& profile =
        sys.runtimeHardware.resolved;
    if (!profile.hasExpanderInterrupts) {
        return;
    }

    g_expanderScan = &sys.inputScan;

    for (const uint8_t pin : profile.expanderInterrupts) {
        pinMode(pin, INPUT_PULLUP);
    }

    attachInterrupt(digitalPinToInterrupt(profile.expanderInterrupts[0]), OnExpanderInt0, FALLING);
    attachInterrupt(digitalPinToInterrupt(profile.expanderInterrupts[1]), OnExpanderInt1, FALLING);
    attachInterrupt(digitalPinToInterrupt(profile.expanderInterrupts[2]), OnExpanderInt2, FALLING);
    attachInterrupt(digitalPinToInterrupt(profile.expanderInterrupts[3]), OnExpanderInt3, FALLING);
}

} // namespace

namespace firmware_internal {

void FinishStartup() {
    auto& sys = GetSystem();
    InitializeEarlyUpdater(sys);
    const auto& hardwareProfile = sys.runtimeHardware.resolved;
    const auto servicesElapsed = fxe::ElapsedTime::StartNow();
    auto phaseElapsed = fxe::ElapsedTime::StartNow();
    EnsureConsoleInitialized();
    const uint32_t consoleMs = phaseElapsed.ElapsedMs();
    const auto& platform = firmware::PlatformBootTiming();
    logger::Trace(
        "boot: platform_ms usb_begin=%lu usb_end=%lu arduino_begin=%lu arduino_end=%lu setup=%lu watchdog_end=%lu",
        static_cast<unsigned long>(platform.usbBeginMs),
        static_cast<unsigned long>(platform.usbEndMs),
        static_cast<unsigned long>(platform.arduinoBeginMs),
        static_cast<unsigned long>(platform.arduinoEndMs),
        static_cast<unsigned long>(platform.setupBeginMs),
        static_cast<unsigned long>(platform.watchdogEndMs));
    const uint32_t otaWorkerMs = sys.startupOtaWorkerMs;
    const uint32_t otaCheckMs = sys.startupOtaCheckMs;
    // Read-only OTA preflight is already complete. Apply the one-use RTC
    // request only after settings/provisioning can decide factory eligibility.
    ApplyRequestedFactoryBoot(sys);
#if defined(LITCH_ESP32S3)
    if (!sys.safeModeActive && !sys.factoryBootOnly) {
        sys.settingsPersistence.SetLitchcraftFrameSummaryMaintenanceReady(false);
        sys.ble.ReserveBootFirmwareDigest();
    }
#endif

    logger::Trace("boot: rng_seed=%lu", static_cast<unsigned long>(sys.rngSeed));

    if (hardwareProfile.hasCustomI2cPins) {
        Wire.begin(hardwareProfile.i2cSda, hardwareProfile.i2cScl);
    } else {
        Wire.begin();
    }

    SetSystemI2cClock(sys, hardware::i2c_clock_settings::Read(sys.settings));

    const firmware::MotionWakePlatform motionPlatform{
        nullptr,
        &ConfigureMotionInterruptInput,
        &ReadMotionInterruptHigh,
    };
    const bool motionReady = sys.motionWake.Begin(
        hardwareProfile.revision,
        sys.motionWire.Transport(),
        motionPlatform,
        sys.settings);
    if (hardwareProfile.accelerometerInterrupt != hardware::NO_GPIO) {
        logger::Trace(
            "motion: begin=%u pin=%u available=%u id=0x%02x",
            static_cast<unsigned>(motionReady ? 1U : 0U),
            static_cast<unsigned>(hardwareProfile.accelerometerInterrupt),
            static_cast<unsigned>(sys.motionWake.IsAvailable() ? 1U : 0U),
            static_cast<unsigned>(
                sys.motionWake.GetSensor().GetDeviceId()));
    }
    if (!motionReady &&
        hardwareProfile.accelerometerInterrupt != hardware::NO_GPIO) {
        logger::Warn(
            "motion: V3 accelerometer unavailable; enabled motion wake will refuse sleep");
    }
    sys.sleep.ConfigureOptionalActiveLowWakeSource(
        hardwareProfile.accelerometerInterrupt,
        &PrepareMotionWakeSource,
        &IsMotionWakeSourceArmed,
        &sys);

    if (sys.factoryBootOnly) {
        ConfigureBoardIdentityPersistence(sys);
        sys.boardInput.Begin();
        sys.boardInput.SetInterruptDriven(hardwareProfile.hasExpanderInterrupts);
        ConfigureExpanderInterrupts(sys);
        if (hardwareProfile.hasLightSensors) {
            sys.ambientLights.Begin(Wire);
            sys.ambientLux.Reset();
        }
        sys.ble.SetFactoryDutTaskStorage(sys.settingsPersistence.FactoryTaskStorage());
        sys.ble.SetFactoryBootOnly(true, sys.factoryVerifierBoot);
        sys.ble.SetRuntimeHealthProvider(&sys.runtimeHealth);
        sys.ble.SetRuntimeHealthRecheckCallback(&RecheckRuntimeHealthPowerProtection, &sys);
#if defined(LITCH_BLE_OTA) && defined(LITCH_ESP32S3)
        sys.ble.SetOtaStorePersistenceCallbacks(nullptr, nullptr, &sys, &FactoryOtaBeginBusy);
#endif
        sys.ble.Begin(sys.settings);
        sys.ble.SetFactoryVerifierConfiguredEnabled(sys.factoryVerifierBoot);
        if (sys.factoryVerifierBoot) {
            modules::HardwareTestRecord saved;
            if (modules::DecodeHardwareTestRecord(sys.settings.ReadValue(
                    modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""), saved)) {
                sys.hardwareTest.RestoreSavedVerifier(saved, fxe::Millis());
            }
        }
        sys.sleep.SetPreSleepCallback(&StopBleBeforeSleep, &sys.ble);
        if (sys.startupHardwareTestRequested) {
            sys.hardwareTest.Enter(sys.runtimeHardware.detectedRevision, fxe::Millis());
        }
        RefreshLedPowerSourceStatus(sys, fxe::Millis(), false);
        const auto firstSemanticRender = RenderLeds(true);
        sys.ledRuntimeAdmission.NoteRenderOutcome(firstSemanticRender, fxe::Millis());
#if defined(LITCH_HW_VARIANT_S3)
        firmware::RecordUsbBootServicesReady();
#endif
        logger::Info("boot: factory essentials ready; normal game, artwork, history and settings worker deferred");
        return;
    }
    sys.gamePersistence.SetRngSeed(sys.rngSeed);
    runtime::NoteProgress(0U, runtime::ProgressStage::Storage);
    ConfigureBoardIdentityPersistence(sys);
    if (sys.settingsLoadAttempted && sys.settingsLoadSucceeded) {
        sys.supportRuntimeStats.BeginBoot(sys.settings.Doc());
    }
    sys.settingsPersistence.SetSupportRuntimeStats(&sys.supportRuntimeStats);
    runtime::NoteProgress(0U, runtime::ProgressStage::GameRestore);
    if (!modules::LoadGameForPersistenceSession(
            sys.settings, sys.gamePersistence, sys.persistenceDegradation)) {
        logger::Error("boot: volatile game initialization failed; entering recovery");
        EnterSafeModeRecovery(sys, firmware::RecoveryCause::PersistedStateCorrupt);
        return;
    }
    uint64_t bootResumeOccupancy = 0ULL;
    const bool bootResumableGame =
        sys.gamePersistence.TryGetBootResumeOccupancy(bootResumeOccupancy);
    // LoadGameForPersistenceSession arms the king-identification assist only
    // for a provably untouched starting position. Preserve that decision across
    // every boot; X remains the explicit per-boot dismissal. In-progress games
    // never arm the assist during restoration.
    runtime::NoteProgress(0U, runtime::ProgressStage::Controls);
    firmware_internal::ConfigureBoardControlMutations(sys, MakeControlMutationCallbacks(sys));

    sys.ble.SetBeforeBlockingDiagnosticCallback(
        &SuspendNormalLedPowerBeforeBlockingBleDiagnostic, &sys);
    sys.ble.SetSettingsMutationCallback(
        &OnBleSettingsMutationCommitted, &sys);
    sys.ble.SetControlMutationService(&sys.controlMutation);
    sys.ble.SetLitchcraftCoordinator(&sys.gamePersistence);
    sys.gamePersistence.Litchcraft().Configure(
        static_cast<uint8_t>(hardwareProfile.revision),sys.rngSeed);
    sys.gamePersistence.Litchcraft().ConfigureShakeErase(
        hardware::HasFourLedSquares(hardwareProfile.revision) &&
        sys.motionWake.IsAvailable());
    sys.ble.SetRuntimeHealthProvider(&sys.runtimeHealth);
    if (hardwareProfile.isS3) {
        sys.ble.SetRuntimeHealthRecheckCallback(
            &RecheckRuntimeHealthPowerProtection, &sys);
    }
    const bool boardRotate = GetFlipSidesSetting(sys.settings);
    const bool firstPlayerIsP2 = GetFirstPlayerIsP2Setting(sys.settings);
    const bool squareRotate = modules::ComputeSquareRotate(
        boardRotate, firstPlayerIsP2, sys.gamePersistence.Litchcraft().IsActive());
    runtime::NoteProgress(0U, runtime::ProgressStage::Sensors);
    sys.boardInput.Begin();
    sys.boardInput.SetInterruptDriven(hardwareProfile.hasExpanderInterrupts);
    ConfigureExpanderInterrupts(sys);

    uint32_t ambientMs = 0U;
    if (hardwareProfile.hasLightSensors) {
        phaseElapsed.Reset();
        sys.ambientLights.Begin(Wire);
        sys.ambientLux.Reset();
        (void)sys.ambientLux.ComputeSmoothedAmbientFactor01(fxe::Millis());
        ambientMs = phaseElapsed.ElapsedMs();
    }

    runtime::NoteProgress(0U, runtime::ProgressStage::Render);
    sys.board.SetFlipSides(squareRotate);
    sys.board.Resync();

    uint64_t rawOcc64 = 0;
    if (sys.boardInput.TryReadSnapshot(rawOcc64)) {
        sys.startupBoardSnapshotObserved = true;
        uint64_t logicalOcc64 = 0;
        for (uint8_t rawSquare = 0; rawSquare < 64; ++rawSquare) {
            if ((rawOcc64 & (1ULL << rawSquare)) == 0) {
                continue;
            }

            const uint8_t logicalSquare = runtime::MapSquareIndex(rawSquare, squareRotate);
            logicalOcc64 |= (1ULL << logicalSquare);
        }
        sys.stateMachine.ApplyPhysicalOccupancySnapshot(logicalOcc64);

        if (!sys.stateMachine.IsPhysicalOccupancyConsistentWithInteraction() &&
            !sys.gamePersistence.ShouldDeferInitialPlacementForVariantDetection(
                logicalOcc64)) {
            (void)sys.stateMachine.ForceEnterPlacementMode();
        }
    }

    runtime::NoteProgress(0U, runtime::ProgressStage::Storage);
    phaseElapsed.Reset();
    const bool currentGameSyncConfigured =
        !sys.settings.IsDisabled() &&
        sys.currentGameSync.Configure(sys.settings.Doc());
    // A disabled source still needs its existing worker for explicit recovery.
    const bool settingsWorkerStarted =
        sys.settingsPersistence.BindFlashArbiter(sys.rmtFlashArbiter) &&
        sys.settingsPersistence.Begin(sys.settings);
    if (modules::OWNER_PAIRING_ENABLED) {
        sys.ownerPairingPrepared = settingsWorkerStarted &&
            !sys.settings.IsDisabled() && sys.boardOwnerPersistence.Begin();
        // Bind even when storage preparation fails: an enabled private build
        // must remain unavailable instead of reverting to unprotected access.
        (void)sys.ble.ConfigureOwnerPairing(
            sys.boardOwnerPairing, sys.boardOwnerPersistence, sys.boardIdentity);
    }
    if (!sys.settings.IsDisabled()) {
        sys.gamePersistence.SetReplacementAdmissionCallback(
            [](void* context, const modules::GameHistoryPersistenceState& current,
               const modules::GamePersistenceCoordinator::ReplacementKind kind) {
                return static_cast<firmware::CurrentGameSyncRuntime*>(context)
                    ->CheckReplacementAdmission(current, kind);
            }, &sys.currentGameSync);
    }
    if (currentGameSyncConfigured && settingsWorkerStarted) {
        sys.ble.SetConnectionGameHistoryIngressCallback(
            [](void* context, const uint8_t* bytes, const size_t size,
               const uint32_t connectionEpoch, const bool otaBusy) {
                return static_cast<firmware::CurrentGameSyncRuntime*>(context)
                    ->Admit(bytes, size, connectionEpoch, otaBusy);
            },
            &sys.currentGameSync);
        sys.ble.SetConnectionGameHistoryOutputCallbacks(
            [](void* context, uint8_t* out, const size_t capacity,
               size_t& outSize, uint32_t& token,
               const uint32_t connectionEpoch) {
                return static_cast<firmware::CurrentGameSyncRuntime*>(context)
                    ->Peek(out, capacity, outSize, token, connectionEpoch);
            },
            [](void* context, const uint32_t token,
               const uint32_t connectionEpoch) {
                static_cast<firmware::CurrentGameSyncRuntime*>(context)
                    ->Accept(token, connectionEpoch);
            },
            &sys.currentGameSync);
    } else if (!sys.settings.IsDisabled()) {
        if (!currentGameSyncConfigured) {
            logger::Warn(
                "current_game_sync: storage owner unavailable; capability disabled");
        }
    }
    if (!sys.settings.IsDisabled() && !settingsWorkerStarted) {
        logger::Warn(
            "settings_io: worker start failed; continuing in RAM without saved reboot recovery");
        sys.settings.DisablePersistenceForSession();
        sys.controlMutation.AllowVolatileUserSettings();
    }
#if defined(LITCH_BLE_OTA) && defined(LITCH_ESP32S3)
    sys.ble.SetOtaStorePersistenceCallbacks(
        [](void* context) {
            static_cast<FirmwareSystem*>(context)->settingsPersistence
                .LitchcraftFrameStream().RequestImmediateSave();
        },
        [](void* context) {
            return LifecycleStoreSavePending(
                *static_cast<FirmwareSystem*>(context));
        }, &sys, &BoardStorageRecoveryBlocksOta);
    if (sys.settingsPersistence.IsStarted()) {
        sys.ble.SetLitchcraftFrameIngressCallbacks(
            [](void* context, modules::BleOtaChunkQueue& queue,
               const modules::litchcraft_frame_store::StoreProjection projection,
               const uint32_t ownerEpoch, const uint16_t packetMaximum) {
                static_cast<firmware::SettingsPersistenceWorker*>(context)
                    ->NotifyLitchcraftFrameIngress(
                        queue, projection, ownerEpoch, packetMaximum);
            },
            [](void* context, const uint8_t* bytes, const size_t size,
               const uint32_t ownerEpoch) {
                static_cast<firmware::SettingsPersistenceWorker*>(context)
                    ->NotifyLitchcraftFramePriority(
                        bytes, size, ownerEpoch);
            },
            [](void* context, uint8_t* out, const size_t capacity,
               size_t& outSize) {
                return static_cast<firmware::SettingsPersistenceWorker*>(context)
                    ->PeekLitchcraftFrameResponse(out, capacity, outSize);
            },
            [](void* context) {
                static_cast<firmware::SettingsPersistenceWorker*>(context)
                    ->AcceptLitchcraftFrameResponse();
            },
            [](void* context, const uint16_t request,
               const uint32_t ownerEpoch) {
                return static_cast<firmware::SettingsPersistenceWorker*>(context)
                    ->ReplayLitchcraftFrameResponse(request, ownerEpoch);
            },
            &sys.settingsPersistence);
#if defined(LITCH_PANIC_DIAGNOSTIC)
        sys.ble.SetLiveIngressDiagnosticCallback([](void* context) {
            const auto live = static_cast<firmware::SettingsPersistenceWorker*>(context)
                ->GetLiveIngressDiagnostics();
            logger::Warn("store_ingress_live: available=%u owner_epoch=%lu response_bytes=%u prefetch=%u ingress_pending=%u mode_save_pending=%u storage_active=%u handle=%u/%u age_ms=%lu",
                unsigned(live.available), static_cast<unsigned long>(live.ownerEpoch),
                unsigned(live.responseSize), unsigned(live.prefetchCount),
                unsigned(live.ingressPending), unsigned(live.modeSavePending),
                unsigned(live.storageActive), unsigned(live.handlePhase), unsigned(live.handleOperation),
                static_cast<unsigned long>(live.handleAgeMs));
        });
#endif
    }
#endif
#if defined(LITCH_ESP32S3)
    // Store ownership must not depend on worker startup. An unavailable
    // worker leaves the canvas visibly blank and rejects old artwork commands
    // until Store recovery is possible.
    if (sys.settingsPersistence.IsStarted()) {
        sys.gamePersistence.Litchcraft().AttachFrameStream(
            &sys.settingsPersistence.LitchcraftFrameStream(),
            [](void* context, const bool playing, const uint32_t session,
               const bool active) {
                auto& system = *static_cast<FirmwareSystem*>(context);
                // Session/mode projection follows release of any admitted
                // mutation; this wake must not wait on the worker FS mutex.
                system.settingsPersistence.NotifyLitchcraftFramePlayback(playing);
            },
            &sys);
    } else {
        sys.gamePersistence.Litchcraft().AttachFrameStream(
            &sys.settingsPersistence.LitchcraftFrameStream());
    }
#endif
    // Startup restores in RAM. A later user-owned mutation admits persistence;
    // native BLE restoration must not race a boot-initiated file write.
    ConfigureLitchcraftPersistence(sys);
    const uint32_t settingsWorkerMs = phaseElapsed.ElapsedMs();
    runtime::FeedProgressWatchdog();
    phaseElapsed.Reset();
    RefreshLedPowerSourceStatus(sys, fxe::Millis(), false);
    const uint32_t sourceMs = phaseElapsed.ElapsedMs();
    if (sys.startupHardwareTestRequested) {
        sys.hardwareTest.Enter(sys.runtimeHardware.detectedRevision, fxe::Millis());
    }
    sys.bootDisplay.Begin(modules::ReadBootDisplayChoice(sys.settings),
        hardwareProfile.revision == hardware::BoardRevision::V3_3S3 && !sys.factoryBootOnly,
        {sys.gamePersistence.Litchcraft().ModeGeneration(),
         sys.gamePersistence.Litchcraft().CanvasGeneration(), bootResumeOccupancy, bootResumableGame});
    sys.bootDisplay.ObserveBoard(sys.startupBoardSnapshotObserved &&
        !sys.gamePersistence.Litchcraft().IsInitialStoreModePending(),
        sys.stateMachine.GetPhysicalOccupancyMask(), !sys.gamePersistence.Litchcraft().IsActive());
    if (hardwareProfile.revision == hardware::BoardRevision::V3_3S3) {
        const auto firstSemanticRender = RenderLeds(true);
        if (firstSemanticRender == firmware::LedRenderAttemptResult::Denied) {
            sys.ledRuntimeAdmission.NoteRenderOutcome(
                firstSemanticRender, fxe::Millis());
        }
#if defined(LITCH_ESP32S3)
        // Restore/Hall and storage bindings are ready. Let the existing worker
        // recover saved mode while BLE starts; unresolved mode stays black and
        // the independent renderer preserves accepted-transfer animation timing.
        if (sys.ledAnimation.IsStarted()) {
            sys.settingsPersistence.NotifyLitchcraftFrameRuntimeReady();
        }
#endif
    }
    runtime::NoteProgress(0U, runtime::ProgressStage::Ble);
    sys.ble.Begin(sys.settings);
    sys.sleep.SetPreSleepCallback(&StopBleBeforeSleep, &sys.ble);
#ifdef LITCH_TRACE
    auto& recorder = GetTraceRecorder();
    sys.gamePersistence.SetTraceRecorder(&recorder);
    const auto settingsSnapshot = GetTraceSettingsSnapshot(*GetSystemContext().settings);

    const char* boardId =
#ifdef LITCH_ESP32S3
        "esp32s3";
#else
        "esp32c3";
#endif

    recorder.Start("litch", boardId, sys.rngSeed, settingsSnapshot);
    recorder.RecordSettings(settingsSnapshot);
    recorder.RecordCheckpointState(sys.stateMachine.DescribeInteractionState(),
                                   sys.stateMachine.IsPlacementModeActive(),
                                   sys.stateMachine.IsCpuThinkingActive(),
                                   sys.stateMachine.HasHumanIllegal(),
                                   sys.stateMachine.HasCpuIllegal());
    const auto variantState =
        sys.gamePersistence.GetGameVariantStateSnapshot();
    recorder.RecordCheckpointInteraction(
        sys.stateMachine.GetChessInteractionModel(),
        sys.stateMachine.GetInteractionInvariantStatus());
    recorder.RecordCheckpointVariant(
        variantState,
        runtime::ComputeChessInteractionModelHash(
            sys.stateMachine.GetChessInteractionModel()));
#endif
    // Publish any mode/health changes made during BLE initialization without
    // restarting the already-presented boot episode or its animation clock.
    const auto servicesRender = RenderLeds(true);
    if (servicesRender == firmware::LedRenderAttemptResult::Denied) {
        sys.ledRuntimeAdmission.NoteRenderOutcome(
            servicesRender, fxe::Millis());
    }
#if defined(LITCH_HW_VARIANT_S3)
    firmware::RecordUsbBootServicesReady();
#endif
    logger::Trace(
        "boot: services_ms console=%lu ota_worker=%lu ota_check=%lu ambient=%lu settings_worker=%lu source=%lu total=%lu ready_return=%lu",
        static_cast<unsigned long>(consoleMs),
        static_cast<unsigned long>(otaWorkerMs),
        static_cast<unsigned long>(otaCheckMs),
        static_cast<unsigned long>(ambientMs),
        static_cast<unsigned long>(settingsWorkerMs),
        static_cast<unsigned long>(sourceMs),
        static_cast<unsigned long>(servicesElapsed.ElapsedMs()),
        static_cast<unsigned long>(fxe::Millis()));

}

} // namespace firmware_internal

#endif
