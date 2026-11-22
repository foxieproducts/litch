#ifdef ARDUINO

#include "firmware/firmware_internal.hpp"

#include "firmware/ambient_runtime_health.hpp"
#include "firmware/firmware_tasks_internal.hpp"
#include "firmware/water_slosh_runtime.hpp"
#include "firmware/firmware_version.hpp"

#include <Arduino.h>

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "hardware/led_mapping.hpp"
#include "hardware/pca9555_occupancy_decode.hpp"
#include "logger.hpp"
#include "modules/button_event_transform.hpp"
#include "modules/edge_led_intent.hpp"
#include "modules/edge_led_state_selection.hpp"
#include "modules/game_persistence.hpp"
#include "modules/initial_fen.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/orientation.hpp"
#include "modules/ui_color_keys.hpp"
#include "modules/ui_color_presets.hpp"
#include "modules/ui_color_wheel.hpp"
#include "modules/user_input.hpp"
#include "modules/white_squares_setting.hpp"
#include "runtime/game_clock.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "runtime/square_mapping.hpp"

namespace {

uint32_t ClampSleepMs(const uint32_t sleepMs, const uint32_t maxSleepMs) {
    return (sleepMs > maxSleepMs) ? maxSleepMs : sleepMs;
}

void LogRuntimeHealthRevision(
    const runtime::RuntimeHealthSnapshot& snapshot) {
    logger::Info("health: revision=0x%04X state=%u causes=0x%04X",
                 static_cast<unsigned>(snapshot.revision),
                 static_cast<unsigned>(snapshot.state),
                 static_cast<unsigned>(snapshot.causes));
}

} // namespace

namespace firmware_internal {

bool ApplyRuntimeHealthFactAndForward(
    FirmwareSystem& sys, const runtime::RuntimeHealthFact& fact) {
    const bool changed = sys.runtimeHealth.Apply(fact);
    const runtime::RuntimeHealthSnapshot snapshot =
        sys.runtimeHealth.Snapshot();
    if (changed) {
        LogRuntimeHealthRevision(snapshot);
    }
    if (!sys.factoryBootOnly) sys.gamePersistence.SetRuntimeHealthSnapshot(snapshot);
    return changed;
}

uint32_t ReadVsCpuLevel(const modules::SettingsStore& settings) {
    const uint32_t v = settings.ReadValue("VS_CPU_LEVEL", static_cast<uint32_t>(0U));
    return (v > 11U) ? 11U : v;
}

uint32_t ClampVsCpuLevel(const int value) {
    if (value <= 0) {
        return 0U;
    }
    if (value >= 11) {
        return 11U;
    }
    return static_cast<uint32_t>(value);
}

void WriteVsCpuLevel(modules::SettingsStore& settings, const uint32_t level) {
    settings.Doc()["VS_CPU_LEVEL"] = level;
    settings.MarkDirty();
}

uint32_t ReadQuickBrightness(const modules::SettingsStore& settings) {
    return modules::white_squares_setting::ReadBrightness0To8(settings);
}

bool ReadWhiteSquaresOn(const modules::SettingsStore& settings) {
    return modules::white_squares_setting::ReadWhiteSquaresOn(settings);
}

void WriteWhiteSquaresOn(modules::SettingsStore& settings, const bool whiteSquaresOn) {
    modules::white_squares_setting::WriteWhiteSquaresOn(settings, whiteSquaresOn);
}

uint8_t ComputeMenuValue01_255ForQuickSettings(const uint8_t optionIndex, const int value) {
    auto clamp255 = [](const int v) -> uint8_t {
        return static_cast<uint8_t>((v < 0) ? 0 : ((v > 255) ? 255 : v));
    };

    switch (optionIndex) {
        case modules::menu_option_indices::VS_CPU: {
            int clamped = value;
            if (clamped < 0) {
                clamped = 0;
            }
            if (clamped > 11) {
                clamped = 11;
            }
            return clamp255((clamped * 255 + 5) / 11);
        }
        case modules::menu_option_indices::BRIGHTNESS: {
            int clamped = value;
            if (clamped < 0) {
                clamped = 0;
            }
            if (clamped > 8) {
                clamped = 8;
            }
            return clamp255((clamped * 255 + 4) / 8);
        }
        case modules::menu_option_indices::FLIP_SIDES:
            return value != 0 ? 255U : 0U;
        case modules::menu_option_indices::GAME_FIRST_PLAYER:
            return value == 2 ? 255U : 0U;
        case modules::menu_option_indices::TIMER: {
            int clamped = value;
            if (clamped < 0) {
                clamped = 0;
            }
            if (clamped > 6) {
                clamped = 6;
            }
            return clamp255((clamped * 255 + 3) / 6);
        }
        default:
            return 0U;
    }
}

const char* UserButtonToString(const modules::UserButton b) {
    switch (b) {
        case modules::UserButton::Up:
            return "Up";
        case modules::UserButton::Down:
            return "Down";
        case modules::UserButton::Left:
            return "Left";
        case modules::UserButton::Right:
            return "Right";
        case modules::UserButton::P1X:
            return "P1X";
        case modules::UserButton::P2X:
            return "P2X";
        case modules::UserButton::P1O:
            return "P1O";
        case modules::UserButton::P2O:
            return "P2O";
        case modules::UserButton::None:
            return "None";
        default:
            return "?";
    }
}

const char* ButtonEventToString(const fxe::Button::Event e) {
    switch (e) {
        case fxe::Button::Event::Press:
            return "Pressed";
        case fxe::Button::Event::Release:
            return "Released";
        case fxe::Button::Event::Repeat:
            return "Repeat";
        case fxe::Button::Event::LongHold:
            return "LongHold";
        default:
            return "?";
    }
}

ToggleGate EvaluateFlipSidesToggleGate(const firmware_internal::FirmwareSystem& sys) {
    ToggleGate out;

    // Rotate/player-end gating policy:
    // - Only blocked when the game is not at the starting position.
    // - UX feedback for this block is handled by flashing all pieces.
    //
    // Prefer engine FEN so this works even before persistence has written GAME_FEN.
    bool fenOk = false;
    const char* authoritativeFen = sys.gamePersistence.GetAuthoritativeFen();
    if (authoritativeFen != nullptr && authoritativeFen[0] != '\0') {
        fenOk = modules::IsAnyInitialFen(authoritativeFen);
    }

    if (!fenOk) {
        const auto& doc = sys.settings.Doc();
        if (!doc.containsKey(modules::KEY_GAME_FEN)) {
            // No persisted state yet: treat as starting position (history gate still applies).
            fenOk = true;
        } else {
            const char* persistedFen = sys.settings.ReadValue(modules::KEY_GAME_FEN, "");
            if (persistedFen && persistedFen[0] != '\0') {
                fenOk = modules::IsAnyInitialFen(persistedFen);
            }
        }
    }

    out.allowed = fenOk;
    out.reason = out.allowed ? modules::MenuToggleBlockReason::None : modules::MenuToggleBlockReason::Position;
    return out;
}

ToggleGate EvaluateFirstPlayerToggleGate(const firmware_internal::FirmwareSystem& sys) {
    // For now, the player-end toggle is gated identically to flip-sides:
    // both are only safe/allowed at the starting position and when the system
    // is otherwise idle.
    return EvaluateFlipSidesToggleGate(sys);
}

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
) {
    if (configured) {
        return;
    }

    ioCadence.periodMs = 5;
    engineCadence.periodMs = 20;
    settingsCadence.periodMs = 20;
    bleCadence.periodMs = 20;
    sensorCadence.periodMs = 5;
    ledCadence.periodMs = 20;
#ifdef LITCH_TRACE
    traceCadence.periodMs = 1000;
#endif

    configured = true;
}

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
) {
    ioCadence.EnsureInitialized();
    engineCadence.EnsureInitialized();
    settingsCadence.EnsureInitialized();
    bleCadence.EnsureInitialized();
    sensorCadence.EnsureInitialized();
    ledCadence.EnsureInitialized();
#ifdef LITCH_TRACE
    traceCadence.EnsureInitialized();
#endif
}

void PollSensorsAndDispatch() {
    auto& ctx = GetSystemContext();
    auto& sys = GetSystem();
    runtime::InteractionDispatcher dispatcher(
        sys.stateMachine,
        sys.gamePersistence,
        sys.appEventQueue);
    const uint32_t preChangeCount = sys.board.GetSensorChangeCount();

    const bool boardRotate = GetFlipSidesSetting(*ctx.settings);
    const bool firstPlayerIsP2 = firmware_internal::GetFirstPlayerIsP2Setting(*ctx.settings);
    const bool squareRotate = modules::ComputeSquareRotate(
        boardRotate, firstPlayerIsP2, sys.gamePersistence.Litchcraft().IsActive());
    sys.board.SetFlipSides(squareRotate);

    {
    const auto& artwork = sys.gamePersistence.Litchcraft();
    const bool craftOwnsInput = artwork.IsActive() || artwork.IsBootArtworkPlaying();
    const auto access = artwork.IsActive() ? modules::LitchcraftSession::PresentationAccess::Frame
                                          : modules::LitchcraftSession::PresentationAccess::Control;
    if (craftOwnsInput && !BeginPhysicalLitchcraftPresentation(sys, access)) return;
    modules::GamePersistenceCoordinator::LitchcraftPresentationGuard presentation(
        craftOwnsInput ? &sys.gamePersistence : nullptr);
    if (sys.boardInput.SupportsAtomicObservations()) {
        const modules::BoardScanAttemptResult attempt =
            sys.board.CaptureScanAttempt();
        const uint16_t healthRevisionBefore =
            sys.runtimeHealth.Snapshot().revision;
        const modules::BoardInputHealthDecision decision =
            sys.boardInputHealth.ObserveScanAttempt(fxe::Millis(), attempt);
        if (decision.snapshot.revision != healthRevisionBefore) {
            LogRuntimeHealthRevision(decision.snapshot);
        }
        if (!sys.factoryBootOnly) sys.gamePersistence.SetRuntimeHealthSnapshot(decision.snapshot);
        if (attempt.status == modules::BoardScanAttemptStatus::Succeeded) {
            (void)sys.gamePersistence.AuthorizeRestoredClockResumeAfterBoot(
                true, decision.snapshot);
        }
        if (decision.dispatchObservation) {
            const auto mode = decision.authoritativeResync || !sys.startupBoardSnapshotObserved
                                  ? hardware::ChessboardHardware::
                                        ObservationDispatchMode::Always
                                  : hardware::ChessboardHardware::
                                        ObservationDispatchMode::ChangedOnly;
#ifdef LITCH_TRACE
            const bool accepted = sys.board.AcceptAndDispatchScan(
                attempt, dispatcher, mode, &sys.recorder);
#else
            const bool accepted = sys.board.AcceptAndDispatchScan(
                attempt, dispatcher, mode);
#endif
            if (decision.authoritativeResync) {
                sys.boardInputHealth.AcknowledgeAuthoritativeResync(accepted);
            }
            // Even an unchanged successful scan is fresh. ChangedOnly retains
            // its ordinary ingress semantics while resignation checks this
            // admitted physical sample after buttons and board dispatch.
            if (attempt.status == modules::BoardScanAttemptStatus::Succeeded) {
                uint64_t logicalMask = 0ULL;
                for (uint8_t raw = 0U; raw < 64U; ++raw) {
                    if ((attempt.rawOccupiedMask & (UINT64_C(1) << raw)) != 0ULL)
                        logicalMask |= UINT64_C(1) << runtime::MapSquareIndex(raw, squareRotate);
                }
                if (accepted) {
                    sys.startupBoardSnapshotObserved = true;
                    sys.bootDisplay.ObserveBoard(
                        !sys.gamePersistence.Litchcraft().IsInitialStoreModePending(), logicalMask,
                        !sys.gamePersistence.Litchcraft().IsActive());
                    if (sys.bootDisplay.WasCancelled()) CancelBootPresentation(sys);
                }
                sys.gamePersistence.ObserveFreshAcceptedBoardScan(logicalMask);
            }
        }
    } else {
#ifdef LITCH_TRACE
        sys.board.PollAndDispatch(dispatcher, &sys.recorder);
#else
        sys.board.PollAndDispatch(dispatcher);
#endif
    }

    }

#if defined(LITCH_BLE)
    // Publish the reducer-accepted logical occupancy, never an uncertain raw
    // scan. Exact repeats are silent in the retained physical-input key.
    sys.ble.PublishConnectionPhysicalInputState(
        sys.stateMachine.GetPhysicalOccupancyMask());
#endif

    if (sys.board.GetSensorChangeCount() != preChangeCount) {
        sys.sleep.NoteHallSensorActivity();
    }

    if (sys.runtimeHardware.resolved.hasLightSensors) {
        const hardware::AmbientLightUpdateResult ambientAttempt =
            sys.ambientLights.UpdateAttempt();
        if (ambientAttempt.attemptCompleted) {
            runtime::RuntimeHealthFact fact;
            fact.tMs = fxe::Millis();
            fact.kind = runtime::RuntimeHealthFactKind::AmbientSensorCount;
            fact.ambientSensorCount = ambientAttempt.validSensorCount;
            (void)ApplyRuntimeHealthFactAndForward(sys, fact);
        }
        if (ambientAttempt.sampleCaptured) {
            sys.ambientLux.OnLuxSample(fxe::Millis(), sys.ambientLights.GetLastLux(), true);
        }
    }

    PollWaterSloshMotion(sys);
    if (sys.hallMapActive && hardware::HW_HAS_EXPANDER_INTERRUPTS) {
        const uint32_t updateId = sys.inputScan.GetUpdateCounter();
        if (updateId != sys.hallMapLastUpdate) {
            sys.hallMapLastUpdate = updateId;

            uint16_t raw[hardware::PCA9555_EXPANDER_COUNT] = {};
            sys.inputScan.CopyRawValues(raw);

            if (ctx.console) {
                ctx.console->PrintLn("hallmap seq=%lu raw0=0x%04x raw1=0x%04x raw2=0x%04x raw3=0x%04x",
                                     static_cast<unsigned long>(sys.hallMapSeq++),
                                     static_cast<unsigned>(raw[0]),
                                     static_cast<unsigned>(raw[1]),
                                     static_cast<unsigned>(raw[2]),
                                     static_cast<unsigned>(raw[3]));
            }
        }
    }

    static uint32_t lastI2cErrorCount = 0;
    const uint32_t nowI2cErrorCount = sys.boardInput.GetI2cErrorCount();
    if (nowI2cErrorCount >= lastI2cErrorCount) {
        const uint32_t delta = nowI2cErrorCount - lastI2cErrorCount;
        if (delta != 0) {
            ctx.telemetry->OnI2cErrors(delta);
            lastI2cErrorCount = nowI2cErrorCount;
        }
    } else {
        lastI2cErrorCount = nowI2cErrorCount;
    }
}

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
) {
    uint32_t nextMs = ioCadence.RemainingMs();

    const uint32_t engineRemaining = engineCadence.RemainingMs();
    if (engineRemaining < nextMs) {
        nextMs = engineRemaining;
    }
    const uint32_t settingsRemaining = settingsCadence.RemainingMs();
    if (settingsRemaining < nextMs) {
        nextMs = settingsRemaining;
    }
    const uint32_t bleRemaining = bleCadence.RemainingMs();
    if (bleRemaining < nextMs) {
        nextMs = bleRemaining;
    }
    const uint32_t sensorRemaining = sensorCadence.RemainingMs();
    if (sensorRemaining < nextMs) {
        nextMs = sensorRemaining;
    }
    const uint32_t ledRemaining = ledCadence.RemainingMs();
    if (ledRemaining < nextMs) {
        nextMs = ledRemaining;
    }
#ifdef LITCH_TRACE
    const uint32_t traceRemaining = traceCadence.RemainingMs();
    if (traceRemaining < nextMs) {
        nextMs = traceRemaining;
    }
#endif

    return nextMs;
}

void SleepUntilNextWork(const uint32_t remainingMs) {
    const auto& sys = firmware_internal::GetSystem();
    const bool cpuThinking = sys.stateMachine.IsCpuThinkingActive();
    const uint32_t maxSleepMs = cpuThinking ? 20 : 10;
    const uint32_t sleepMs = sys.ledRuntimeAdmission.ConstrainNextWakeMs(
        ClampSleepMs(remainingMs, maxSleepMs), fxe::Millis());
    delay((sleepMs == 0) ? 1 : sleepMs);
}

} // namespace firmware_internal

#endif
