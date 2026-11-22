#ifdef ARDUINO

#include <Arduino.h>

#include <cstring>

#include "firmware/firmware_app_internal.hpp"
#include "firmware/board_storage_recovery.hpp"
#include "firmware/firmware_recovery_internal.hpp"
#include "firmware/local_control_requests.hpp"
#include "firmware/native_usb_reset_recovery.hpp"
#include "firmware/litchcraft_persistence_runtime.hpp"
#include "firmware/ota_boot_health.hpp"
#include "firmware/ota_platform_adapter.hpp"
#include "firmware/firmware_tasks_internal.hpp"
#include "firmware/sleep_entry_orchestration.hpp"
#include "logger.hpp"
#include "modules/move_confirm_setting.hpp"
#include "modules/ota_runtime_acceptance.hpp"
#include "modules/volatile_persistence_session.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "runtime/progress_watchdog.hpp"
#include "runtime/pending_undo_command_handoff.hpp"

namespace firmware_internal {
namespace {

bool WakeButtonsReleasedBeforeSleep(void* context) {
    return static_cast<FirmwareSystem*>(context)->sleep.CanBeginSleepTeardown();
}

bool PrepareWakeSourceBeforeSleep(void* context) {
    return static_cast<FirmwareSystem*>(context)->sleep.PrepareWakeSourceForSleep();
}

bool ClearLedsBeforeSleep(void* context) {
    auto& sys = *static_cast<FirmwareSystem*>(context);
    return sys.ledAnimation.StartAttempted() ? sys.ledAnimation.Quiesce()
                                           : ClearLedOutput(sys);
}

void WaitBeforeSleepStep(void*, const uint32_t milliseconds) {
    delay(milliseconds);
}

void ShutdownAmbientBeforeSleep(void* context) {
    if (hardware::HW_HAS_LIGHT_SENSORS) {
        static_cast<FirmwareSystem*>(context)->ambientLights.ShutDown();
    }
}

void PreparePowerRailBeforeSleep(void* context) {
    static_cast<FirmwareSystem*>(context)->powerRail.PrepareForSleep();
}

bool EnterSleep(void* context) {
    return static_cast<FirmwareSystem*>(context)->sleep.EnterSleepNow();
}

void RestartAfterSleepEntryReturn(void*, const bool enteredSleep) {
    logger::Error(
        "sleep: entry returned unexpectedly (entered=%u); restarting",
        static_cast<unsigned>(enteredSleep ? 1U : 0U));
    delay(20);
    ESP.restart();
}

const char* SleepPreparationFailureName(
    const firmware::SleepPreparationFailure failure) {
    switch (failure) {
        case firmware::SleepPreparationFailure::GamePersistence:
            return "settings or game snapshot unavailable";
        case firmware::SleepPreparationFailure::SettingsPersistence:
            return "startup checks or settings/artwork save failed or timed out";
        case firmware::SleepPreparationFailure::WakeButtonsHeld:
            return "wake button held";
        case firmware::SleepPreparationFailure::WakeSourceUnavailable:
            return "wake source unavailable";
        case firmware::SleepPreparationFailure::PolicyRejected:
            return "transition policy rejected preflight";
        case firmware::SleepPreparationFailure::None:
        default:
            return "none";
    }
}

// Boot restores the game on the same task. Keep normal runtime's stack frame
// out of startup so its event and renderer scratch space is only reserved here.
[[gnu::noinline]] void LoopNormalRuntime(FirmwareSystem& sys,
                                         runtime::SystemContext& ctx) {
#if defined(LITCH_ESP32S3)
    // V3.3 already admits recovery after binding the independent renderer,
    // ahead of BLE. This idempotent notification retains the normal runtime
    // handoff for other profiles without restarting recovery or its clock.
    sys.settingsPersistence.NotifyLitchcraftFrameRuntimeReady();
    // Root, mode and first-frame recovery must not wait for verification.
    // Only optional missing-summary repair waits until normal boot is accepted.
    sys.settingsPersistence.SetLitchcraftFrameSummaryMaintenanceReady(
        sys.runtimeBootHealth.commitFinished && sys.sleep.GetState() == firmware::SleepState::Awake &&
        !sys.rebootPersistencePending);
#endif

    if (modules::UpdateRuntimePersistenceSession(
            sys.settings, sys.controlMutation, sys.persistenceDegradation,
            sys.settingsPersistence.IsStarted())) {
        logger::Warn("settings_io: saving unavailable; continuing in RAM until restart or recovery");
    }

    if (sys.durableSettingsSnapshotObserved.exchange(
            false, std::memory_order_acq_rel) &&
        modules::NoteVerifiedPersistenceSave(
            sys.persistenceDegradation)) {
        logger::Info(
            "settings_io: quarantined game record repaired by verified save");
    }

    if (PollPendingReboot(sys)) {
        // The flash worker yields between bounded accesses until another LED
        // frame completes. Keep those frames live while reboot owns input.
        runtime::NoteProgress(0U, runtime::ProgressStage::Render);
        const auto result = RenderLeds();
        sys.ledRuntimeAdmission.NoteRenderOutcome(result, fxe::Millis());
        runtime::FeedProgressWatchdog();
        delay(result == firmware::LedRenderAttemptResult::Denied ? 1U : 20U);
        return;
    }

    static uint32_t lastIncidentContextMs = 0U;
    const uint32_t contextNowMs = fxe::Millis();
    if (contextNowMs - lastIncidentContextMs >= 250U) {
        runtime::IncidentContext context{};
        context.uptimeMs = contextNowMs;
        context.expectedOccupancy = sys.stateMachine.GetExpectedOccupancyMask();
        context.observedOccupancy = sys.stateMachine.GetPhysicalOccupancyMask();
        context.interactionPhase = static_cast<uint8_t>(sys.stateMachine.GetChessInteractionModel().phase.tag);
        context.variant = static_cast<uint8_t>(sys.gamePersistence.GetGameVariant());
        context.healthCauses = sys.runtimeHealth.Snapshot().causes;
        context.storageError = static_cast<uint8_t>(sys.settings.GetLastError());
        context.flags = (sys.ble.IsConnected() ? 1U : 0U) | (sys.ble.IsOtaInProgress() ? 2U : 0U) |
                        (sys.settings.IsDisabled() ? 4U : 0U) | (sys.safeModeActive ? 8U : 0U);
        runtime::UpdateIncidentContext(context);
        lastIncidentContextMs = contextNowMs;
    }

    const bool otaHealthCommitFinished = PollRuntimeBootHealth(sys);

    PollLedPowerSourceStatus(sys, fxe::Millis());

    static PeriodicTaskCadence ioCadence;
    static PeriodicTaskCadence engineCadence;
    static PeriodicTaskCadence settingsCadence;
    static PeriodicTaskCadence bleCadence;
    static PeriodicTaskCadence sensorCadence;
    static PeriodicTaskCadence ledCadence;
#ifdef LITCH_TRACE
    static PeriodicTaskCadence traceCadence;
#endif

    static bool cadenceConfigured = false;
    ConfigureCadencesOnce(cadenceConfigured,
                          ioCadence,
                          engineCadence,
                          settingsCadence,
                          bleCadence,
                          sensorCadence,
                          ledCadence
#ifdef LITCH_TRACE
                          ,
                          traceCadence
#endif
    );

    InitCadences(ioCadence,
                 engineCadence,
                 settingsCadence,
                 bleCadence,
                 sensorCadence,
                 ledCadence
#ifdef LITCH_TRACE
                 ,
                 traceCadence
#endif
    );

    runtime::NoteProgress(0U, runtime::ProgressStage::Controls);
    sys.ble.SetLitchcraftRuntimeEligible(!sys.hardwareTest.IsActive() &&
        !sys.comptest.IsActive() && !sys.amptest.IsActive() && !sys.amptestVerify.IsActive() &&
        !sys.quickSettingsOverlayActive && !sys.menuOverlay.IsActive() &&
        sys.sleep.GetState()==firmware::SleepState::Awake &&
        sys.runtimeHealth.Snapshot().advice.boardInputAllowed);
    PublishBoardStorageRecovery(sys);
    sys.ble.DrainEventQueue();
    if (modules::OWNER_PAIRING_ENABLED) {
        if (sys.sleep.ShouldIgnoreActivity(firmware::SleepTransitionCause::ButtonPress))
            sys.boardOwnerPairing.ReseedPhysicalInput();
        sys.boardOwnerPersistence.Update();
        sys.ble.UpdateOwnerPairing();
    }
    sys.gamePersistence.UpdateLitchcraft();
    if (sys.gamePersistence.Litchcraft().ConsumeActivity()) sys.sleep.NoteHallSensorActivity();
    sys.controlMutation.Update();
    sys.currentGameSync.ServiceMain();
    DrainTrackedLocalControlCompletions(sys);
    UpdateHardwareTestRadio(sys, fxe::Millis());
    // BLE settings writes are applied while draining. Re-evaluate CC policy
    // before the settings cadence can persist a changed threshold triple.
    PollLedPowerSourceStatus(sys, fxe::Millis());

    if (ioCadence.IsDue()) {
        MaybeEnableConsoleOutput();
        runtime::NoteProgress(0U, runtime::ProgressStage::Console);
        ctx.console->Update();
        if (sys.rebootPersistencePending) return;
        // Console settings operations can change CC thresholds. Re-evaluate
        // only after the operation has completed and the new values are live.
        PollLedPowerSourceStatus(sys, fxe::Millis());
        ioCadence.RescheduleFromNow();
    }

    sys.comptest.Update();

    if (settingsCadence.IsDue()) {
        runtime::NoteProgress(0U, runtime::ProgressStage::Settings);
        ctx.settings->Update();
        settingsCadence.RescheduleFromNow();
    }

    if (bleCadence.IsDue()) {
        runtime::NoteProgress(0U, runtime::ProgressStage::Ble);
        if (modules::OWNER_PAIRING_ENABLED &&
            sys.sleep.ShouldIgnoreActivity(firmware::SleepTransitionCause::ButtonPress))
            sys.boardOwnerPairing.ReseedPhysicalInput();
        sys.ble.Update(sys.settings);
        sys.telemetry.OnBleCompletionGap(bleCadence.sinceLast.ElapsedMs());
        bleCadence.RescheduleFromNow();
    }

    static bool stateLogInit = false;
    static const char* lastState = "Idle";
    if (!stateLogInit) {
        lastState = sys.stateMachine.DescribeInteractionState();
        stateLogInit = true;
    }

    const auto logIfStateChanged = [&](const uint8_t preFromSq,
                                       const uint8_t preToSq,
                                       const bool preCapValid,
                                       const uint8_t preCapSq) {
        const char* const cur = sys.stateMachine.DescribeInteractionState();
        if (std::strcmp(cur, lastState) != 0) {
            if (std::strcmp(lastState, "Settling") == 0 &&
                std::strcmp(cur, "Idle") == 0) {
                LogStateTransitionInternalWithSquares(sys, lastState, cur, preFromSq, preToSq, preCapValid, preCapSq);
            } else {
                LogStateTransitionInternal(sys, lastState, cur);
            }
            lastState = cur;
        }
    };

    if (engineCadence.IsDue()) {
        runtime::NoteProgress(0U, runtime::ProgressStage::Engine);
        if (!sys.hardwareTest.IsActive() && !sys.comptest.IsActive()) {
            GetSystem().gamePersistence.Update();
        }
        uint8_t capSq = 255;
        const bool capValid = sys.stateMachine.TryGetPendingCaptureSquare(capSq);
        const runtime::PhysicalTransitionPlan& plan =
            sys.stateMachine.GetChessInteractionModel().phase.plan;
        logIfStateChanged(plan.moveFrom, plan.moveTo, capValid, capSq);
        engineCadence.RescheduleFromNow();
    }

    if (sensorCadence.IsDue()) {
        runtime::NoteProgress(0U, runtime::ProgressStage::Sensors);
        static bool motionUpdateFailed = false;
        if (sys.motionWake.IsSupported()) {
            const uint32_t motionEventCountBefore =
                sys.motionWake.GetDiagnostics().eventCount;
            const bool motionUpdateOk = sys.motionWake.Update(sys.settings);
            if (!motionUpdateOk && !motionUpdateFailed) {
                logger::Warn("motion: runtime update failed");
            } else if (motionUpdateOk && motionUpdateFailed) {
                logger::Info("motion: runtime update recovered");
            }
            motionUpdateFailed = !motionUpdateOk;
            if (sys.motionWake.GetDiagnostics().eventCount !=
                motionEventCountBefore) {
                (void)sys.sleep.NoteValidatedMotionOrTapActivity();
            }
        }
        auto& litchcraft = sys.gamePersistence.Litchcraft();
        const bool sampleLitchcraftShake =
            !sys.hardwareTest.IsActive() && !sys.amptest.IsActive() &&
            !sys.amptestVerify.IsActive() && !sys.comptest.IsActive() && litchcraft.IsActive() &&
            litchcraft.IsShakeEraseSupported() &&
            hardware::HasFourLedSquares(
                sys.runtimeHardware.resolved.revision) &&
            sys.motionWake.IsAvailable();
        (void)sys.motionWake.SetInteractiveSampling(
            firmware::InteractiveSamplingOwner::Litchcraft,
            sampleLitchcraftShake);
        const uint32_t motionNowMs = fxe::Millis();
        if (sampleLitchcraftShake &&
            motionNowMs - sys.litchcraftShakeSampleMs >=
                modules::LitchcraftShakeDetector::SAMPLE_PERIOD_MS) {
            const bool admitted = BeginPhysicalLitchcraftPresentation(sys,
                modules::LitchcraftSession::PresentationAccess::Frame);
            modules::GamePersistenceCoordinator::LitchcraftPresentationGuard presentation(
                admitted ? &sys.gamePersistence : nullptr);
            if (admitted) {
            sys.litchcraftShakeSampleMs = motionNowMs;
            hardware::Lis2dw12Sample sample;
            if (sys.motionWake.GetSensor().ReadSample(sample) &&
                !sys.sleep.ShouldIgnoreActivity(firmware::SleepTransitionCause::ValidatedMotionOrTap)) {
                litchcraft.ObserveMotionSample(
                    sample.milliGx, sample.milliGy, sample.milliGz);
            }
            }
        } else if (!sampleLitchcraftShake) {
            sys.litchcraftShakeSampleMs = motionNowMs;
        }
        LogExpanderInterruptsIfAny();
        if (sys.hardwareTest.IsActive()) {
            const uint32_t hardwareTestNowMs = fxe::Millis();
            if (!sys.hardwareTestMotionInitialized ||
                sys.hardwareTestMotionRunId !=
                    sys.hardwareTest.GetRunId()) {
                sys.hardwareTestMotionInitialized = true;
                sys.hardwareTestMotionRunId =
                    sys.hardwareTest.GetRunId();
                sys.hardwareTestMotionBaseline =
                    sys.motionWake.GetDiagnostics().eventCount;
            }
            if (hardware::HasFourLedSquares(sys.runtimeHardware.resolved.revision))
                sys.hardwareTest.ReportMotionPresence(sys.motionWake.CheckFactoryPresence(!motionUpdateFailed), hardwareTestNowMs);
            sys.hardwareTest.Tick(hardwareTestNowMs);
            if (!sys.hardwareTest.IsActive()) ReleaseHardwareTestInput(sys);
            PollButtonsAndDispatch();
            if (sys.runtimeHardware.resolved.hasLightSensors) {
                if (sys.ambientLights.UpdateAttempt().attemptCompleted) {
                    sys.hardwareTest.ReportLightSensorLinks(
                        sys.ambientLights.Sensor0Ok(),
                        sys.ambientLights.Sensor1Ok(),
                        hardwareTestNowMs);
                    sys.hardwareTest.ObserveLightSample(
                        {static_cast<uint8_t>((sys.ambientLights.Sensor0Ok() ? 1U : 0U) |
                                              (sys.ambientLights.Sensor1Ok() ? 2U : 0U)),
                         {sys.ambientLights.GetLastLux0(), sys.ambientLights.GetLastLux1()}},
                        hardwareTestNowMs);
                    auto& loopCtx = GetSystemContext();
                    if (loopCtx.console) {
                        loopCtx.console->PrintLn("hwtest: lux0=%lu lux1=%lu valid=%u lux=%0.2f",
                                                 static_cast<unsigned long>(sys.ambientLights.GetLastLux0()),
                                                 static_cast<unsigned long>(sys.ambientLights.GetLastLux1()),
                                                 static_cast<unsigned>(sys.ambientLights.GetLastValidCount()),
                                                 static_cast<double>(sys.ambientLights.GetLastLux()));
                    }
                }
            }
            sys.hardwareTest.UpdateSensors(sys.boardInput, hardwareTestNowMs);
            sys.hardwareTest.ReportExpanderLinks(sys.inputScan.LastRespondingExpanderMask(), hardwareTestNowMs);
            if (sys.motionWake.GetDiagnostics().eventCount >
                sys.hardwareTestMotionBaseline) {
                sys.hardwareTest.ObserveMotionEvent(hardwareTestNowMs);
            }
            UpdateHardwareTestPersistence(sys);
        } else if (sys.comptest.IsActive()) {
            PollButtonsAndDispatch();
        } else if (sys.amptest.IsActive()) {
            sys.hardwareTestMotionInitialized = false;
            PollButtonsAndDispatch();
        } else if (sys.amptestVerify.IsActive()) {
            sys.hardwareTestMotionInitialized = false;
            // Keep the board in the staged verification state without advancing
            // gameplay or reacting to button input.
        } else if (sys.gamePersistence.Litchcraft().IsActive()) {
            PollButtonsAndDispatch();
            PollSensorsAndDispatch();
            sys.gamePersistence.UpdateLitchcraft();
        } else {
            sys.hardwareTestMotionInitialized = false;
            modules::move_confirm_setting::Apply(sys.stateMachine, sys.settings);

            uint8_t preCapSq = 255;
            bool preCapValid = sys.stateMachine.TryGetPendingCaptureSquare(preCapSq);
            const runtime::PhysicalTransitionPlan* prePlan =
                &sys.stateMachine.GetChessInteractionModel().phase.plan;
            uint8_t preFromSq = prePlan->moveFrom;
            uint8_t preToSq = prePlan->moveTo;
            runtime::InteractionDispatcher interactionDispatcher(
                sys.stateMachine,
                sys.gamePersistence,
                sys.appEventQueue);
            interactionDispatcher.DispatchTick(fxe::Millis());
            logIfStateChanged(preFromSq, preToSq, preCapValid, preCapSq);

            PollButtonsAndDispatch();

            // Cancel-undo has a render-only forward-position handoff. Put that
            // frame on the LEDs before synchronous engine restoration,
            // then consume every undo command before accepting a newer scan.
            const bool undoCommandReady =
                runtime::RunPendingUndoCommandHandoff(
                {sys.gamePersistence.HasPendingUndoCommand(),
                 sys.stateMachine.GetRenderState().moveTransition.kind,
                 sys.ble.IsOtaInProgress()},
                [&]() {
                    const auto result = RenderLeds(true);
                    sys.ledRuntimeAdmission.NoteRenderOutcome(
                        result, fxe::Millis());
                    ledCadence.RescheduleFromNow();
                    return result ==
                           firmware::LedRenderAttemptResult::Completed;
                },
                [&]() { sys.gamePersistence.Update(); });

            if (undoCommandReady) {
                preCapSq = 255;
                preCapValid =
                    sys.stateMachine.TryGetPendingCaptureSquare(preCapSq);
                prePlan =
                    &sys.stateMachine.GetChessInteractionModel().phase.plan;
                preFromSq = prePlan->moveFrom;
                preToSq = prePlan->moveTo;
                PollSensorsAndDispatch();
                logIfStateChanged(preFromSq, preToSq, preCapValid, preCapSq);

                if (sys.stateMachine.ShouldProcessCommittedMoveImmediately()) {
                    sys.gamePersistence.Update();
                }

                sys.gamePersistence.UpdateLegalDestinationsMask();
            }
        }
        sys.telemetry.OnInputCompletionGap(sensorCadence.sinceLast.ElapsedMs());
        sensorCadence.RescheduleFromNow();
    }

    const bool cpuThinking = sys.stateMachine.IsCpuThinkingActive();
    const bool otaInProgress = sys.ble.IsOtaInProgress();

    if (otaInProgress) {
        ioCadence.periodMs = 1;
        bleCadence.periodMs = 5;
        sensorCadence.periodMs = 20;
        engineCadence.periodMs = 50;
        settingsCadence.periodMs = 50;
    } else {
        ioCadence.periodMs = cpuThinking ? 10 : 5;
        bleCadence.periodMs = 20;
        sensorCadence.periodMs = cpuThinking ? 10 : 5;
        engineCadence.periodMs = 20;
        settingsCadence.periodMs = 20;
    }

    static constexpr uint32_t LED_CADENCE_NORMAL_MS = 20U;
    const auto otaPresentation = sys.ble.GetOtaLedPresentation();
    const bool bulkTransferActive = sys.ble.IsBulkTransferActive()
#if defined(LITCH_ESP32S3)
        || sys.settingsPersistence.IsLitchcraftBulkTransferActive()
#endif
        ;
    auto& artwork = sys.gamePersistence.Litchcraft();
    if (artwork.IsPlaybackTransferSuspended() != bulkTransferActive &&
        sys.gamePersistence.TryBeginLitchcraftPresentation(
            modules::LitchcraftSession::PresentationAccess::Control)) {
        // The existing admission consumes completed output and publishes the
        // changed context before reopening Show, including between deadlines.
        modules::GamePersistenceCoordinator::LitchcraftPresentationGuard admission(sys.gamePersistence);
        artwork.SetBulkTransferActive(bulkTransferActive);
    }
#if defined(LITCH_ESP32S3)
    sys.settingsPersistence.SetLitchcraftPlaybackSuspended(bulkTransferActive);
#endif
    ledCadence.periodMs = modules::RuntimeLedRefreshMs(otaPresentation,
        bulkTransferActive, LED_CADENCE_NORMAL_MS, otaInProgress);
    const uint32_t renderNowMs = fxe::Millis();
    if (sys.ledRuntimeAdmission.Update(otaPresentation.active,
                                       ledCadence.IsDue(),
                                       renderNowMs)) {
        runtime::NoteProgress(0U, runtime::ProgressStage::Render);
        // Include synchronous rendering in the period; overruns remain due.
        ledCadence.RescheduleFromNow();
        const auto renderResult = RenderLeds();
        sys.ledRuntimeAdmission.NoteRenderOutcome(renderResult, fxe::Millis());
    }
    PublishHomeStateIfNeeded(sys);
#ifdef LITCH_TRACE
    if (traceCadence.IsDue()) {
        auto& recorder = GetTraceRecorder();
        while (traceCadence.IsDue()) {
            recorder.RecordTick();
            traceCadence.RescheduleFromNow();
        }
    }
#endif

    const uint32_t sleepMs = ComputeNextWakeMs(ioCadence,
                                               engineCadence,
                                               settingsCadence,
                                               bleCadence,
                                               sensorCadence,
                                               ledCadence
#ifdef LITCH_TRACE
                                               ,
                                               traceCadence
#endif
    );

    const bool sleepSuppressed =
        firmware::IsSleepSuppressedByHigherPriority(
            firmware::SleepHigherPriorityState{
                .recovery = sys.safeModeActive,
                .runtimeHealthPaused =
                    runtime::IsRuntimeHealthPlayPaused(
                        sys.runtimeHealth.Snapshot()),
                .otaTransfer = otaInProgress,
                .otaHealthConfirmationPending = !otaHealthCommitFinished,
                .hardwareTest = sys.hardwareTest.IsActive(),
                .ampTest = sys.amptest.IsActive() || sys.comptest.IsActive(),
                .ampTestVerify = sys.amptestVerify.IsActive(),
            },
            sys.sleep.GetState() == firmware::SleepState::Awake
                ? firmware::SleepRequestOrigin::None : sys.sleep.GetRequestOrigin());
    sys.sleep.Update(
        sys.settings,
        fxe::Millis(),
        sleepSuppressed ? firmware::SleepSuppression::HigherPriority
                        : firmware::SleepSuppression::Allow);

    runtime::NoteProgress(0U, runtime::ProgressStage::Sleep);
    if (sys.sleep.GetState() == firmware::SleepState::Preparing) {
        auto& sleepSys = sys;
        const firmware::SleepLifecycleResult lifecycle =
            firmware::RunSleepLifecycleStep(
                sleepSys.sleep,
                firmware::SleepPreparationHooks{
                    &sleepSys,
                    &BeginPersistenceBeforeSleep,
                    &PollPersistenceBeforeSleep,
                    &WakeButtonsReleasedBeforeSleep,
                    &PrepareWakeSourceBeforeSleep,
                },
                firmware::SleepEntryHooks{
                    &sleepSys,
                    &ClearLedsBeforeSleep,
                    &WaitBeforeSleepStep,
                    &ShutdownAmbientBeforeSleep,
                    &PreparePowerRailBeforeSleep,
                    &EnterSleep,
                    &RestartAfterSleepEntryReturn,
                });
        if (lifecycle == firmware::SleepLifecycleResult::PreparationFailed) {
            logger::Warn(
                "sleep: aborted before teardown (%s)",
                SleepPreparationFailureName(
                    sleepSys.sleep.GetLastPreparationFailure()));
        } else if (lifecycle ==
                   firmware::SleepLifecycleResult::AuthorizationUnavailable) {
            logger::Error(
                "sleep: transition authorization was not available");
            return;
        } else if (lifecycle ==
                   firmware::SleepLifecycleResult::EntryHooksUnavailable) {
            logger::Error("sleep: entry orchestration was not configured");
            return;
        } else if (lifecycle ==
                   firmware::SleepLifecycleResult::RestartReturned) {
            logger::Error("sleep: restart returned unexpectedly");
            while (true) {
                delay(1000);
            }
        }
    }

    SleepUntilNextWork(sleepMs);
}

} // namespace

void LoopApp() {
    PollNativeUsbResetRecovery();
    GetSystem().telemetry.OnLoop();
    if (!GetSystem().safeModeActive && !GetSystem().startupLeds.IsReady()) {
        AdvanceStartup();
        delay(1);
        return;
    }
    EnsureConsoleInitialized();

    auto& ctx = GetSystemContext();

    auto& sys = GetSystem();
    sys.stateMachine.RetireExpiredAllPiecesFlash(fxe::Millis());
    ConsumeLedPresentation(sys);
    if (sys.safeModeActive) {
        // Recovery must not enter uninitialized gameplay/persistence owners.
        // The health owner may reject a failed trial, never accept recovery.
        (void)sys.sleep.NoteHigherPriorityState();
        (void)PollRuntimeBootHealth(sys);
        RunSafeModeRecovery(sys);
        return;
    }
    if (sys.factoryBootOnly && !sys.safeModeActive) {
        LoopFactoryRuntime(sys);
        return;
    }
    LoopNormalRuntime(sys, ctx);
}

} // namespace firmware_internal

#endif
