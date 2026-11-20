#ifdef ARDUINO

#ifndef LITCH_NATIVE
#include <Arduino.h>
#if defined(LITCH_ESP32S3) && (!defined(ARDUINO_USB_MODE) || !ARDUINO_USB_MODE)
#include <esp32-hal-tinyusb.h>
#endif
#endif

#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_internal.hpp"
#include "firmware/firmware_app_internal.hpp"
#include "firmware/litchcraft_persistence_runtime.hpp"
#include "modules/board_storage_recovery.hpp"
#include "modules/board_owner_persistence.hpp"

namespace firmware_internal {
namespace {

constexpr uint32_t SETTINGS_REBOOT_FLUSH_TIMEOUT_MS = 10000U;
constexpr uint32_t USB_ROM_RAIL_SETTLE_MS = 1000U;

void AbortReboot(FirmwareSystem& sys, const char* reason) {
    auto& console = *GetSystemContext().console;
    console.PrintLn("%s aborted: %s", sys.rebootToBootloader ? "reboot bootloader" : "reboot", reason);
    sys.rebootPersistencePending = false;
    sys.rebootToBootloader = false;
    sys.pendingFactoryBootIntent = firmware::FactoryBootIntent::None;
}

void Restart(Console& console, FirmwareSystem& sys, const bool armedRetry) {
    const bool bootloader = sys.rebootToBootloader;
    sys.rebootToBootloader = false;
#if defined(LITCH_ESP32S3) && defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE
    if (bootloader) {
        console.PrintLn("reboot bootloader rejected: native USB entry unavailable");
        return;
    }
#endif
    if (bootloader) {
        if (!QuiesceLedRail(sys)) {
            console.PrintLn("reboot bootloader aborted: LED clear incomplete; rail remains on");
            return;
        }
        delay(USB_ROM_RAIL_SETTLE_MS);
        console.PrintLn("reboot bootloader ready: state saved; entering ROM");
    } else console.PrintLn("rebooting");
#ifdef LITCH_TRACE
    GetTraceRecorder().Stop();
#endif
    if (armedRetry) {
        sys.bootGuard.PrepareForRetryReboot();
    } else {
        // A requested software reset is not an early boot crash. Preserve any
        // recovery latch; only an explicitly armed retry clears it.
        sys.bootGuard.MarkBootHealthy();
    }
    ArmFactoryBootIntent(armedRetry ? firmware::FactoryBootIntent::None
                                  : sys.pendingFactoryBootIntent);
    sys.pendingFactoryBootIntent = firmware::FactoryBootIntent::None;
    delay(50);
#if defined(LITCH_ESP32S3) && (!defined(ARDUINO_USB_MODE) || !ARDUINO_USB_MODE)
    if (bootloader) {
        usb_persist_restart(RESTART_BOOTLOADER);
        // A rejected SDK shutdown registration returns instead of restarting.
        // Never substitute an application reset or another ROM request.
        console.PrintLn("reboot bootloader aborted: SDK ROM entry returned");
        return;
    }
#endif
    ESP.restart();
}

} // namespace

bool RequestSettingsClear(Console& console) {
    auto& sys = GetSystem();
    if (console.CommandInputSource() != fxe::ConsoleInputSource::Remote ||
        console.CommandRemoteInputGeneration() == 0U ||
        console.CommandRemoteInputGeneration() != sys.ble.ConsoleInputGeneration()) {
        console.PrintLn("settings clear: use the authenticated owner BLE Console or physical boot recovery");
        return false;
    }
    if (sys.safeModeActive || sys.factoryBootOnly || sys.settings.IsDisabled() ||
        !sys.settingsPersistence.IsStarted() || !sys.settingsPersistence.IsIdle() ||
        sys.rebootPersistencePending || sys.ble.IsOtaInProgress() ||
        (sys.otaBootAwaitingHealth && !sys.ble.IsOtaRuntimeAccepted()) ||
        sys.hardwareTest.IsActive() || sys.amptest.IsActive() ||
        sys.amptestVerify.IsActive() || sys.comptest.IsActive() ||
        IsHardwareTestSavePending() ||
        sys.sleep.GetState() != firmware::SleepState::Awake ||
        sys.gamePersistence.Litchcraft().IsActive() ||
        sys.gamePersistence.ArtPersistence().FlushState() ==
            modules::LitchcraftPersistence::FlushStatus::Pending ||
        LifecycleStoreSavePending(sys)) {
        console.PrintLn("settings clear: busy or saving unavailable; finish active work or use physical boot recovery");
        return false;
    }
    if (!sys.ble.RequestOwnerSettingsClear()) {
        console.PrintLn("settings clear: current authenticated owner required; no reset accepted");
        return false;
    }
    sys.settingsClearPending = true;
    sys.settingsClearReceipt = 0U;
    sys.settingsClearPrimarySaved = false;
    sys.rebootPersistencePending = true;
    sys.rebootPersistenceStarted = false;
    sys.rebootToBootloader = false;
    sys.pendingFactoryBootIntent = firmware::FactoryBootIntent::None;
    console.PrintLn("settings clear: pending; clearing pairing and user settings, then rebooting");
    return true;
}

bool PollSettingsClear(FirmwareSystem& sys) {
    using Outcome = firmware::SettingsPersistenceWorker::SaveOutcome;
    using Controller = modules::BoardOwnerPairingController;
    auto& console = *GetSystemContext().console;
    // Normal input is frozen by the existing reboot latch. Native disconnects,
    // checked cleanup and owner receipts must still reach their existing owners.
    sys.ble.DrainEventQueue();
    sys.boardOwnerPersistence.Update();
    sys.ble.UpdateOwnerPairing();
    if (sys.settingsClearReceipt == 0U) {
        if (sys.boardOwnerPairing.Custody().GetPhase() == Controller::Phase::Failed ||
            sys.settings.IsDisabled()) {
            sys.settingsClearPending = false;
            sys.boardOwnerPairing.CompleteOwnerSettingsReset(false);
            AbortReboot(sys, "pairing clear failed; user settings retained, restart for recovery");
            return true;
        }
        if (!sys.boardOwnerPairing.Custody().Pairable() ||
            sys.boardOwnerPersistence.DurableOwnerRecord() != modules::BoardOwnerRecord::Absent)
            return true;
        StaticJsonDocument<1024U> safe;
        if (!modules::BuildBoardStorageRecoverySnapshot(sys.settings.Doc(), safe) ||
            !modules::WriteBoardOwner(safe, modules::BoardOwnerRecord::Absent) ||
            !sys.settingsPersistence.EnqueueTracked(safe, sys.settingsClearReceipt)) {
            sys.settingsClearPending = false;
            sys.boardOwnerPairing.CompleteOwnerSettingsReset(false);
            AbortReboot(sys, "pairing cleared but settings reset was not accepted; user settings retained");
        }
        return true;
    }
    const auto outcome = sys.settingsPersistence.GetSaveOutcome(sys.settingsClearReceipt);
    if (outcome == Outcome::Pending || outcome == Outcome::Unknown) return true;
    if (!sys.settingsPersistence.ReleaseSaveReceipt(sys.settingsClearReceipt)) return true;
    sys.settingsClearReceipt = 0U;
    if (outcome == Outcome::Saved && !sys.settingsClearPrimarySaved) {
        // A second real transaction rotates the verified reset primary into
        // .bak. Success requires every normal loader fallback to be reset too.
        sys.settingsClearPrimarySaved = true;
        return true;
    }
    sys.settingsClearPending = false;
    sys.rebootPersistencePending = false;
    if (outcome == Outcome::Saved) {
        sys.boardOwnerPairing.CompleteOwnerSettingsReset(true);
        console.PrintLn("settings clear: settings and pairing cleared; artwork and board safety retained");
        // Never flush the still-live old game over the verified replacement.
        Restart(console, sys, false);
    } else {
        sys.boardOwnerPairing.CompleteOwnerSettingsReset(false);
        AbortReboot(sys, "pairing cleared but settings save failed; reset incomplete, restart for recovery");
    }
    return true;
}

bool RequestFactoryBoot(FirmwareSystem& sys, const firmware::FactoryBootIntent intent) {
    if ((intent != firmware::FactoryBootIntent::None &&
         sys.otaBootAwaitingHealth && !sys.ble.IsOtaRuntimeAccepted()) ||
        sys.safeModeActive || sys.settings.IsDisabled() || sys.rebootPersistencePending ||
        IsHardwareTestSavePending() || sys.hardwareTest.IsOverridePending() ||
        sys.factoryCompletion.IsPending() || sys.ble.IsOtaInProgress() ||
        sys.sleep.GetState() != firmware::SleepState::Awake) return false;
    if (intent == firmware::FactoryBootIntent::Test &&
        sys.factoryProvisioning.State() == modules::FactoryProvisioningState::Override &&
        !RequestFactoryClear(sys)) return false;
    sys.factoryPersistenceStartFailed = false;
    sys.pendingFactoryBootIntent = intent;
    sys.rebootToBootloader = false;
    sys.rebootPersistencePending = true;
    sys.rebootPersistenceStarted = false;
    return true;
}

bool console_commands::RebootCommand(Console& console, const Console::Args& args) {
    auto& sys = GetSystem();
    const bool bootloader = args.Has(1) && args.Equals(1, "bootloader");
    if (bootloader) {
        if (args.argc != 2) {
            console.PrintLn("reboot bootloader rejected: usage: reboot bootloader");
            return false;
        }
#if !defined(LITCH_ESP32S3)
        console.PrintLn("reboot bootloader rejected: unsupported hardware");
        return false;
#elif defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE
        console.PrintLn("reboot bootloader rejected: native USB entry unavailable");
        return false;
#else
        if (sys.settings.IsDisabled() || sys.safeModeActive || sys.factoryBootOnly ||
            !sys.settingsPersistence.IsStarted()) {
            console.PrintLn("reboot bootloader rejected: durable normal runtime unavailable");
            return false;
        }
        if (sys.rebootPersistencePending || IsHardwareTestSavePending() ||
            sys.hardwareTest.IsActive() || sys.hardwareTest.IsOverridePending() ||
            sys.factoryCompletion.IsPending() || sys.comptest.IsActive() ||
            sys.ble.IsOtaInProgress() ||
            (sys.otaBootAwaitingHealth && !sys.ble.IsOtaRuntimeAccepted()) ||
            sys.sleep.GetState() != firmware::SleepState::Awake) {
            console.PrintLn("reboot bootloader rejected: busy");
            return false;
        }
        sys.rebootToBootloader = true;
        sys.rebootPersistencePending = true;
        sys.rebootPersistenceStarted = false;
        console.PrintLn("reboot bootloader pending: saving state");
        return true;
#endif
    }
    if (sys.rebootPersistencePending) {
        console.PrintLn("reboot pending: saving settings and artwork");
        return true;
    }
    if (sys.settings.IsDisabled() && !sys.safeModeActive) {
        // Explicit reboot may discard a RAM-only session, but cannot interrupt
        // an accepted format or another already-running flash transaction.
        if (sys.ble.IsOtaInProgress() ||
            sys.sleep.GetState() != firmware::SleepState::Awake ||
            (sys.settingsPersistence.IsStarted() && !sys.settingsPersistence.IsIdle())) {
            console.PrintLn("reboot busy: wait for the active storage operation");
            return false;
        }
        console.PrintLn("saving unavailable: reboot discards this session's unsaved changes");
        Restart(console, sys, false);
        return true;
    }
    if (sys.factoryBootOnly) {
        if (IsHardwareTestSavePending() || sys.hardwareTest.IsOverridePending() ||
            sys.factoryCompletion.IsPending() || sys.ble.IsOtaInProgress()) {
            console.PrintLn("reboot busy: factory result or update is pending");
            return false;
        }
        if (args.Has(1)) {
            console.PrintLn("factory mode: reboot saves state; nosave is unavailable");
            return false;
        }
        return RequestFactoryBoot(sys, firmware::FactoryBootIntent::None);
    }
    const bool noSave = args.Has(1) && args.Equals(1, "nosave");
    const bool shouldSave = !noSave && !sys.safeModeActive;
    const bool armedRetry =
        sys.safeModeActive && sys.bootGuard.ConsumeRetryRebootArm();
    if (armedRetry) {
        console.PrintLn("recovery: consuming armed retry and skipping persistence writes");
    } else if (sys.safeModeActive) {
        console.PrintLn(
            "recovery: reboot skips persistence writes and preserves recovery; use safemode clear or safemode reboot for retry");
    }
    if (!shouldSave) {
        Restart(console, sys, armedRetry);
        return true;
    }
    if (sys.ble.IsOtaInProgress() ||
        sys.sleep.GetState() != firmware::SleepState::Awake) {
        console.PrintLn("reboot aborted: settings and artwork could not be saved");
        return false;
    }
    sys.rebootPersistencePending = true;
    sys.rebootToBootloader = false;
    sys.rebootPersistenceStarted = false;
    console.PrintLn("reboot pending: saving settings and artwork");
    return true;
}

bool PollPendingReboot(FirmwareSystem& sys) {
    if (!sys.rebootPersistencePending) return false;
    if (sys.settingsClearPending) return PollSettingsClear(sys);
    if (IsHardwareTestSavePending()) {
        // Reboot holds the normal loop, so it must still reconcile the exact
        // factory receipt that can release (or reject) this transition.
        UpdateHardwareTestPersistence(sys);
        if (!sys.rebootPersistencePending || IsHardwareTestSavePending()) return true;
    }
    if (sys.factoryBootOnly) {
        static firmware::SettingsPersistenceWorker::SaveReceipt receipt = 0U;
        if (!sys.rebootPersistenceStarted) {
            if (sys.factoryRadio.HasPendingTerminalReply()) {
                sys.factoryRadio.PollTerminalReply(sys.ble, fxe::Millis());
                return true;
            }
            if (!PrepareFactoryPersistence(sys)) {
                if (sys.factoryPersistenceStartFailed) {
                    sys.rebootPersistencePending = false;
                    sys.pendingFactoryBootIntent = firmware::FactoryBootIntent::None;
                    GetSystemContext().console->PrintLn("reboot aborted: settings worker unavailable; factory mode retained");
                }
                return true;
            }
            sys.hardwareTest.Stop();
            ReleaseHardwareTestInput(sys);
            if (!sys.settingsPersistence.EnqueueTracked(sys.settings.Doc(), receipt)) {
                sys.rebootPersistencePending = false;
                sys.pendingFactoryBootIntent = firmware::FactoryBootIntent::None;
                return true;
            }
            sys.rebootPersistenceStarted = true;
            sys.rebootPersistenceElapsed.Reset();
        }
        using Outcome = firmware::SettingsPersistenceWorker::SaveOutcome;
        const auto outcome = sys.settingsPersistence.GetSaveOutcome(receipt);
        if (outcome == Outcome::Pending || outcome == Outcome::Unknown) return true;
        sys.rebootPersistencePending = false;
        sys.settingsPersistence.ReleaseSaveReceipt(receipt);
        receipt = 0U;
        if (outcome == Outcome::Saved) Restart(*GetSystemContext().console, sys, false);
        else {
            sys.pendingFactoryBootIntent = firmware::FactoryBootIntent::None;
            GetSystemContext().console->PrintLn("reboot aborted: settings save failed; factory mode retained");
        }
        return true;
    }
    if (!sys.rebootPersistenceStarted) {
        // Console can consume several queued lines in one Update. Prepare
        // after that call returns, before the loop admits any further input.
        if (!BeginLifecyclePersistence(sys)) {
            AbortReboot(sys, "settings and artwork could not be saved; retry reboot");
            return true;
        }
        sys.rebootPersistenceElapsed.Reset();
        sys.rebootPersistenceStarted = true;
    }
    // The normal loop holds new logical input, but rendering and the storage
    // worker remain live. Advance the same owner that governs idle/exit saves.
    sys.gamePersistence.UpdateLitchcraft();
    const auto status = sys.gamePersistence.ArtPersistence().FlushState();
    if ((status == modules::LitchcraftPersistence::FlushStatus::Pending ||
         LifecycleStoreSavePending(sys)) &&
        !sys.rebootPersistenceElapsed.HasElapsed(SETTINGS_REBOOT_FLUSH_TIMEOUT_MS)) {
        return true;
    }
    sys.rebootPersistencePending = false;
    auto& console = *GetSystemContext().console;
    if (status == modules::LitchcraftPersistence::FlushStatus::Saved &&
        !LifecycleStoreSavePending(sys)) {
        Restart(console, sys, false);
    } else {
        AbortReboot(sys, "settings and artwork save failed or timed out; retry reboot");
    }
    return true;
}

} // namespace firmware_internal

#endif
