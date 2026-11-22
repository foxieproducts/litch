#ifdef ARDUINO

#include "firmware/firmware_app_internal.hpp"

#include "logger.hpp"
#include "modules/hardware_test_record.hpp"

namespace firmware_internal {
namespace {

using SaveOutcome = firmware::SettingsPersistenceWorker::SaveOutcome;
// FirmwareSystem is the single runtime owner. Keep the one manufacturing
// request alive across Stop/Enter so accepted storage work is reconciled.
struct PendingHardwareTestSave {
    firmware::SettingsPersistenceWorker::SaveReceipt receipt = 0U;
    modules::HardwareTestRecord record{};
    modules::EncodedHardwareTestRecord original;
    uint32_t runId = 0U;
    bool activeRun = false;
    bool clearFactoryRecord = false;
    bool hasVerifierIntent = false;
    bool verifierEnabled = false;
};
PendingHardwareTestSave pendingSave;

bool ReadHardwareTestRecord(FirmwareSystem& sys, modules::HardwareTestRecord& out) {
    return modules::DecodeHardwareTestRecord(sys.settings.ReadValue(
        modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""), out);
}

void UpdateVerifierEndpointAfterSave(FirmwareSystem& sys,
                                     const modules::HardwareTestRecord& record) {
    if (!sys.factoryVerifierBoot || !modules::IsFactoryVerifierEligible(record)) {
        sys.ble.SetFactoryVerifierConfiguredEnabled(false);
    } else if (!sys.ble.EnableFactoryVerifierEndpoint(sys.settings)) {
        logger::Error("hwtest: saved verifier endpoint activation failed; reboot required");
    }
}

bool StartHardwareTestSave(FirmwareSystem& sys, const modules::HardwareTestRecord& record) {
    if (pendingSave.receipt != 0U || !pendingSave.original.Assign(sys.settings.ReadValue(
            modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""))) {
        return false;
    }
    // The worker takes a bounded serialized copy before returning. Until its
    // receipt says Saved, the source document continues to expose the last
    // known record; a timeout cannot roll it back over an in-flight write.
    modules::EncodedHardwareTestRecord encoded;
    if (!modules::EncodeHardwareTestRecord(record, encoded)) return false;
    sys.factoryProvisioning.ExpectFactoryResult(record);
    if (!sys.settingsPersistence.EnqueueTracked(sys.settings.Doc(), pendingSave.receipt, nullptr,
            sys.factoryProvisioning.ExpectedReceiptPublication(), nullptr, nullptr,
            modules::HARDWARE_TEST_RECORD_SETTING_KEY, encoded.CStr())) {
        sys.factoryProvisioning.ForgetExpectedResult();
        logger::Error("hwtest: record save was not accepted; result is not durable");
        return false;
    }
    pendingSave.record = record;
    pendingSave.runId = sys.hardwareTest.GetRunId();
    pendingSave.activeRun = sys.hardwareTest.IsActive();
    logger::Info("hwtest: saving record; result remains pending until storage completes");
    return true;
}

void ReconcileHardwareTestSave(FirmwareSystem& sys, const uint32_t nowMs) {
    if (pendingSave.receipt == 0U) { return; }
    const auto outcome = sys.settingsPersistence.GetSaveOutcome(pendingSave.receipt);
    if (outcome == SaveOutcome::Pending || outcome == SaveOutcome::Unknown) { return; }

    const bool sameRun = !pendingSave.clearFactoryRecord && pendingSave.runId == sys.hardwareTest.GetRunId() &&
        (sys.hardwareTest.IsActive() || sys.hardwareTest.IsOverridePending()) &&
        pendingSave.record.radioResult == sys.hardwareTest.GetRequestedRadioResult();
    const bool saved = outcome == SaveOutcome::Saved;
    const bool unchangedRecord = pendingSave.original.View() == sys.settings.ReadValue(
        modules::HARDWARE_TEST_RECORD_SETTING_KEY, "");
    auto liveRecord = pendingSave.record;
    if (pendingSave.hasVerifierIntent) {
        liveRecord.factoryVerifierEnabled = pendingSave.verifierEnabled;
    }
    const bool intentNeedsSave = liveRecord.factoryVerifierEnabled !=
        pendingSave.record.factoryVerifierEnabled;
    const bool adopted = saved && unchangedRecord && (pendingSave.clearFactoryRecord
        ? (sys.settings.Doc().remove(modules::HARDWARE_TEST_RECORD_SETTING_KEY), true)
        : modules::WriteHardwareTestRecord(sys.settings.Doc(), liveRecord));
    const bool provisioningPassed =
        sys.factoryProvisioning.AllowsCompletedHardwareTestResult();
    if (adopted) {
        if (intentNeedsSave) { sys.settings.MarkMustSaveDirty(); }
        else { sys.settings.NotifyDocChanged(); }
        if (pendingSave.clearFactoryRecord) {
            sys.ble.SetFactoryVerifierConfiguredEnabled(false);
        } else if (liveRecord.radioResult == modules::ManufacturingTestRadioResult::Override &&
                   sys.factoryProvisioning.State() == modules::FactoryProvisioningState::Override) {
            sys.ble.SetFactoryVerifierConfiguredEnabled(false);
        } else if (provisioningPassed) {
            UpdateVerifierEndpointAfterSave(sys, liveRecord);
        } else {
            sys.ble.SetFactoryVerifierConfiguredEnabled(false);
            logger::Error(
                "hwtest: record saved but NVS provisioning promotion failed; PASS is suppressed");
        }
    } else if (saved && !unchangedRecord) {
        // A later explicit record mutation (for example verifier off) owns RAM.
        // Retain that intent and let the existing deferred settings owner save it.
        sys.settings.MarkMustSaveDirty();
    }
    if (sameRun) {
        const bool complete = adopted && (provisioningPassed ||
            (pendingSave.record.radioResult == modules::ManufacturingTestRadioResult::Override &&
             sys.factoryProvisioning.State() == modules::FactoryProvisioningState::Override));
        sys.hardwareTest.ReportPersistenceResult(complete, pendingSave.record.radioResult);
        if (complete && sys.factoryBootOnly && !sys.factoryVerifierBoot) {
            if (liveRecord.factoryVerifierEnabled &&
                liveRecord.radioResult == modules::ManufacturingTestRadioResult::PeerVerified &&
                sys.hardwareTest.RestoreSavedVerifier(liveRecord, nowMs)) {
                // The exact originating responder has now saved genuine proof
                // and NVS Passed. Reuse the inactive saved-service owner in this
                // minimal runtime; an election or a late receipt cannot enter it.
                sys.factoryVerifierBoot = true;
                sys.factoryCompletion.Cancel();
                sys.factoryRadioReleasedForSave = false;
                UpdateVerifierEndpointAfterSave(sys, liveRecord);
                logger::Info("hwtest: durable verifier retained; serving next factory board");
            } else {
                sys.factoryCompletion.Begin();
            }
        }
        if (complete && pendingSave.record.radioResult == modules::ManufacturingTestRadioResult::PeerVerified) {
            sys.hardwareTest.ReportVerifierProofComplete(nowMs);
        }
    }
    if (pendingSave.clearFactoryRecord && !adopted &&
        sys.pendingFactoryBootIntent == firmware::FactoryBootIntent::Test) {
        sys.pendingFactoryBootIntent = firmware::FactoryBootIntent::None;
        sys.rebootPersistencePending = false;
    }
    if (pendingSave.clearFactoryRecord) {
        logger::Info("factory clear: %s; provisioning_state=%u; user settings kept",
            adopted ? "Required durable; factory record removed" : "failed; factory record retained",
            static_cast<unsigned>(sys.factoryProvisioning.State()));
    }
    logger::Info("hwtest: storage completed saved=%u live_record_adopted=%u originating_run_active=%u",
                 static_cast<unsigned>(saved), static_cast<unsigned>(adopted), static_cast<unsigned>(sameRun));
    if (!sys.settingsPersistence.ReleaseSaveReceipt(pendingSave.receipt)) { return; }
    sys.factoryProvisioning.ForgetExpectedResult();
    pendingSave = {};
}

} // namespace

bool PrepareFactoryPersistence(FirmwareSystem& sys) {
    if (!sys.factoryBootOnly) return sys.settingsPersistence.IsStarted();
    if (sys.factoryRadio.HasPendingTerminalReply() || !sys.ble.QuiesceFactoryBleDut()) return false;
    if (!sys.factoryRadioReleasedForSave) {
        sys.ble.SetFactoryVerifierConfiguredEnabled(false);
        sys.factoryRadio.Update(false, false, modules::ManufacturingTestStage::Inactive,
            sys.ble, fxe::Millis());
        sys.factoryRadioReleasedForSave = true;
    }
    if (sys.settingsPersistence.IsStarted()) return true;
    if (sys.factoryPersistenceStartFailed) return false;
    const bool started = sys.settingsPersistence.BindFlashArbiter(sys.rmtFlashArbiter) &&
        sys.settingsPersistence.Begin(sys.settings);
    sys.factoryPersistenceStartFailed = !started;
    return started;
}

bool FactoryOtaBeginBusy(void* context) {
    auto& sys = *static_cast<FirmwareSystem*>(context);
    if (IsHardwareTestSavePending() || sys.factoryCompletion.IsPending() ||
        sys.rebootPersistencePending || sys.factoryRadio.HasPendingTerminalReply() ||
        !sys.ble.QuiesceFactoryBleDut()) return true;
    // An OTA request abandons unsaved qualification; a rejected/aborted update
    // cannot reuse partial proof. Existing hwtest restart enters a fresh boot.
    sys.hardwareTest.Stop();
    ReleaseHardwareTestInput(sys);
    sys.ble.SetFactoryVerifierConfiguredEnabled(false);
    sys.factoryRadio.Update(false, false, modules::ManufacturingTestStage::Inactive,
        sys.ble, fxe::Millis());
    sys.factoryRadioReleasedForSave = true;
    return false;
}

void PollFactoryCompletionReboot(FirmwareSystem& sys) {
    const bool operationBusy = sys.ble.IsOtaInProgress() || IsHardwareTestSavePending() ||
        !sys.settingsPersistence.IsIdle() || sys.factoryRadio.HasPendingTerminalReply();
    const bool otaHealthy = !sys.otaBootAwaitingHealth || sys.ble.IsOtaRuntimeAccepted();
    if (!sys.factoryCompletion.IsReady(operationBusy, otaHealthy)) return;
    sys.factoryCompletion.Cancel();
    ArmFactoryBootIntent(firmware::FactoryBootIntent::None);
    sys.bootGuard.MarkBootHealthy();
    logger::Info("hwtest: durable result acknowledged; rebooting to normal play");
    ESP.restart();
}

bool IsHardwareTestSavePending() {
    return pendingSave.receipt != 0U;
}

bool IsFactoryClearPending() {
    return pendingSave.receipt != 0U && pendingSave.clearFactoryRecord;
}

void ReleaseHardwareTestInput(FirmwareSystem& sys) {
    sys.resetHoldController.Reset(); sys.hintButtonController.Reset();
    sys.quickSettingsOverlayController.Reset();
    sys.gpioUserInput.SuppressHeldControlsUntilRelease();
    sys.ble.ResetConnectionButtonState();
    sys.buttonInputQueue.Clear();
}

bool RequestFactoryClear(FirmwareSystem& sys) {
    if (pendingSave.receipt != 0U || sys.hardwareTest.IsOverridePending() ||
        !hardware::SupportsProductionHardwareTest(sys.runtimeHardware.detectedRevision)) return false;
    if (!PrepareFactoryPersistence(sys)) return false;
    if (!pendingSave.original.Assign(sys.settings.ReadValue(modules::HARDWARE_TEST_RECORD_SETTING_KEY, ""))) return false;
    if (!sys.settingsPersistence.EnqueueTracked(sys.settings.Doc(), pendingSave.receipt, nullptr, nullptr,
            [](void* context) {
                return static_cast<FirmwareSystem*>(context)->factoryProvisioning.RequireFactoryTest();
            }, &sys, modules::HARDWARE_TEST_RECORD_SETTING_KEY)) return false;
    pendingSave.clearFactoryRecord = true;
    sys.hardwareTest.Stop();
    ReleaseHardwareTestInput(sys);
    sys.ble.SetFactoryVerifierConfiguredEnabled(false);
    logger::Info("factory clear: pending Required commit then narrow record removal; user settings kept");
    return true;
}

void NoteHardwareTestVerifierIntent(const bool enabled) {
    if (pendingSave.receipt != 0U && !pendingSave.clearFactoryRecord) {
        // An accepted command can be newer than the save without changing
        // bytes: off with no previous record, or an already-off record.
        pendingSave.hasVerifierIntent = true;
        pendingSave.verifierEnabled = enabled;
    }
}

void UpdateHardwareTestPersistence(FirmwareSystem& sys) {
    ReconcileHardwareTestSave(sys, fxe::Millis());
    if (pendingSave.receipt != 0U || (!sys.hardwareTest.IsActive() && !sys.hardwareTest.IsOverridePending()) ||
        sys.hardwareTest.GetStage() != modules::ManufacturingTestStage::Persisting) {
        return;
    }
    if (sys.factoryBootOnly) {
        sys.factoryRadio.PollTerminalReply(sys.ble, fxe::Millis());
        if (!PrepareFactoryPersistence(sys)) {
            if (sys.factoryPersistenceStartFailed) sys.hardwareTest.ReportPersistenceResult(false);
            return;
        }
    }
    modules::HardwareTestRecord record;
    const bool built = sys.hardwareTest.BuildPendingRecord(record);
    if (sys.factoryBootOnly) {
        record.factoryVerifierEnabled = built &&
            record.radioResult == modules::ManufacturingTestRadioResult::PeerVerified &&
            sys.hardwareTest.IsAutomaticVerifier();
    }
    if (!built || !StartHardwareTestSave(sys, record)) {
        sys.hardwareTest.ReportPersistenceResult(false);
    }
}

void UpdateHardwareTestRadio(FirmwareSystem& sys, const uint32_t nowMs) {
    // This runs even after hwtest stop, unlike the active-test sensor cadence.
    UpdateHardwareTestPersistence(sys);
    if (sys.comptest.IsActive()) {
        sys.ble.SetFactoryVerifierConfiguredEnabled(false);
        sys.ble.SetFactoryVerifierRuntimeAllowed(false);
        sys.factoryRadio.Update(false, false, modules::ManufacturingTestStage::Inactive, sys.ble, nowMs);
        return;
    }
    const bool hardwareTestSupported =
        hardware::SupportsProductionHardwareTest(
            sys.runtimeHardware.detectedRevision);
    if (sys.hardwareTest.TakeCompletedBatchExit()) {
        NoteHardwareTestVerifierIntent(false);
        modules::HardwareTestRecord completed;
        if (ReadHardwareTestRecord(sys, completed) && modules::IsFactoryVerifierEligible(completed)) {
            completed.factoryVerifierEnabled = false;
            if (modules::WriteHardwareTestRecord(sys.settings.Doc(), completed)) sys.settings.MarkMustSaveDirty();
        }
        sys.ble.SetFactoryVerifierConfiguredEnabled(false);
    }
    const bool inactiveVerifierAllowed = sys.factoryVerifierBoot && hardwareTestSupported && !pendingSave.clearFactoryRecord &&
        sys.factoryProvisioning.AllowsCompletedHardwareTestResult();
    const bool forced = sys.hardwareTest.IsOverride();
    if (sys.factoryRadio.HasPendingTerminalReply() &&
        (forced || !sys.hardwareTest.IsActive())) {
        sys.factoryRadio.PollTerminalReply(sys.ble, nowMs);
        return;
    }
    if (!hardwareTestSupported || forced) {
        sys.ble.SetFactoryVerifierConfiguredEnabled(false);
    } else if (!sys.hardwareTest.IsActive()) {
        modules::HardwareTestRecord saved;
        sys.ble.SetFactoryVerifierConfiguredEnabled(
            inactiveVerifierAllowed && ReadHardwareTestRecord(sys, saved) &&
            modules::IsFactoryVerifierEligible(saved));
    }
    const auto stage = sys.hardwareTest.GetStage();
    const bool locallyReady = modules::IsFactoryRadioStageReady(stage);
    if (sys.factoryRadioReleasedForSave) return;
    sys.factoryRadio.Update(sys.hardwareTest.IsActive(),
        hardwareTestSupported && !forced && (locallyReady || inactiveVerifierAllowed),
        sys.hardwareTest.GetStage(), sys.ble, nowMs, sys.hardwareTest.GetRunId());
    if (sys.hardwareTest.IsActive()) {
        sys.hardwareTest.SetAutomaticVerifier(sys.factoryRadio.IsAutomaticVerifier(), nowMs);
    }
    sys.hardwareTest.SetVerifierBleProofActive(sys.factoryRadio.IsVerifierBleProofActive(), nowMs);
    if (sys.factoryRadio.ConsumeDutBleVerified()) {
        sys.hardwareTest.ReportRadioVerified(modules::FactoryRadioChannel::Ble, nowMs);
    }
    if (sys.factoryRadio.ConsumeVerifierBleVerified()) {
        if (sys.hardwareTest.IsActive()) {
            sys.hardwareTest.ReportRadioVerified(modules::FactoryRadioChannel::Ble, nowMs);
            modules::HardwareTestRecord record;
            if (sys.hardwareTest.GetStage() == modules::ManufacturingTestStage::Passed &&
                ReadHardwareTestRecord(sys, record) &&
                record.radioResult == modules::ManufacturingTestRadioResult::PeerVerified) {
                sys.hardwareTest.ReportVerifierProofComplete(nowMs);
            }
        } else if (pendingSave.receipt == 0U && inactiveVerifierAllowed) {
            sys.hardwareTest.ReportVerifierProofComplete(nowMs);

        }
    }
}

} // namespace firmware_internal

#endif
