#ifdef ARDUINO
#include "firmware/firmware_tasks_internal.hpp"
#include "firmware/water_slosh_runtime.hpp"
#include "modules/control_led_feedback_projection.hpp"
#include "modules/beginner_mode_policy.hpp"
#include "modules/quick_settings_orchestration.hpp"
#include "modules/quick_settings_hold_preview.hpp"
#include "modules/orientation.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"
#include "logger.hpp"

namespace firmware_internal {
namespace {
void WakeArtwork(void* context, const bool playing, const uint32_t session,
    const bool, const bool) {
#if defined(LITCH_ESP32S3)
    auto& worker = *static_cast<firmware::SettingsPersistenceWorker*>(context);
    // Main owns session/mode publication. Autonomous paging only raises the
    // existing atomic wake; it never waits for the storage worker's FS mutex.
    worker.NotifyLitchcraftFramePlayback(playing);
#else
    (void)context; (void)playing;
#endif
}
void CaptureTest(FirmwareSystem& sys, firmware::ManufacturingLedPresentation& test) {
    auto& in = test.inputs;
    const uint32_t now = fxe::Millis();
    in.revision = sys.runtimeHardware.resolved.revision;
    in.step = sys.hardwareTest.GetStep();
    in.stepElapsedMs = sys.hardwareTest.GetStepElapsedMs(now);
    in.runElapsedMs = sys.hardwareTest.GetRunElapsedMs(now);
    in.ledChannel = sys.hardwareTest.GetLedChannel();
    in.ledScanElapsedMs = sys.hardwareTest.GetLedScanElapsedMs(now);
    in.automaticVerifier = sys.hardwareTest.IsAutomaticVerifier();
    in.operatorHoldProgress = sys.hardwareTest.GetOperatorHoldProgress();
    in.progress = sys.hardwareTest.GetProgress();
    in.verifierFeedback = sys.hardwareTest.GetVerifierFeedback();
    in.verifierFeedbackElapsedMs = sys.hardwareTest.GetVerifierFeedbackElapsedMs(now);
    if (in.step == firmware::HardwareTestMode::Step::HallSensors)
        (void)sys.boardInput.TryReadSnapshot(in.occupiedMask);
    test.brightnessScale256 = sys.hardwareTest.GetBrightnessScale256();
}
void InitializeAnimation(FirmwareSystem& sys, const firmware::GameLedPresentation* game) {
    modules::litchcraft_presentation::StreamPort port;
#if defined(LITCH_ESP32S3)
    port.stream = &sys.settingsPersistence.LitchcraftFrameStream();
#endif
    port.wake = &WakeArtwork;
    port.context = &sys.settingsPersistence;
    port.confirmedOutput = true;
    sys.ledAnimation.Bind(sys.Leds(), sys.runtimeHardware.resolved, port);
    sys.ledAnimation.InitializeBoot(sys.bootDisplay);
    sys.ledAnimation.InitializeBrightness(sys.ambientLux, sys.sourceOutputLimit);
    if (game != nullptr) sys.ledAnimation.InitializeGameBackground(*game);
    sys.stateMachine.SetHintPresentationObserver([](void* context, const bool retire) {
        auto& system = *static_cast<FirmwareSystem*>(context);
        const auto shown = system.ledAnimation.ObserveHint(retire);
        const auto& state = system.stateMachine;
        const auto& render = state.GetRenderState();
        return shown.completed && shown.visibleHintValid &&
            shown.positionRevision == state.GetChessInteractionModel().acknowledged.revision &&
            shown.hintEpoch == render.hintPresentationEpoch && render.hintDisplayRequested
                ? shown.visibleHint : runtime::HintPresentationTuple{};
    }, [](void*) { (void)RenderLeds(); }, &sys);
    sys.gamePersistence.Litchcraft().SetIndependentPresentation(true);
    if (port.stream != nullptr) port.stream->SetConfirmedPresentation(true);
    if (!sys.ledAnimation.Start())
        logger::Error("led_animation: task start failed; output remains held");
    else ConfigureLitchcraftPresentationAdmission(sys);
}
}

void CancelBootPresentation(FirmwareSystem& sys) {
    // A cosmetic edit after confirmed startup must not reopen its receipt gate:
    // Coaching would hide and restart the shown Hint until the next LED frame.
    if (StartupPresentationComplete(sys)) return;
    sys.bootDisplay.Cancel();
    ++sys.ledBootCancelEpoch;
    sys.ledAnimation.CancelBoot();
}

void ConsumeLedPresentation(FirmwareSystem& sys) {
    if (!sys.ledAnimation.IsStarted()) return;
    const auto shown = sys.ledAnimation.Receipt();
    sys.ledPowerStats = shown.power;
    sys.ledPowerLimitMa = shown.currentLimitMa;
    if (!shown.completed) return;
    if (shown.bootCancelEpoch == sys.ledBootCancelEpoch)
        sys.bootDisplay.SynchronizePresentation(shown.boot);
    sys.gamePersistence.Litchcraft().ApplyPresentationFeedback(shown.artwork);
    if (!sys.bootFirstNonzeroRenderLogged) RecordBootRenderReturn(sys,
        fxe::ElapsedTime(shown.frameStartedMs), shown.brightnessScale256);
}

bool StartupPresentationComplete(const FirmwareSystem& sys) {
    const auto shown = sys.ledAnimation.Receipt();
    return shown.completed && shown.bootCancelEpoch == sys.ledBootCancelEpoch &&
        shown.startupComplete;
}

firmware::LedRenderAttemptResult RenderLeds(const bool waitForCompletion) {
    auto& sys = GetSystem();
    ConsumeLedPresentation(sys);
    const uint32_t nowMs = fxe::Millis();
    auto& bootArtwork = sys.gamePersistence.Litchcraft();
    bool unavailable = !sys.settingsPersistence.IsStarted();
    if (sys.bootDisplay.NeedsArtwork()) unavailable = unavailable ||
        modules::BootArtworkUnavailableAfterRecovery(
            static_cast<modules::litchcraft_frame_store::Status>(
                sys.settingsPersistence.GetDiagnostics().frameStoreRecovery));
    if ((sys.bootDisplay.NeedsArtwork() || sys.bootDisplay.UsesArtwork() ||
         bootArtwork.IsBootArtworkPlaying()) &&
        sys.gamePersistence.TryBeginLitchcraftPresentation(
            modules::LitchcraftSession::PresentationAccess::Control)) {
        modules::GamePersistenceCoordinator::LitchcraftPresentationGuard admission(sys.gamePersistence);
        sys.bootDisplay.UpdateArtwork(bootArtwork, unavailable);
    }
    auto* capture = sys.ledAnimation.BeginCapture();
    if (capture == nullptr) return firmware::LedRenderAttemptResult::NotAttempted;
    auto& presentation = *capture;
    presentation.bulkTransferActive = sys.ble.IsBulkTransferActive()
#if defined(LITCH_ESP32S3)
        || sys.settingsPersistence.IsLitchcraftBulkTransferActive()
#endif
        ;
    presentation.capturedAtMs = nowMs;
    presentation.contextEpoch = sys.ledAnimation.ContextEpoch();
    presentation.hintEpoch = sys.stateMachine.GetRenderState().hintPresentationEpoch;
    presentation.output = CaptureLedOutputPresentation(sys);
    presentation.boardRotate = GetFlipSidesSetting(sys.settings);
    presentation.firstPlayerIsP2 = GetFirstPlayerIsP2Setting(sys.settings);
    presentation.health = sys.runtimeHealth.Snapshot();
    presentation.artworkUnavailable = unavailable;
    const auto ota = sys.ble.GetOtaLedPresentation();
    if (ota.active) {
        presentation.scene = firmware::OtaLedScene{ota};
        presentation.output = CaptureLedOutputPresentation(sys, true);
    } else if (sys.comptest.IsActive()) {
        presentation.scene = firmware::ComplianceLedPresentation{
            sys.comptest.CurrentLoad(), sys.comptest.StartedAtMs()};
    } else if (sys.ble.IsOtaInProgress() ||
               sys.amptest.IsActive() || sys.amptestVerify.IsActive()) {
        presentation.scene = firmware::HoldLedPresentation{};
    } else if (sys.hardwareTest.IsActive() || sys.factoryBootOnly) {
        firmware::ManufacturingLedPresentation test;
        CaptureTest(sys, test);
        presentation.scene = test;
    } else {
        sys.gamePersistence.PresentPendingOccupiedBoardFeedback(sys.edgeCue);
    const bool boardRotate = presentation.boardRotate;
    const bool firstPlayerIsP2 = presentation.firstPlayerIsP2;
    const uint32_t boardObservationSequence =
        sys.stateMachine.GetLastBoardObservationSequence();
    const bool setupCompletionActive =
        sys.edgeCue.Current(fxe::Millis()).kind ==
        modules::EdgeLedCueKind::SetupComplete;
    if (((sys.menuOverlay.IsActive() &&
          sys.menuOverlay.PreservesTurnIndicator()) ||
         setupCompletionActive) &&
        sys.setupPresentationObservationValid &&
        boardObservationSequence !=
            sys.setupPresentationObservationSequence) {
        if (sys.stateMachine.GetLastBoardObservationChangedMask() != 0ULL) {
            sys.menuOverlay.Stop();
            sys.edgeCue.Clear();
            sys.gamePersistence.CancelSetupPresentationEvents();
            sys.setupPresentationObservationValid = false;
        } else {
            sys.setupPresentationObservationSequence =
                boardObservationSequence;
        }
    } else if (!sys.menuOverlay.IsActive() && !setupCompletionActive) {
        sys.setupPresentationObservationValid = false;
    }

    if (!sys.menuOverlay.IsActive() && !sys.quickSettingsOverlayActive &&
        StartupPresentationComplete(sys)) {
        modules::SetupPresentationEvent event;
        if (sys.gamePersistence.TryTakeSetupPresentationEvent(event)) {
            if (event == modules::SetupPresentationEvent::Ready) {
                sys.edgeCue.Show(modules::EdgeLedCueKind::SetupComplete,
                                 modules::UiOwner::None,
                                 fxe::Millis());
                sys.setupPresentationObservationSequence =
                    boardObservationSequence;
                sys.setupPresentationObservationValid = true;
                // Ready is admitted after button policy; retire only its automatic request before this first frame.
                runtime::SyncBeginnerModeHintDisplay(sys.stateMachine, modules::UiOwner::None,
                                                    sys.beginnerModeForcedHintDisplay);
            }
        }
    }

    auto& game = presentation.scene.emplace<firmware::GameLedPresentation>();
    auto& interactionSnapshot = game.interaction.snapshot;
    interactionSnapshot = runtime::BuildInteractionModelSnapshot(sys);
    runtime::CaptureInteractionModelPresentation(sys.stateMachine, interactionSnapshot, game.interaction);
    modules::EdgeLedIntent menuIntent;
    if (sys.quickSettingsOverlayActive && sys.quickSettingsOverlayOwner != modules::UiOwner::None) {
        menuIntent.menuActive = true;
        menuIntent.menuOwner = sys.quickSettingsOverlayOwner;
        menuIntent.menuSelectionIndex = sys.quickSettingsOverlaySelectedIndex;
        menuIntent.menuHoldActive = false;
        menuIntent.menuHoldFillAll = false;

        switch (sys.quickSettingsOverlaySelectedIndex) {
            case QUICK_SETTINGS_ITEM_CPU_DIFFICULTY:
                menuIntent.menuValue01_255 = ComputeMenuValue01_255ForQuickSettings(
                    modules::menu_option_indices::VS_CPU,
                    static_cast<int>(ReadVsCpuLevel(sys.settings)));
                break;
            case QUICK_SETTINGS_ITEM_CPU_PLAYER:
                menuIntent.menuValue01_255 = GetVsCpuPlayerIsP2Setting(sys.settings) ? 255U : 0U;
                break;
            case QUICK_SETTINGS_ITEM_FIRST_PLAYER: {
                int firstPlayer = sys.settings.ReadValue("GAME_FIRST_PLAYER", static_cast<int32_t>(1));
                firstPlayer = (firstPlayer == 2) ? 2 : 1;
                menuIntent.menuValue01_255 = ComputeMenuValue01_255ForQuickSettings(
                    modules::menu_option_indices::GAME_FIRST_PLAYER,
                    firstPlayer);
                break;
            }
            case QUICK_SETTINGS_ITEM_BRIGHTNESS:
                menuIntent.menuValue01_255 = ComputeMenuValue01_255ForQuickSettings(
                    modules::menu_option_indices::BRIGHTNESS,
                    static_cast<int>(ReadQuickBrightness(sys.settings)));
                break;
            case QUICK_SETTINGS_ITEM_WHITE_SQUARES:
                menuIntent.menuValue01_255 = ReadWhiteSquaresOn(sys.settings) ? 255U : 0U;
                break;
            case QUICK_SETTINGS_ITEM_ROTATE:
                menuIntent.menuValue01_255 = ComputeMenuValue01_255ForQuickSettings(
                    modules::menu_option_indices::FLIP_SIDES,
                    GetFlipSidesSetting(sys.settings) ? 1 : 0);
                break;
            case QUICK_SETTINGS_ITEM_TIMER:
                menuIntent.menuValue01_255 = ComputeMenuValue01_255ForQuickSettings(
                    modules::menu_option_indices::TIMER,
                    static_cast<int>(sys.settings.ReadValue(
                        "TIMER", static_cast<uint32_t>(0U))));
                break;
            default:
                menuIntent.menuValue01_255 = 0U;
                break;
        }
    }

    if (!sys.quickSettingsOverlayActive) {
        const bool directOpenAllowed = modules::IsQuickSettingsDirectEntryAllowed({
            .wakeInputSuppressed = sys.sleep.ShouldIgnoreButtonEvents(),
            .hardwareTestActive = sys.hardwareTest.IsActive(),
            .ampTestActive = sys.amptest.IsActive(),
            .resetHoldActive = sys.gameResetHoldActive,
            .menuOverlayActive = sys.menuOverlay.IsActive(),
            .textOverlayActive = interactionSnapshot.render.textOverlayActive,
            .kingQueenSwapActive = interactionSnapshot.interaction.kingQueenSwapPromptActive,
            .undoPromptActive = interactionSnapshot.interaction.undoPromptActive,
            .promotionActive = runtime::IsHumanPromotionActive(interactionSnapshot.interaction),
        });
        const auto preview = modules::BuildQuickSettingsHoldPreview(
            sys.quickSettingsOverlayController, &sys.gamePersistence.Litchcraft(),
            directOpenAllowed);
        if (preview.menuActive) {
            menuIntent = preview;
            menuIntent.menuSelectionIndex = sys.quickSettingsOverlaySelectedIndex;
        }
    }


    modules::led_frame_composer::ComposeInputs inputs;
    inputs.interactionSnapshot = &interactionSnapshot;
    inputs.settings = &sys.settings;
    inputs.runtimeHealth = presentation.health;
    inputs.menuActive = sys.menuOverlay.IsActive() || sys.quickSettingsOverlayActive;
    inputs.menuIntent = menuIntent;
    inputs.edgeCueIntent = sys.edgeCue.Current(nowMs);
    inputs.beginnerModeP1Enabled = sys.beginnerModeController.IsEffective(modules::UiOwner::P1);
    inputs.beginnerModeP2Enabled = sys.beginnerModeController.IsEffective(modules::UiOwner::P2);
    if (sys.sleep.GetState() != firmware::SleepState::Awake) sys.boardStylePreview.Cancel();
    game.stylePreview = sys.boardStylePreview.Capture(inputs);
    presentation.settings = modules::led_frame_composer::CaptureComposeSettings(sys.settings);
    presentation.positionRevision = sys.stateMachine.GetChessInteractionModel().acknowledged.revision;
    presentation.physicalOccupiedMask = interactionSnapshot.board.physicalOccupiedMask;
    presentation.menuIntent = menuIntent;
    presentation.edgeCue = inputs.edgeCueIntent;
    presentation.controlInputs = CaptureControlFeedback(sys, inputs, false);
    presentation.controlEvents = sys.controlFeedback.CaptureEvents();
    presentation.quickSettingsActive = sys.quickSettingsOverlayActive;
    presentation.menuControllerActive = false;
    presentation.menuSelection = sys.quickSettingsOverlaySelectedIndex;
    presentation.menuHoldThresholdMs = sys.quickSettingsOverlayController.HoldThresholdMs();
    presentation.resetHold = {sys.gameResetHoldOwner, sys.gameResetHoldStartMs,
        sys.gameResetHoldActive, sys.gameResetHoldTriggered, sys.gameResetHoldRejected};
    presentation.paletteEditOwners = sys.runtimeHardware.resolved.revision == hardware::BoardRevision::V3_3S3
        ? 0U : sys.paletteEditPreview.VisibleOwners();
    presentation.beginnerModeP1 = inputs.beginnerModeP1Enabled;
    presentation.beginnerModeP2 = inputs.beginnerModeP2Enabled;
    presentation.beginnerStingerOwner = sys.beginnerModeStingerOwner;
    presentation.beginnerStingerEnabling = sys.beginnerModeStingerEnabling;
    presentation.beginnerStingerStartedMs = sys.beginnerModeStingerStartMs;
    presentation.startupPieceAccent = true;
    presentation.startupBoardObserved = sys.startupBoardSnapshotObserved;
    presentation.boardObservationSequence = boardObservationSequence;
    presentation.allPiecesFlash = sys.stateMachine.GetRenderState().allPiecesFlash;
    presentation.sleepState = sys.sleep.GetState();
    presentation.sleepRemainingMs = sys.sleep.GetRemainingCountdownMs();
    presentation.sleepDurationMs = sys.sleep.GetCountdownDurationMs();
    presentation.sleepScale256 = sys.sleep.GetFadeScale256();
    presentation.sleepStartedMs = sys.sleep.GetCountdownStartedMs();
    auto& art = sys.gamePersistence.Litchcraft();
    presentation.entryHold = art.CaptureEntryHoldPresentation();
    if (presentation.output.canvasVisible) presentation.physicalOccupiedMask = art.Occupancy();
    presentation.initialStoreModePending = art.IsInitialStoreModePending();
    presentation.artworkModeGeneration = art.ModeGeneration();
    presentation.artworkCanvasGeneration = art.CanvasGeneration();
    sys.identifyPresentation.Consume(sys.ble, nowMs);
    presentation.identify = sys.identifyPresentation.Capture();
    presentation.pairingAllowed = modules::OWNER_PAIRING_ENABLED && sys.ownerPairingPrepared &&
        !runtime::IsRuntimeHealthPlayPaused(presentation.health);
    if (presentation.pairingAllowed) presentation.pairing = sys.boardOwnerPairing.CaptureCue();
    if (presentation.output.canvasVisible) SuppressWaterSlosh(sys);
    else presentation.waterRiderSquare = UpdateWaterSlosh(sys, inputs);
    presentation.water = sys.waterSlosh;
    if (!sys.ledAnimation.StartAttempted()) InitializeAnimation(sys, &game);
    if (art.IsCanvasVisible() || sys.bootDisplay.UsesArtwork()) {
        auto& artwork = presentation.scene.emplace<modules::litchcraft_presentation::Inputs>();
        art.CapturePresentation(artwork);
    }
    }
    if (!sys.ledAnimation.StartAttempted()) InitializeAnimation(sys, nullptr);
    // Capture after setup feedback may have retired the overlay, and outside
    // the scene variant so replacing Game with Litchcraft retains its menu.
    presentation.menu = sys.menuOverlay;
    const uint32_t sequence = sys.ledAnimation.FinishCapture();
    if (sequence == 0U) return firmware::LedRenderAttemptResult::NotAttempted;
    if (!waitForCompletion) return firmware::LedRenderAttemptResult::NotAttempted;
    const bool completed = sys.ledAnimation.WaitForPresentation(sequence);
    ConsumeLedPresentation(sys);
    return completed ? firmware::LedRenderAttemptResult::Completed : firmware::LedRenderAttemptResult::Denied;
}
} // namespace firmware_internal
#endif
