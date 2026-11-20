#ifdef ARDUINO
#include <Arduino.h>

#include "firmware/firmware_internal.hpp"
#include "firmware/local_control_requests.hpp"
#include "firmware/firmware_tasks_internal.hpp"
#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/beginner_mode_policy.hpp"
#include "modules/board_owner_pairing_feature.hpp"
#include "modules/button_event_transform.hpp"
#include "modules/game_reset_hold_policy.hpp"
#include "modules/hint_button_gesture_controller.hpp"
#include "modules/hint_button_policy.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "modules/physical_palette_shortcut.hpp"
#include "modules/reset_hold_gesture_controller.hpp"
#include "modules/vs_cpu_turn.hpp"
#include "modules/white_squares_setting.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "runtime/interaction_domain_types.hpp"
namespace firmware_internal {
namespace {

void SetPhysicalHoldTicksAllowed(FirmwareSystem& sys, const bool allowed,
                                 const uint32_t throughMs = 0U) {
    if (allowed) {
        sys.gamePersistence.Litchcraft().SetPhysicalButtonTickTime(throughMs);
        sys.boardOwnerPairing.SetPhysicalButtonTickTime(throughMs);
        sys.hardwareTest.SetPhysicalButtonTickTime(throughMs);
    }
    sys.gamePersistence.Litchcraft().SetPhysicalButtonTicksAllowed(allowed);
    sys.boardOwnerPairing.SetPhysicalButtonTicksAllowed(allowed);
    sys.hardwareTest.SetPhysicalButtonTicksAllowed(allowed);
}

void ResetDispatchedButtonInput(FirmwareSystem& sys, const uint32_t nowMs) {
    // The source has already suppressed held controls. Retire every downstream
    // owner without fabricating a Release which could become Undo or Hint.
    sys.buttonInputQueue.Clear();
    sys.gamePersistence.ResetKingResignationInput();
    sys.gamePersistence.Litchcraft().CancelEntryHold();
    sys.gameResetHoldActive = false;
    sys.gameResetHoldStartMs = 0U;
    sys.gameResetHoldOwner = modules::UiOwner::None;
    sys.gameResetHoldTriggered = false;
    sys.hintButtonController.Reset();
    sys.resetHoldController.Reset();
    sys.quickSettingsOverlayController.Reset();
    sys.hardwareTest.ResetButtonInput();
    sys.paletteEditPreview.Reset();
    sys.controlFeedback.Reset();
    sys.waterSlosh.ResetControlInput(nowMs);
    if (modules::OWNER_PAIRING_ENABLED && sys.ownerPairingPrepared)
        sys.boardOwnerPairing.ReseedPhysicalInput();
#if defined(LITCH_BLE)
    sys.ble.ResetConnectionButtonState();
#endif
}

} // namespace

void PollButtonsAndDispatch() {
    auto& sys = GetSystem();
    SetPhysicalHoldTicksAllowed(sys, false);
    if (sys.gpioUserInput.ConsumeInputReset()) ResetDispatchedButtonInput(sys, fxe::Millis());
    if (sys.factoryBootOnly) {
        sys.gpioUserInput.SetRotate180(false);
        sys.sleep.UpdateIgnoreGate();
        modules::ButtonEvent event;
        const uint32_t nowMs = fxe::Millis();
        while (sys.gpioUserInput.TryReadButtonEvent(event)) {
            if (sys.gpioUserInput.ConsumeInputReset()) ResetDispatchedButtonInput(sys, nowMs);
            if (!sys.sleep.ShouldIgnoreButtonEvents() && sys.hardwareTest.IsActive() &&
                !sys.ble.IsOtaInProgress() && !sys.comptest.IsActive()) {
                sys.hardwareTest.OnButtonEvent(event, event.timestampValid ? event.tMs : nowMs);
            }
        }
        if (sys.gpioUserInput.ConsumeInputReset()) ResetDispatchedButtonInput(sys, nowMs);
        SetPhysicalHoldTicksAllowed(sys, !sys.gpioUserInput.HasPendingButtonRelease(),
                                    sys.gpioUserInput.ButtonTimelineThroughMs());
        return;
    }
    runtime::InteractionDispatcher dispatcher(sys.stateMachine, sys.gamePersistence, sys.appEventQueue);
    const uint32_t nowMs = fxe::Millis();
    const auto& derived = sys.gamePersistence.GetEngineDerivedState();
    modules::white_squares_setting::MigrateLegacyWhiteSquaresSetting(sys.settings);
    auto& resetHoldController = sys.resetHoldController;
    auto& hintButtonController = sys.hintButtonController;

    struct BenchmarkControlContext {
        FirmwareSystem* system;
        modules::HintButtonGestureController* hint;
    };
    static BenchmarkControlContext benchmarkControls{&sys, &hintButtonController};
    sys.gamePersistence.SetBenchmarkControlsIdleProbe(&benchmarkControls, [](void* context) {
        const auto& state = *static_cast<BenchmarkControlContext*>(context);
        const auto& system = *state.system;
        modules::UiOwner owner = modules::UiOwner::None;
        uint8_t progress = 0U;
        return system.gpioUserInput.AreCachedControlsReleased() &&
            !system.hardwareTest.IsActive() && !system.gameResetHoldActive &&
            !system.quickSettingsOverlayActive &&
            !system.quickSettingsOverlayController.TryGetOpenHoldProgress(owner, progress) &&
            !state.hint->IsHeld(modules::UiOwner::P1) && !state.hint->IsHeld(modules::UiOwner::P2);
    });
    const auto clearGameResetHold = [&]() {
        sys.gameResetHoldActive = false;
        sys.gameResetHoldStartMs = 0;
        sys.gameResetHoldOwner = modules::UiOwner::None;
        sys.gameResetHoldTriggered = false;
    };
    const auto consumeSleepInput = [&]() {
        sys.gpioUserInput.SuppressHeldControlsUntilRelease();
        (void)sys.gpioUserInput.ConsumeInputReset();
        ResetDispatchedButtonInput(sys, nowMs);
    };
    if (sys.sleep.ShouldIgnoreActivity(firmware::SleepTransitionCause::ButtonPress)) {
        consumeSleepInput();
        if (modules::OWNER_PAIRING_ENABLED && sys.ownerPairingPrepared)
            sys.boardOwnerPairing.ReseedPhysicalInput();
        return;
    }
    SyncOwnerPairingControls(sys);

    // Exclusive tests cancel timed gameplay owners even with no new edge.
    if (sys.hardwareTest.IsActive() || sys.comptest.IsActive()) {
        clearGameResetHold();
        resetHoldController.Reset(); hintButtonController.Reset();
        sys.quickSettingsOverlayController.Reset();
    }

    // Manufacturing coverage names physical P1/P2 controls. Keep it
    // independent of a prior user's board-orientation setting.
    const bool boardRotate = GetFlipSidesSetting(sys.settings);
    const bool playerEndIsP2 = GetFirstPlayerIsP2Setting(sys.settings);
    const bool rotateEnabled = sys.hardwareTest.IsActive()
                                   ? false
                                   : modules::ComputeControlBankSwap(
                                         boardRotate, playerEndIsP2,
                                         sys.gamePersistence.Litchcraft().IsActive());
    if (sys.gpioUserInput.SetRotate180(rotateEnabled)) {
        sys.controlFeedback.Reset();
        sys.gamePersistence.Litchcraft().CancelEntryHold();
        // GPIO ownership is reseeded under the new orientation. Discard any
        // input and gesture state tagged with the previous owner mapping.
        sys.buttonInputQueue.Clear();
        sys.gamePersistence.ResetKingResignationInput();
        if (modules::OWNER_PAIRING_ENABLED && sys.ownerPairingPrepared)
            sys.boardOwnerPairing.ReseedPhysicalInput();
#if defined(LITCH_BLE)
        sys.ble.ResetConnectionButtonState();
#endif
        clearGameResetHold();
        resetHoldController.Reset();
        hintButtonController.Reset();
        sys.quickSettingsOverlayController.Reset();
    }

    sys.sleep.UpdateIgnoreGate();
    if (sys.sleep.ShouldIgnoreButtonEvents()) {
        modules::ButtonEvent ignored;
        while (sys.gpioUserInput.TryReadButtonEvent(ignored)) {
            sys.gamePersistence.StopBenchmark();
        }
        sys.buttonInputQueue.Clear();
        sys.gamePersistence.ResetKingResignationInput();
#if defined(LITCH_BLE)
        sys.ble.ResetConnectionButtonState();
#endif
        clearGameResetHold();
        hintButtonController.Reset();
        resetHoldController.Reset();
        sys.quickSettingsOverlayController.Reset();
        return;
    }

    modules::ButtonEvent rawEvent;
    while (sys.gpioUserInput.TryReadButtonEvent(rawEvent)) {
        if (sys.gpioUserInput.ConsumeInputReset()) ResetDispatchedButtonInput(sys, nowMs);
        if (rawEvent.button == modules::UserButton::Press) continue;
        const uint32_t rawTimeMs = rawEvent.timestampValid ? rawEvent.tMs : nowMs;
        sys.boardStylePreview.Cancel();
        sys.gamePersistence.StopBenchmark();
#if defined(LITCH_BLE)
        sys.ble.PublishConnectionButtonState(rawEvent, rawTimeMs);
#endif
        sys.waterSlosh.ObserveControl(rawEvent, rawTimeMs);
        if (!sys.buttonInputQueue.Enqueue(runtime::ButtonInputEvent{
                .tMs = rawTimeMs,
                .button = rawEvent.button,
                .event = rawEvent.event,
                .owner = rawEvent.owner,
                .timestampValid = rawEvent.timestampValid,
            })) {
            sys.gpioUserInput.SuppressHeldControlsUntilRelease();
            (void)sys.gpioUserInput.ConsumeInputReset();
            ResetDispatchedButtonInput(sys, nowMs);
            break;
        }
    }

    if (sys.gpioUserInput.ConsumeInputReset()) ResetDispatchedButtonInput(sys, nowMs);

    const uint32_t physicalThroughMs = sys.gpioUserInput.ButtonTimelineThroughMs();
    if (sys.buttonInputQueue.Size() == 0U && !sys.gpioUserInput.HasPendingButtonRelease()) {
        modules::ButtonEvent fallbackEvent;
        uint32_t fallbackTimeMs = 0U;
        if (sys.gamePersistence.Litchcraft().TryExpireEntryButton(
                physicalThroughMs, fallbackEvent, fallbackTimeMs)) {
            (void)sys.buttonInputQueue.EnqueueFront(runtime::ButtonInputEvent{
                .tMs = fallbackTimeMs,
                .button = fallbackEvent.button,
                .event = fallbackEvent.event,
                .owner = fallbackEvent.owner,
                .litchcraftEntryArbitrated = true,
                .timestampValid = fallbackEvent.timestampValid,
            });
        }
    }

    const auto advanceGameplayHolds = [&](const uint32_t throughMs) {
        const bool resetHoldStillEligible = modules::IsGameResetHoldEligible(
            {.menuVisualActive = false,
             .menuOverlayActive = sys.menuOverlay.IsActive(),
             .quickSettingsOverlayActive = sys.quickSettingsOverlayActive,
             .textOverlayActive = sys.stateMachine.IsTextOverlayActive(),
             .promotionActive = sys.stateMachine.IsPromotionActive()});
        if (resetHoldStillEligible) {
            modules::ResetHoldAction resetTickAction;
            while (resetHoldController.Tick(resetTickAction, throughMs)) {
                if (resetTickAction.type != modules::ResetHoldActionType::TriggerReset ||
                    sys.gameResetHoldTriggered) {
                    continue;
                }
#ifdef LITCH_TRACE
                auto& recorder = GetTraceRecorder();
                if (recorder.IsActive()) {
                    recorder.RecordButton((sys.gameResetHoldOwner == modules::UiOwner::P2) ? "P2.X+O" : "P1.X+O",
                                          "hold_reset");
                }
#endif
                (void)RunBoardControlAction(
                    sys,
                    modules::ControlAction::NewGame);
                sys.gameResetHoldTriggered = true;
            }
        } else {
            clearGameResetHold();
            resetHoldController.Reset();
        }
        if (!sys.quickSettingsOverlayActive && !sys.gameResetHoldActive) {
            if (!modules::CanTrackCoachingGesture(sys.stateMachine)) {
                hintButtonController.Cancel(modules::UiOwner::P1);
                hintButtonController.Cancel(modules::UiOwner::P2);
            }
            modules::HintButtonAction hintTickAction;
            while (hintButtonController.Tick(hintTickAction, throughMs)) {
                if (hintTickAction.type == modules::HintButtonActionType::BeginnerModeHold) {
                    (void)RunBoardControlAction(
                        sys,
                        modules::ControlAction::Coaching,
                        hintTickAction.owner);
                }
            }
            modules::QuickSettingsAction quickTickAction;
            while (sys.quickSettingsOverlayController.Tick(quickTickAction, true, throughMs)) {
                if (quickTickAction.type == modules::QuickSettingsActionType::OpenOverlay) {
                    OpenBoardQuickSettings(sys, quickTickAction.owner, throughMs);
                }
            }
        }
    };

    runtime::ButtonInputEvent inputEvent;
    while (sys.buttonInputQueue.TryPeek(inputEvent)) {
        const modules::ButtonEvent queued{inputEvent.button, inputEvent.event, inputEvent.owner,
                                         inputEvent.tMs, inputEvent.timestampValid};
        auto& artwork = sys.gamePersistence.Litchcraft();
        const bool craftOwnsInput = artwork.IsActive() || artwork.IsBootArtworkPlaying();
        if (craftOwnsInput && !BeginPhysicalLitchcraftPresentation(sys,
                artwork.ButtonPresentationAccess(queued))) break;
        modules::GamePersistenceCoordinator::LitchcraftPresentationGuard presentation(
            craftOwnsInput ? &sys.gamePersistence : nullptr);
        if (!sys.buttonInputQueue.TryDequeue(inputEvent)) break;
        const bool consumedByBoot = sys.bootDisplay.ConsumeButton(queued);
        if (sys.bootDisplay.WasCancelled()) {
            CancelBootPresentation(sys);
            artwork.CancelBootArtwork();
        }
        if (consumedByBoot) continue;
        if (inputEvent.button == modules::UserButton::Press) continue;
        sys.gamePersistence.StopBenchmark();
        uint32_t eventTimeMs = inputEvent.tMs;
        modules::ButtonEvent event{inputEvent.button, inputEvent.event, inputEvent.owner,
                                   inputEvent.tMs, inputEvent.timestampValid};
        SyncOwnerPairingControls(sys);
        if (modules::OWNER_PAIRING_ENABLED && sys.ownerPairingPrepared &&
            !sys.hardwareTest.IsActive() && !sys.comptest.IsActive() &&
            !inputEvent.litchcraftEntryArbitrated &&
            (modules::IsXButton(event.button) || modules::IsOButton(event.button))) {
            const uint8_t physicalEnd = static_cast<uint8_t>(
                (modules::ResolveButtonEventOwner(event) == modules::UiOwner::P2) != rotateEnabled);
            if (sys.boardOwnerPairing.ObserveButton({physicalEnd,
                    modules::IsXButton(event.button),
                    event.event != fxe::Button::Event::Release, eventTimeMs})) {
                if (sys.quickSettingsOverlayActive) {
                    sys.quickSettingsOverlayIdleElapsed.Reset();
                }
                sys.gamePersistence.Litchcraft().CancelEntryHold();
                sys.gamePersistence.ResetKingResignationInput();
                clearGameResetHold();
                resetHoldController.Reset();
                hintButtonController.Reset();
                sys.quickSettingsOverlayController.Reset();
                continue;
            }
        }
        if (inputEvent.event == fxe::Button::Event::Press) {
            if (sys.sleep.NoteButtonPressActivity()) {
                // Sleep consumed this cancelling or ignored Press. Discard its paired
                // gesture stream so it cannot also trigger reset, undo, hint,
                // menu, or manufacturing behavior.
                consumeSleepInput();
                return;
            }
        }
        if (sys.comptest.IsActive()) {
            clearGameResetHold(); hintButtonController.Reset();
            resetHoldController.Reset(); sys.quickSettingsOverlayController.Reset();
            continue;
        }
        if (sys.hardwareTest.IsActive()) {
            sys.hardwareTest.OnButtonEvent(event, eventTimeMs);
            if (!sys.hardwareTest.IsActive()) {
                ReleaseHardwareTestInput(sys);
            }
            clearGameResetHold();
            hintButtonController.Reset();
            resetHoldController.Reset();
            sys.quickSettingsOverlayController.Reset();
            continue;
        }
        if (sys.amptest.IsActive() && sys.amptest.HandleButtonEvent(event)) {
            clearGameResetHold();
            hintButtonController.Reset();
            resetHoldController.Reset();
            sys.quickSettingsOverlayController.Reset();
            continue;
        }
        if (sys.gamePersistence.Litchcraft().OwnsButton(event)) {
            sys.gamePersistence.ObserveLitchcraftButton(event, eventTimeMs);
            clearGameResetHold(); hintButtonController.Reset(); resetHoldController.Reset();
            sys.quickSettingsOverlayController.Reset();
            continue;
        }
        advanceGameplayHolds(eventTimeMs);
        const bool playFaceChordAvailable =
            !sys.menuOverlay.IsActive() &&
            !sys.quickSettingsOverlayActive &&
            !sys.stateMachine.IsTextOverlayActive() &&
            !sys.stateMachine.IsPromotionActive() &&
            !sys.stateMachine.IsUndoPromptActive() &&
            !sys.stateMachine.IsKingQueenSwapPromptActive() &&
            sys.controlMutation.GetActionState() !=
                modules::ControlActionState::UndoPending;
        if (!inputEvent.litchcraftEntryArbitrated &&
            playFaceChordAvailable &&
            (modules::IsXButton(event.button) || modules::IsOButton(event.button)) &&
            (sys.gamePersistence.Litchcraft().IsEntryHoldActive() ||
             event.event == fxe::Button::Event::Press)) {
            const auto result =
                sys.gamePersistence.ObserveLitchcraftEntryButton(
                    event, eventTimeMs);
            if (result == modules::LitchcraftSession::ModeChordResult::Blocked) {
                sys.stateMachine.TriggerAllPiecesFlash(1000U);
                sys.edgeCue.Show(modules::EdgeLedCueKind::OccupiedBoardBlocked,
                                 modules::UiOwner::None, eventTimeMs);
                clearGameResetHold();
                hintButtonController.Reset();
                resetHoldController.Reset();
                sys.quickSettingsOverlayController.Reset();
                continue;
            }
            if (result == modules::LitchcraftSession::ModeChordResult::Replay ||
                result == modules::LitchcraftSession::ModeChordResult::Blocked) {
                modules::ButtonEvent fallbackEvent;
                uint32_t fallbackTimeMs = 0U;
                if (sys.gamePersistence.Litchcraft().TakeEntryButtonForReplay(
                        fallbackEvent, fallbackTimeMs)) {
                    inputEvent.litchcraftEntryArbitrated = true;
                    (void)sys.buttonInputQueue.EnqueueFront(inputEvent);
                    inputEvent = runtime::ButtonInputEvent{
                        .tMs = fallbackTimeMs,
                        .button = fallbackEvent.button,
                        .event = fallbackEvent.event,
                        .owner = fallbackEvent.owner,
                        .litchcraftEntryArbitrated = true,
                        .timestampValid = fallbackEvent.timestampValid,
                    };
                    eventTimeMs = inputEvent.tMs;
                    event = fallbackEvent;
                }
            } else if (result ==
                       modules::LitchcraftSession::ModeChordResult::Entered) {
                clearGameResetHold();
                hintButtonController.Reset();
                resetHoldController.Reset();
                sys.quickSettingsOverlayController.Reset();
                continue;
            } else if (result ==
                       modules::LitchcraftSession::ModeChordResult::Pending) {
                continue;
            } else if (result ==
                       modules::LitchcraftSession::ModeChordResult::Cancelled) {
                continue;
            }
        }
        const bool eventIsPressLike = (inputEvent.event == fxe::Button::Event::Press) ||
                                      (inputEvent.event == fxe::Button::Event::Repeat);

        if (sys.quickSettingsOverlayActive) {
            modules::QuickSettingsAction quickAction;
            (void)sys.quickSettingsOverlayController.HandleButtonEvent(event, true, quickAction, eventTimeMs);

            HandleBoardQuickSettingsAction(sys, quickAction, eventTimeMs);
            continue;
        }

        if (sys.gamePersistence.HandleKingResignationButton(event, eventTimeMs)) {
            hintButtonController.Reset();
            continue;
        }

        const bool resetHoldEligible = modules::IsGameResetHoldEligible(
            {.menuVisualActive = false,
             .menuOverlayActive = sys.menuOverlay.IsActive(),
             .quickSettingsOverlayActive = sys.quickSettingsOverlayActive,
             .textOverlayActive = sys.stateMachine.IsTextOverlayActive(),
             .promotionActive = sys.stateMachine.IsPromotionActive()});
        if (!resetHoldEligible) {
            clearGameResetHold();
            resetHoldController.Reset();
        } else {
            modules::ResetHoldAction resetAction;
            const bool consumedByResetHold = resetHoldController.HandleButtonEvent(event, resetAction, eventTimeMs);
            if (resetAction.type == modules::ResetHoldActionType::StartHold) {
                clearGameResetHold();
                sys.gameResetHoldActive = true;
                sys.gameResetHoldStartMs = eventTimeMs;
                sys.gameResetHoldRejected = false;
                sys.gameResetHoldOwner = resetAction.owner;
                hintButtonController.Reset();
                sys.quickSettingsOverlayController.Reset();
                continue;
            }
            if (resetAction.type == modules::ResetHoldActionType::CancelHold) {
                clearGameResetHold();
                hintButtonController.Reset();
                sys.quickSettingsOverlayController.Reset();
                continue;
            }
            if (consumedByResetHold) {
                continue;
            }
        }

        // A failed save changes durability, not validated in-memory ownership.
        const uint32_t vsCpuLevel = sys.settings.ReadValue("VS_CPU_LEVEL", static_cast<uint32_t>(0U));
        const bool firstPlayerIsP2 = GetFirstPlayerIsP2Setting(sys.settings);
        const bool cpuPlayerIsP2 = GetVsCpuPlayerIsP2Setting(sys.settings);
        const uint8_t undoCount = modules::GetVsCpuUndoPlyCount(
            derived.sideToMove, vsCpuLevel, firstPlayerIsP2, cpuPlayerIsP2);

        const runtime::TaggedInteractionState& interactionPhase = sys.stateMachine.GetChessInteractionModel().phase;
        const bool humanDuckRelocation =
            sys.gamePersistence.GetGameVariant() == modules::GameVariant::Duck &&
            modules::IsHumanDuckRelocation(interactionPhase);
        const bool canTrackHintGesture =
            (interactionPhase.tag == runtime::ChessInteractionPhase::Idle ||
             humanDuckRelocation) &&
            !sys.stateMachine.IsMoveSettlingActive() && !sys.stateMachine.HasQueuedCpuMove() &&
            !sys.stateMachine.IsPromotionActive();
        if (modules::CanTrackCoachingGesture(sys.stateMachine)) {
            const modules::UiOwner activeOwner = modules::led_frame_composer::OwnerForSideToMove(derived.sideToMove, firstPlayerIsP2);
            const modules::HintButtonRoutingDecision hintRouting = modules::ResolveHintButtonRouting(
                true,
                modules::IsOButton(event.button),
                event.owner,
                activeOwner,
                canTrackHintGesture && !sys.stateMachine.IsPlacementModeActive() &&
                    sys.stateMachine.IsPhysicalOccupancyConsistentWithInteraction() &&
                    !modules::assistance_preferences::CpuOwns(sys.settings, event.owner));

            if (hintRouting.shouldTrackGesture) {
                modules::HintButtonAction hintAction;
                if (hintButtonController.HandleButtonEvent(event, hintAction, eventTimeMs)) {
                    if (hintAction.type == modules::HintButtonActionType::ToggleHint &&
                        hintRouting.shouldRequestHintOnRelease) {
                        (void)RunBoardControlAction(
                            sys,
                            modules::ControlAction::Hint, hintAction.owner);
                    }
                    continue;
                }
            }
        } else if (modules::IsOButton(event.button) && hintButtonController.IsHeld(event.owner)) {
            hintButtonController.Cancel(event.owner);
        }
        modules::QuickSettingsAction quickAction;
        const bool directQuickSettingsEntryAllowed =
            modules::IsQuickSettingsDirectEntryAllowed({
                .wakeInputSuppressed = false,
                .hardwareTestActive = sys.hardwareTest.IsActive(),
                .ampTestActive = sys.amptest.IsActive(),
                .resetHoldActive = sys.gameResetHoldActive,
                .menuOverlayActive = sys.menuOverlay.IsActive(),
                .quickSettingsOverlayActive =
                    sys.quickSettingsOverlayActive,
                .textOverlayActive =
                    sys.stateMachine.IsTextOverlayActive(),
                .placementActive =
                    sys.stateMachine.IsPlacementModeActive(),
                .kingQueenSwapActive =
                    sys.stateMachine.IsKingQueenSwapPromptActive(),
                .undoPromptActive =
                    sys.stateMachine.IsUndoPromptActive(),
                .promotionActive =
                    sys.stateMachine.IsPromotionActive(),
            });
        if (sys.quickSettingsOverlayController.HandleButtonEvent(
                event,
                false,
                sys.controlMutation.GetActionState() ==
                    modules::ControlActionState::UndoPending,
                directQuickSettingsEntryAllowed,
                quickAction,
                eventTimeMs)) {
            if (quickAction.type == modules::QuickSettingsActionType::OpenOverlay) {
                OpenBoardQuickSettings(sys, quickAction.owner, eventTimeMs);
            }
            if (quickAction.type == modules::QuickSettingsActionType::UndoTap) {
                (void)RunBoardUndoTap(sys, undoCount, quickAction.owner);
            }
            continue;
        }
        if (!sys.stateMachine.IsPromotionActive() &&
            modules::physical_palette_shortcut::TryApply(
                event,
                eventIsPressLike,
                sys.settings,
                sys.controlMutation, &sys.paletteEditPreview)) {
            continue;
        }

        dispatcher.DispatchButtonEvent(inputEvent);
    }

    if (sys.gpioUserInput.ConsumeInputReset()) ResetDispatchedButtonInput(sys, nowMs);
    const bool physicalHoldTicksAllowed = sys.buttonInputQueue.Size() == 0U &&
        !sys.gpioUserInput.HasPendingButtonRelease();
    SetPhysicalHoldTicksAllowed(sys, physicalHoldTicksAllowed, physicalThroughMs);
    if (sys.hardwareTest.IsActive() || sys.comptest.IsActive()) return;
    if (physicalHoldTicksAllowed) advanceGameplayHolds(physicalThroughMs);

    if (sys.quickSettingsOverlayActive && sys.quickSettingsOverlayIdleElapsed.Started() &&
        sys.quickSettingsOverlayIdleElapsed.HasElapsed(modules::QUICK_SETTINGS_IDLE_CLOSE_MS)) {
        CloseBoardQuickSettings(sys);
    }
    const uint32_t vsCpuLevel = sys.settings.ReadValue("VS_CPU_LEVEL", static_cast<uint32_t>(0U));
    const bool firstPlayerIsP2 = GetFirstPlayerIsP2Setting(sys.settings);
    const bool cpuPlayerIsP2 = GetVsCpuPlayerIsP2Setting(sys.settings);
    const runtime::TaggedInteractionState& interactionPhase =
        sys.stateMachine.GetChessInteractionModel().phase;
    modules::BeginnerModePolicyInputs beginnerModeInputs;
    beginnerModeInputs.variant = sys.gamePersistence.GetGameVariant();
    beginnerModeInputs.setupCompleteActive = !StartupPresentationComplete(sys) ||
        sys.edgeCue.Current(nowMs).kind ==
        modules::EdgeLedCueKind::SetupComplete;
    beginnerModeInputs.quickSettingsOverlayActive =
        sys.quickSettingsOverlayActive;
    beginnerModeInputs.gameResetHoldActive = sys.gameResetHoldActive;
    beginnerModeInputs.textOverlayActive =
        sys.stateMachine.IsTextOverlayActive();
    beginnerModeInputs.placementModeActive =
        sys.stateMachine.IsPlacementModeActive();
    beginnerModeInputs.kingQueenSwapPromptActive =
        sys.stateMachine.IsKingQueenSwapPromptActive();
    beginnerModeInputs.undoPromptActive =
        sys.stateMachine.IsUndoPromptActive();
    beginnerModeInputs.promotionActive =
        sys.stateMachine.IsPromotionActive();
    beginnerModeInputs.moveSettlingActive =
        sys.stateMachine.IsMoveSettlingActive();
    beginnerModeInputs.queuedCpuMove =
        sys.stateMachine.HasQueuedCpuMove();
    beginnerModeInputs.sideToMove =
        sys.gamePersistence.GetEngineDerivedState().sideToMove;
    beginnerModeInputs.vsCpuLevel = vsCpuLevel;
    beginnerModeInputs.firstPlayerIsP2 = firstPlayerIsP2;
    beginnerModeInputs.cpuPlayerIsP2 = cpuPlayerIsP2;
    (void)sys.stateMachine.TryGetHintMoveSquares(
        beginnerModeInputs.hintFromSquare, beginnerModeInputs.hintToSquare);
    const modules::UiOwner beginnerModeOwner =
        modules::ActiveBeginnerModeOwnerForTurn(
            sys.beginnerModeController, beginnerModeInputs, interactionPhase);
    sys.gamePersistence.SetCoachingHintActive(
        modules::BeginnerModeOwnerForHumanTurn(
            sys.beginnerModeController, beginnerModeInputs) != modules::UiOwner::None);
    runtime::SyncBeginnerModeHintDisplay(
        sys.stateMachine,
        beginnerModeOwner,
        sys.beginnerModeForcedHintDisplay);
}
} // namespace firmware_internal
#endif
