#ifdef ARDUINO

#include <array>

#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_app_internal.hpp"

#include "firmware/firmware_internal.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/factory_provisioning_policy.hpp"
#include "runtime/progress_watchdog.hpp"
#include "modules/hardware_test_record.hpp"

namespace firmware_internal::console_commands {

namespace {

void StopHardwareTestAndReleaseInput(FirmwareSystem& sys) {
    sys.hardwareTest.Stop();
    sys.factoryCompletion.Cancel();
    ReleaseHardwareTestInput(sys);
}


const char* StageName(const modules::ManufacturingTestStage stage) {
    switch (stage) {
        case modules::ManufacturingTestStage::OverrideSaved: return "override_saved";
        case modules::ManufacturingTestStage::Inactive:
            return "inactive";
        case modules::ManufacturingTestStage::Local:
            return "local";
        case modules::ManufacturingTestStage::Radio:
            return "radio";
        case modules::ManufacturingTestStage::Persisting:
            return "persisting";
        case modules::ManufacturingTestStage::Passed:
            return "passed";
        case modules::ManufacturingTestStage::PassedRadioBypassed:
            return "local_complete_waiting_for_peer";
        case modules::ManufacturingTestStage::Failed:
            return "failed";
    }
    return "unknown";
}

const char* StepName(const firmware::HardwareTestMode::Step step) {
    switch (step) {
        case firmware::HardwareTestMode::Step::LedRainbow:
            return "led_field";
        case firmware::HardwareTestMode::Step::LedInspect:
            return "led_fault_inspection";
        case firmware::HardwareTestMode::Step::Buttons:
            return "physical_buttons";
        case firmware::HardwareTestMode::Step::HallSensors:
            return "hall_sweep";
        case firmware::HardwareTestMode::Step::LightSensors:
            return "light_sensor_response";
        case firmware::HardwareTestMode::Step::WhiteChannel:
            return "neutral_led_scan";
        case firmware::HardwareTestMode::Step::MotionSensor:
            return "motion_sensor_presence";
        case firmware::HardwareTestMode::Step::ConfirmLeds:
            return "confirm_led_review";
        case firmware::HardwareTestMode::Step::Radio:
            return "peer_radio";
        case firmware::HardwareTestMode::Step::Persisting:
            return "saving_result";
        case firmware::HardwareTestMode::Step::Passed:
            return "pass_saved";
        case firmware::HardwareTestMode::Step::PassedRadioBypassed:
            return "local_complete_waiting_for_peer";
        case firmware::HardwareTestMode::Step::Failed:
            return "failed_current_run";
    }
    return "unknown";
}

const char* CheckState(
    const modules::ManufacturingTestProgress& progress,
    const modules::ManufacturingTestLocalCheck check) {
    const uint8_t bit = static_cast<uint8_t>(
        1U << static_cast<uint8_t>(check));
    if ((progress.automatedChecksRequiredMask & bit) == 0U) {
        return "n/a";
    }
    return (progress.automatedChecksPassedMask & bit) != 0U
        ? "pass"
        : "pending";
}

const char* ResultState(const bool passed) {
    return passed ? "pass" : "pending";
}

const char* ProvisioningStateName(
    const modules::FactoryProvisioningState state) {
    switch (state) {
        case modules::FactoryProvisioningState::Override: return "override";
        case modules::FactoryProvisioningState::Required: return "required";
        case modules::FactoryProvisioningState::Absent: return "absent";
        case modules::FactoryProvisioningState::Pending: return "pending";
        case modules::FactoryProvisioningState::Passed: return "passed";
        case modules::FactoryProvisioningState::Invalid: return "invalid";
        case modules::FactoryProvisioningState::Unavailable: return "unavailable";
        case modules::FactoryProvisioningState::LegacyExempt: return "legacy_exempt";
    }
    return "unknown";
}

const char* ProvisioningNextAction(
    const modules::FactoryProvisioningState state) {
    switch (state) {
        case modules::FactoryProvisioningState::Override: return "normal_boot_override_retained";
        case modules::FactoryProvisioningState::Required: return "complete_required_factory_run";
        case modules::FactoryProvisioningState::Passed: return "none";
        case modules::FactoryProvisioningState::LegacyExempt: return "not_required";
        case modules::FactoryProvisioningState::Pending:
            return "complete_full_test_and_save";
        case modules::FactoryProvisioningState::Unavailable:
            return "restore_nvs_then_hwtest_restart";
        case modules::FactoryProvisioningState::Invalid:
        case modules::FactoryProvisioningState::Absent:
            return "factory_reprovision";
    }
    return "inspect";
}

const char* LightSensorState(
    const modules::ManufacturingTestProgress& progress,
    const uint8_t sensorBit) {
    const uint8_t bit = static_cast<uint8_t>(
        1U << static_cast<uint8_t>(
            modules::ManufacturingTestLocalCheck::LightSensors));
    if ((progress.automatedChecksRequiredMask & bit) == 0U) {
        return "n/a";
    }
    if ((progress.lightSensorLinkMask & sensorBit) == 0U) { return "link_pending"; }
    const auto& evidence = progress.lightSensors[sensorBit == 1U ? 0U : 1U];
    switch (evidence.stage) {
        case modules::LightSensorResponseStage::Baseline: return "hold_light_steady";
        case modules::LightSensorResponseStage::Change: return "change_light";
        case modules::LightSensorResponseStage::Passed: return "pass";
    }
    return "pending";
}

const char* MotionEventState(
    const modules::ManufacturingTestProgress& progress) {
    const uint8_t bit = static_cast<uint8_t>(
        1U << static_cast<uint8_t>(
            modules::ManufacturingTestLocalCheck::MotionSensor));
    if ((progress.automatedChecksRequiredMask & bit) == 0U) {
        return "n/a";
    }
    return ResultState(progress.motionEventObserved);
}

bool ReadRecord(FirmwareSystem& sys,
                modules::HardwareTestRecord& out) {
    const char* encoded = sys.settings.ReadValue(
        modules::HARDWARE_TEST_RECORD_SETTING_KEY, "");
    return modules::DecodeHardwareTestRecord(encoded, out);
}

void PrintStatus(Console& console, FirmwareSystem& sys) {
    const modules::ManufacturingTestProgress progress =
        sys.hardwareTest.GetProgress();
    console.PrintLn("hwtest: radio_role=%s channel=%s",
        sys.hardwareTest.IsAutomaticVerifier() ? "VERIFIER" : "DUT/testing",
        std::array<const char*, 4>{"red", "green", "blue", "white"}[
            sys.hardwareTest.GetLedChannel()]);
    console.PrintLn(
        "hwtest: active=%u stage=%s step=%s revision=%u buttons=%u/%u halls_clear=%u/64 halls_active=%u/64",
        static_cast<unsigned>(sys.hardwareTest.IsActive() ? 1U : 0U),
        StageName(sys.hardwareTest.GetStage()),
        StepName(sys.hardwareTest.GetStep()),
        static_cast<unsigned>(sys.runtimeHardware.detectedRevision),
        static_cast<unsigned>(progress.buttonsObserved),
        static_cast<unsigned>(progress.buttonsRequired),
        static_cast<unsigned>(progress.hallClearObserved),
        static_cast<unsigned>(progress.hallActiveObserved));
    console.PrintLn(
        "hwtest: button_mask=0x%04x/0x%04x hall_clear=0x%08lx%08lx hall_active=0x%08lx%08lx",
        static_cast<unsigned>(progress.observedButtonMask),
        static_cast<unsigned>(progress.expectedButtonMask),
        static_cast<unsigned long>(progress.hallClearMask >> 32U),
        static_cast<unsigned long>(
            progress.hallClearMask & 0xFFFFFFFFULL),
        static_cast<unsigned long>(progress.hallActiveMask >> 32U),
        static_cast<unsigned long>(
            progress.hallActiveMask & 0xFFFFFFFFULL));
    console.PrintLn("hwtest: responding_expanders=0x%02x known=%u motion_present=%u known=%u forced_faults=0x%02x current_failed_checks=0x%02x",
        progress.expanderLinkMask, progress.expanderLinksKnown, progress.motionPresent,
        progress.motionPresenceKnown, progress.diagnosticFaultMask, progress.failedAutomatedCheckMask);
    console.PrintLn(
        "hwtest: current_hall_read=%s current_hall_clear=%u",
        progress.hallSnapshotValid ? "valid" : "unavailable",
        static_cast<unsigned>(progress.hallSnapshotValid && progress.hallOccupiedMask == 0ULL));
    for (uint8_t index = 0U; index < 2U; ++index) {
        const auto& light = progress.lightSensors[index];
        console.PrintLn("hwtest: sensor=%u raw_lux=%lu room=%lu covered=%lu recovered=%lu next=%s",
            static_cast<unsigned>(index + 1U), static_cast<unsigned long>(light.lastLux),
            static_cast<unsigned long>(light.roomLux), static_cast<unsigned long>(light.coveredLux),
            static_cast<unsigned long>(light.recoveredLux), LightSensorState(progress, 1U << index));
    }
    console.PrintLn(
        "hwtest: auto_mask=0x%02x/0x%02x auto_under_10s=%u motion_event=%u leds_confirmed=%u radio_mask=0x%02x",
        static_cast<unsigned>(progress.automatedChecksPassedMask),
        static_cast<unsigned>(progress.automatedChecksRequiredMask),
        static_cast<unsigned>(progress.automatedWithinTenSeconds ? 1U : 0U),
        static_cast<unsigned>(progress.motionEventObserved ? 1U : 0U),
        static_cast<unsigned>(progress.ledEmissionConfirmed ? 1U : 0U),
        static_cast<unsigned>(progress.radioChannelMask));
    console.PrintLn(
        "hwtest: checks led_scan=%s board_io=%s light_1=%s light_2=%s motion_link=%s motion_event=%s led_review=%s radio_ble=%s",
        CheckState(progress, modules::ManufacturingTestLocalCheck::LedDrive),
        CheckState(progress, modules::ManufacturingTestLocalCheck::I2cExpanders),
        LightSensorState(progress, 0x01U),
        LightSensorState(progress, 0x02U),
        CheckState(progress, modules::ManufacturingTestLocalCheck::MotionSensor),
        MotionEventState(progress),
        ResultState(progress.ledEmissionConfirmed),
        ResultState((progress.radioChannelMask &
                     modules::FACTORY_RADIO_BLE_CHANNEL) != 0U));

    console.PrintLn("hwtest: radio_proof=%s peer_candidate=%u discovery_deadline_ms=%lu ble_endpoint_error=%u save_pending=%u override_pending=%u",
        progress.radioChannelMask == modules::FACTORY_RADIO_ALL_CHANNELS ? "verified" : "pending_not_broken_radio_proof",
        static_cast<unsigned>(sys.factoryRadio.HasPeerCandidate()),
        static_cast<unsigned long>(sys.factoryRadio.DiscoveryDeadlineMs()),
        static_cast<unsigned>(sys.factoryRadio.BleEndpointStartFailed()),
        static_cast<unsigned>(IsHardwareTestSavePending()), static_cast<unsigned>(sys.hardwareTest.IsOverridePending()));
    const auto retained = runtime::GetProgressSnapshot();
    console.PrintLn("hwtest: retained_radio_phase current=%u previous=%u previous_valid=%u reset_reason=%lu",
        static_cast<unsigned>(retained.current.stages[1]),
        static_cast<unsigned>(retained.previous.stages[1]),
        static_cast<unsigned>(retained.previousValid),
        static_cast<unsigned long>(retained.resetReason));
    const bool lightResponseRequired = (progress.automatedChecksRequiredMask &
        (1U << static_cast<uint8_t>(modules::ManufacturingTestLocalCheck::LightSensors))) != 0U;
    console.PrintLn("hwtest: manual_ambient_response=%s observed_mask=0x%02x manual_motion=not_required observed=%u",
        lightResponseRequired ? "required" : "not_required",
        static_cast<unsigned>(progress.lightSensorResponseMask), static_cast<unsigned>(progress.motionEventObserved));
    const auto provisioningState = sys.factoryProvisioning.State();
    console.PrintLn(
        "hwtest: provisioning_state=%s completed_result=%s next=%s",
        ProvisioningStateName(provisioningState),
        sys.factoryProvisioning.AllowsCompletedHardwareTestResult()
            ? "allowed"
            : "blocked",
        ProvisioningNextAction(provisioningState));

    modules::HardwareTestRecord record;
    if (!ReadRecord(sys, record)) {
        console.PrintLn("hwtest: saved_record=none");
        return;
    }
    console.PrintLn(
        "hwtest: saved_record=valid radio=%u verifier=%u auto_ms=%lu operator_ms=%lu radio_ms=%lu",
        static_cast<unsigned>(record.radioResult),
        static_cast<unsigned>(record.factoryVerifierEnabled ? 1U : 0U),
        static_cast<unsigned long>(record.automatedDurationMs),
        static_cast<unsigned long>(record.operatorDurationMs),
        static_cast<unsigned long>(record.radioDurationMs));
}

bool UpdateVerifierEligibility(FirmwareSystem& sys, const bool enabled) {
    // Clear already stops service and owns these bytes until its receipt. A
    // redundant off agrees with that intent; it must not resurrect the record.
    if (IsFactoryClearPending()) { return false; }
    modules::HardwareTestRecord record;
    if (!ReadRecord(sys, record) || !modules::CanEnableFactoryVerifier(record)) {
        return false;
    }
    if (record.factoryVerifierEnabled == enabled) {
        return true;
    }
    record.factoryVerifierEnabled = enabled;
    if (!modules::WriteHardwareTestRecord(sys.settings.Doc(), record)) {
        return false;
    }
    sys.settings.MarkMustSaveDirty();
    return true;
}

} // namespace

bool HardwareTestCommand(Console& console, const Console::Args& args) {
    auto& sys = GetSystem();
    const std::string_view sub =
        args.Has(1) ? args.Get(1) : std::string_view("status");
    if (sys.comptest.IsActive() && sub != "status") {
        console.PrintLn("hwtest: stop comptest first"); return false;
    }

    if (sub == "fault") {
        if (!args.Has(2) || args.Has(3) || !sys.hardwareTest.SetDiagnosticFault(args.Get(2), fxe::Millis())) {
            console.PrintLn("hwtest: fault rejected; active unsaved run only: motion|light1|light2|expander1|expander2|expander3|expander4|clear");
            return false;
        }
        console.PrintLn("hwtest: RAM-only forced failure changed; clear/exit/new run/reboot removes it; no PASS granted");
        return true;
    }
    if (sub == "status") {
        PrintStatus(console, sys);
        return true;
    }
    if (sub == "start" || sub == "restart") {
        if (sys.hardwareTest.IsOverridePending()) {
            console.PrintLn("hwtest: override is still saving; wait for saved/failure status before restarting");
            return false;
        }
        if (sys.factoryProvisioning.State() == modules::FactoryProvisioningState::Passed) {
            console.PrintLn("hwtest: saved verified PASS protected; factory clear is required before a new factory run");
            return false;
        }
        if (!hardware::SupportsProductionHardwareTest(
                sys.runtimeHardware.detectedRevision)) {
            console.PrintLn(
                "hwtest: unsupported on this hardware revision; ordinary diagnostics remain available");
            return false;
        }
        if (!RequestFactoryBoot(sys, firmware::FactoryBootIntent::Test)) {
            console.PrintLn("hwtest: entry busy or saving unavailable; retry after storage/update completes");
            return false;
        }
        console.PrintLn("hwtest: saving current state; rebooting into factory essentials");
        return true;
    }
    if (sub == "forcepass") {
        console.PrintLn("hwtest: forcepass retired; use forceexit to record an override, never PASS");
        return false;
    }
    if (sub == "forceexit") {
        if (args.Has(2) || !sys.hardwareTest.ForceExit(fxe::Millis())) return false;
        ReleaseHardwareTestInput(sys);
        console.PrintLn("hwtest: override saving in factory mode; normal reboot follows durable completion; no verified PASS granted");
        return true;
    }
    if (sub == "stop") {
        StopHardwareTestAndReleaseInput(sys);
        console.PrintLn("hwtest: stopped; an already accepted save may still finish; status shows the last reconciled record");
        return true;
    }
    if (sub == "confirm" && args.Equals(2, "leds")) {
        if (!sys.hardwareTest.IsActive() ||
            sys.hardwareTest.GetStep() != firmware::HardwareTestMode::Step::LedRainbow ||
            sys.hardwareTest.GetLedChannel() != 3U) {
            console.PrintLn(
                "hwtest: LED confirmation rejected until the guided scan and local checks reach confirmation");
            return false;
        }
        sys.hardwareTest.ConfirmLedEmission(true, fxe::Millis());
        console.PrintLn("hwtest: LED emission confirmed by operator/fixture");
        return true;
    }
    if (sub == "verifier" && args.Has(2) &&
        (args.Equals(2, "on") || args.Equals(2, "off"))) {
        const bool enabled = args.Equals(2, "on");
        if (enabled && !hardware::SupportsProductionHardwareTest(
                           sys.runtimeHardware.detectedRevision)) {
            sys.ble.SetFactoryVerifierConfiguredEnabled(false);
            console.PrintLn(
                "hwtest: verifier rejected on this hardware revision");
            return false;
        }
        if (enabled && (!sys.factoryProvisioning.AllowsCompletedHardwareTestResult() ||
                        IsHardwareTestSavePending())) {
            console.PrintLn("hwtest: verifier change rejected; factory completion or storage is pending");
            return false;
        }
        const bool updated = UpdateVerifierEligibility(sys, enabled);
        if (!enabled) {
            StopHardwareTestAndReleaseInput(sys);
        }
        if (enabled && !updated) {
            console.PrintLn(
                "hwtest: verifier change rejected; requires an eligible saved factory record");
            return false;
        }
        if (!RequestFactoryBoot(sys, enabled ? firmware::FactoryBootIntent::Verifier
                                            : firmware::FactoryBootIntent::None)) {
            console.PrintLn("hwtest: verifier transition busy; no boot request armed");
            return false;
        }
        sys.ble.SetFactoryVerifierConfiguredEnabled(false);
        console.PrintLn("hwtest: saving verifier=%s; clean reboot into %s", enabled ? "on" : "off",
            enabled ? "factory verifier essentials" : "normal play");
        return true;
    }

    console.PrintLn(
        "usage: hwtest [start|restart|status|forceexit|confirm leds|stop|verifier on|off|fault motion|light1|light2|expander1..4|clear]");
    return false;
}

bool FactoryCommand(Console& console, const Console::Args& args) {
    auto& sys = GetSystem();
    if (!args.Equals(1, "clear") || args.Has(2)) { console.PrintLn("usage: factory clear"); return false; }
    if (sys.comptest.IsActive() || sys.ble.IsOtaInProgress()) { console.PrintLn("factory clear: busy; stop the active diagnostic/update first"); return false; }
    const bool accepted = RequestFactoryClear(sys);
    console.PrintLn(accepted ? "factory clear: pending Required commit and factory record removal; inspect status; user settings kept"
                            : "factory clear: not completed; inspect provisioning_state and save_pending before retry");
    return accepted;
}

size_t HardwareTestComplete(const Console::Args& args,
                            const size_t argIndex,
                            const char* prefix,
                            const char** outMatches,
                            const size_t outMatchesCap) {
    size_t count = 0U;
    if (argIndex == 1U) {
        constexpr const char* VALUES[] = {
            "start", "restart", "status", "forceexit", "confirm", "stop", "verifier", "fault"};
        for (const char* value : VALUES) {
            count = AddMatchIfPrefix(
                value, prefix, outMatches, count, outMatchesCap);
        }
        return count;
    }
    if (argIndex == 2U && args.Equals(1, "confirm")) {
        return AddMatchIfPrefix(
            "leds", prefix, outMatches, count, outMatchesCap);
    }
    if (argIndex == 2U && args.Equals(1, "verifier")) {
        count = AddMatchIfPrefix(
            "on", prefix, outMatches, count, outMatchesCap);
        return AddMatchIfPrefix(
            "off", prefix, outMatches, count, outMatchesCap);
    }
    if (argIndex == 2U && args.Equals(1, "fault")) {
        constexpr const char* VALUES[] = {
            "motion", "light1", "light2", "expander1", "expander2", "expander3", "expander4", "clear"};
        for (const char* value : VALUES)
            count = AddMatchIfPrefix(value, prefix, outMatches, count, outMatchesCap);
        return count;
    }
    return 0U;
}

} // namespace firmware_internal::console_commands

#endif
