#ifdef ARDUINO

#include <esp_system.h>

#include "firmware/firmware_internal.hpp"

#include "firmware/power_runtime_health.hpp"

#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/led_power_envelope.hpp"

namespace firmware_internal {

namespace {

constexpr uint32_t USB_CC_FULL_SAMPLE_INTERVAL_MS = 1000U;

void LogStrongSourceTransition(
    const FirmwareSystem& sys,
    const modules::StrongSourceConfirmationTransition transition) {
    logger::Trace(
        "ledpower: >=1.5A Rp confirmation %s raw1=%u raw2=%u advertised=%s",
        transition == modules::StrongSourceConfirmationTransition::Gained
            ? "gained" : "lost",
        static_cast<unsigned>(sys.lastUsbCc1),
        static_cast<unsigned>(sys.lastUsbCc2),
        hardware::UsbCcLevelToString(sys.lastUsbCcAdvertisedLevel));
}

struct UsbCcMonitorReading {
    uint16_t raw = 0U;
    hardware::UsbCcActivePin activePin = hardware::UsbCcActivePin::None;
};

UsbCcMonitorReading ReadActiveUsbCcMonitorSample(
    const FirmwareSystem& sys) {
    UsbCcMonitorReading reading;
    reading.activePin = sys.usbCcSourcePolicy.activePin;
    if (reading.activePin == hardware::UsbCcActivePin::None) {
        return reading;
    }

    const uint8_t pin =
        (reading.activePin == hardware::UsbCcActivePin::Cc1)
            ? hardware::PIN_USB_CC1
            : hardware::PIN_USB_CC2;
    reading.raw = hardware::ReadUsbCcMonitorMedian(pin);
    return reading;
}

hardware::UsbCcLevel ClassifyUsbCcMonitorReading(
    const UsbCcMonitorReading& reading,
    const hardware::UsbCcThresholds& thresholds) {
    return reading.activePin != hardware::UsbCcActivePin::None
               ? hardware::ClassifyUsbCcRaw(reading.raw, thresholds)
               : hardware::UsbCcLevel::Unknown;
}

void BlankLedOutputForBlockingOperation(FirmwareSystem& sys) {
    if (sys.ledOutputInitialized) {
        firmware_internal::ClearLedOutput(sys);
    }
    sys.ledPowerStats = modules::BudgetAwareLedPipelineStats{};
    sys.ledPowerLimitMa = GetActiveLedCurrentLimitMa(sys);
}

bool UsbCcThresholdsEqual(
    const hardware::UsbCcThresholds& lhs,
    const hardware::UsbCcThresholds& rhs) {
    return lhs.level500 == rhs.level500 &&
           lhs.level1500 == rhs.level1500 &&
           lhs.level3000 == rhs.level3000;
}

bool ObserveUsbCcThresholdPolicy(
    FirmwareSystem& sys,
    const hardware::UsbCcThresholds& thresholds) {
    if (!sys.usbCcThresholdPolicyValid) {
        sys.usbCcThresholdPolicy = thresholds;
        sys.usbCcThresholdPolicyValid = true;
        return false;
    }
    if (UsbCcThresholdsEqual(sys.usbCcThresholdPolicy, thresholds)) {
        return false;
    }

    sys.usbCcThresholdPolicy = thresholds;
    // A deferred or failed save still needs the persistence guard. An
    // immediate save that already succeeded has no pending write to guard;
    // re-arming here would make an unrelated later save restart confirmation.
    (void)sys.settings.ArmPersistenceGuardIfSavePending();
    ResetUsbCcSourceAuthorization(sys);
    return true;
}

void ApplyUsbCcPolicyUpdate(
    FirmwareSystem& sys,
    const modules::UsbCcSourcePolicyUpdate& update,
    const uint32_t nowMs) {
    sys.usbCcSourcePolicy = update.state;
    if (update.transition ==
        modules::StrongSourceConfirmationTransition::None) {
        return;
    }

    sys.lastUsbCcMonitorSampleMs = nowMs;
    LogStrongSourceTransition(sys, update.transition);
    if (update.transition ==
        modules::StrongSourceConfirmationTransition::Gained) {
        sys.lastUsbCcMonitorRaw = 0U;
    }
    (void)firmware::ClearPowerProtectionForConfirmedSource(
        update.transition, IsLedBrownoutLimitLatched(sys.settings),
        !sys.settings.IsDisabled(), nowMs,
        [&sys]() { (void)SetLedBrownoutLimitLatched(sys.settings, false); },
        [&sys](const runtime::RuntimeHealthFact& fact) {
            (void)ApplyRuntimeHealthFactAndForward(sys, fact);
        });

    runtime::RuntimeHealthFact fact;
    if (firmware::MakePowerSourceRuntimeHealthFact(
            update.transition, nowMs, fact)) {
        (void)ApplyRuntimeHealthFactAndForward(sys, fact);
    }
}

void PublishPowerEnvelope(FirmwareSystem& sys,
                                    const uint32_t nowMs) {
    const bool protectionLatched = IsLedBrownoutLimitLatched(sys.settings);
    const modules::LedPowerEnvelope envelope = modules::SelectS3LedPowerEnvelope(
        protectionLatched);
    runtime::RuntimeHealthFact fact;
    if (firmware::MakePowerEnvelopeRuntimeHealthFact(
            hardware::HW_IS_S3,
            !sys.settings.IsDisabled(),
            protectionLatched,
            envelope,
            nowMs,
            fact)) {
        (void)ApplyRuntimeHealthFactAndForward(sys, fact);
    }
}

uint16_t RawForActiveUsbCcPin(const FirmwareSystem& sys) {
    if (sys.usbCcSourcePolicy.activePin == hardware::UsbCcActivePin::Cc1) {
        return sys.lastUsbCc1;
    }
    if (sys.usbCcSourcePolicy.activePin == hardware::UsbCcActivePin::Cc2) {
        return sys.lastUsbCc2;
    }
    return 0U;
}

void ApplyFullSourceSample(
    FirmwareSystem& sys,
    const hardware::UsbCcThresholds& thresholds,
    const hardware::UsbCcReadings& readings) {
    const uint32_t sampleCompletedMs = fxe::Millis();
    sys.lastUsbCc1 = readings.cc1;
    sys.lastUsbCc2 = readings.cc2;
    sys.lastUsbCcAdvertisedLevel = hardware::ClassifyUsbCcReadings(
        readings.cc1, readings.cc2, thresholds);
    const modules::UsbCcSourcePolicyUpdate update =
        modules::UpdateUsbCcSourceFromFullSample(
            sys.usbCcSourcePolicy,
            readings.cc1,
            readings.cc2,
            thresholds,
            sampleCompletedMs);
    ApplyUsbCcPolicyUpdate(sys, update, sampleCompletedMs);
    sys.lastUsbCcMonitorSampleMs = sampleCompletedMs;
    sys.lastUsbCcMonitorRaw = RawForActiveUsbCcPin(sys);
    sys.lastUsbCcSampleMs = sampleCompletedMs;
    sys.usbCcSampleValid = true;
}

} // namespace

void RefreshLedPowerSourceStatus(
    FirmwareSystem& sys,
    const uint32_t nowMs,
    const bool forceSample) {
    if (!hardware::HW_IS_S3) {
        sys.lastUsbCc1 = 0;
        sys.lastUsbCc2 = 0;
        sys.lastUsbCcAdvertisedLevel = hardware::UsbCcLevel::Unknown;
        sys.lastUsbCcSampleMs = nowMs;
        sys.lastUsbCcMonitorSampleMs = nowMs;
        sys.lastUsbCcMonitorRaw = 0U;
        sys.usbCcSampleValid = false;
        sys.usbCcSourcePolicy = modules::UsbCcSourcePolicyState{};
        sys.usbCcThresholdPolicyValid = false;
        return;
    }

    const bool confirmationActive =
        modules::ShouldContinueAutomaticStrongSourceSampling(
            sys.usbCcSourcePolicy.confirmation, sys.usbCcSampleValid);
    const bool fullSampleDue = forceSample || sys.usbCcSampler.IsActive() ||
                               !sys.usbCcSampleValid ||
                               modules::IsUsbCcFullPairRecheckDue(
                                   sys.usbCcSourcePolicy, nowMs) ||
                               (confirmationActive &&
                                (nowMs - sys.lastUsbCcSampleMs) >=
                                    USB_CC_FULL_SAMPLE_INTERVAL_MS);
    if (!fullSampleDue) {
        return;
    }

    const hardware::UsbCcThresholds thresholds =
        LoadUsbCcThresholds(sys.settings);
    (void)ObserveUsbCcThresholdPolicy(sys, thresholds);
    hardware::UsbCcReadings readings;
    if (forceSample) {
        // Explicit protection-clear and diagnostic validation already blanks
        // output and may retain the synchronous full-pair boundary.
        sys.usbCcSampler.Cancel();
        readings = hardware::ReadUsbCcPins();
    } else {
        if (!sys.usbCcSampler.IsActive()) sys.usbCcSampler.Begin(nowMs);
        const bool completed = sys.usbCcSampler.Update(
            nowMs,
            [](const uint8_t pin) {
                return static_cast<uint16_t>(analogRead(
                    pin == 0U ? hardware::PIN_USB_CC1
                              : hardware::PIN_USB_CC2));
            },
            readings);
        if (!completed) return;
    }
    ApplyFullSourceSample(sys, thresholds, readings);
    sys.ledPowerLimitMa = GetActiveLedCurrentLimitMa(sys);
    PublishPowerEnvelope(sys, sys.lastUsbCcSampleMs);
}

void ResetUsbCcSourceAuthorization(FirmwareSystem& sys) {
    if (!hardware::HW_IS_S3) {
        return;
    }

    sys.usbCcSourcePolicy = modules::ResetUsbCcSourcePolicy(
        sys.usbCcSourcePolicy);
    sys.usbCcSampler.Cancel();
    sys.ledSourceResetLimitMa = modules::S3_CONSERVATIVE_TOTAL_CURRENT_MA;
    ++sys.ledSourceResetEpoch;
    if (!sys.ledAnimation.IsStarted()) sys.sourceOutputLimit.Reset(
        modules::S3_CONSERVATIVE_TOTAL_CURRENT_MA, fxe::Millis());
    sys.lastUsbCcMonitorSampleMs = 0U;
    sys.lastUsbCcMonitorRaw = 0U;
    sys.usbCcSampleValid = false;
    // Callers use this before operations that can pause cooperative CC
    // monitoring. Blank even when raw diagnostics bypassed authorization.
    BlankLedOutputForBlockingOperation(sys);
}

void PollLedPowerSourceStatus(FirmwareSystem& sys, const uint32_t nowMs) {
    if (!hardware::HW_IS_S3) {
        return;
    }

    RefreshLedPowerSourceStatus(sys, nowMs, false);

    const hardware::UsbCcThresholds thresholds =
        LoadUsbCcThresholds(sys.settings);
    if (ObserveUsbCcThresholdPolicy(sys, thresholds)) {
        return;
    }

    const bool normalEnvelopeActive =
        !IsLedBrownoutLimitLatched(sys.settings) &&
        modules::IsStrongSourceConfirmed(
            sys.usbCcSourcePolicy.confirmation);
    if (!normalEnvelopeActive) {
        return;
    }

    if (sys.usbCcSourcePolicy.fullPairRecheckPending) {
        // The scheduled refresh above advances a complete pair. Partial
        // evidence is never published or used to reset loss debounce.
        return;
    }

    if (!modules::IsUsbCcActivePinMonitorDue(
            sys.lastUsbCcMonitorSampleMs, nowMs)) {
        return;
    }

    const UsbCcMonitorReading reading =
        ReadActiveUsbCcMonitorSample(sys);
    const uint32_t sampleCompletedMs = fxe::Millis();
    sys.lastUsbCcMonitorSampleMs = sampleCompletedMs;
    sys.lastUsbCcMonitorRaw = reading.raw;
    const modules::UsbCcSourcePolicyUpdate update =
        modules::UpdateUsbCcSourceFromActivePinSample(
            sys.usbCcSourcePolicy,
            reading.raw,
            thresholds,
            sampleCompletedMs);
    if (update.transition ==
        modules::StrongSourceConfirmationTransition::Lost) {
        if (reading.activePin == hardware::UsbCcActivePin::None) {
            sys.lastUsbCcAdvertisedLevel = hardware::UsbCcLevel::Unknown;
        } else {
            uint16_t& selectedRaw =
                (reading.activePin == hardware::UsbCcActivePin::Cc1)
                    ? sys.lastUsbCc1
                    : sys.lastUsbCc2;
            selectedRaw = reading.raw;
            sys.lastUsbCcAdvertisedLevel =
                ClassifyUsbCcMonitorReading(reading, thresholds);
        }
    }
    ApplyUsbCcPolicyUpdate(sys, update, sampleCompletedMs);
    PublishPowerEnvelope(sys, sampleCompletedMs);
}

void InitializeLedPowerBudgetAfterSettingsLoad(FirmwareSystem& sys) {
    sys.ledPowerStats = modules::BudgetAwareLedPipelineStats{};
    sys.ledPowerLimitMa = 0U;
    sys.lastUsbCc1 = 0;
    sys.lastUsbCc2 = 0;
    sys.lastUsbCcSampleMs = 0;
    sys.lastUsbCcMonitorSampleMs = 0;
    sys.lastUsbCcMonitorRaw = 0U;
    sys.usbCcSampleValid = false;
    sys.usbCcSourcePolicy = modules::UsbCcSourcePolicyState{};
    sys.usbCcSampler.Cancel();
    sys.ledSourceResetLimitMa = modules::S3_CONSERVATIVE_TOTAL_CURRENT_MA;
    ++sys.ledSourceResetEpoch;
    if (!sys.ledAnimation.IsStarted()) sys.sourceOutputLimit.Reset(
        modules::S3_CONSERVATIVE_TOTAL_CURRENT_MA, fxe::Millis());
    sys.usbCcThresholdPolicy = LoadUsbCcThresholds(sys.settings);
    sys.usbCcThresholdPolicyValid = true;
    sys.lastUsbCcAdvertisedLevel = hardware::UsbCcLevel::Unknown;

    const runtime::BootGuard::Snapshot snapshot = sys.bootGuard.GetSnapshot();
    firmware::RestorePowerProtectionAfterBoot(
        snapshot.lastResetReason == static_cast<uint32_t>(ESP_RST_BROWNOUT),
        IsLedBrownoutLimitLatched(sys.settings), fxe::Millis(),
        [&sys]() {
            (void)SetLedBrownoutLimitLatched(sys.settings, true);
            logger::Warn("ledpower: brownout reset detected; nominal 500mA protection budget latched");
        },
        [&sys](const runtime::RuntimeHealthFact& fact) {
            (void)ApplyRuntimeHealthFactAndForward(sys, fact);
        });

    // Normal startup attempts the full budget through the existing upward ramp.
    // Only a confirmed or retained brownout latch selects protection.
    sys.ledPowerLimitMa = GetActiveLedCurrentLimitMa(sys);
}

bool ClearLedPowerProtectionLatch(FirmwareSystem& sys, const uint32_t nowMs) {
    if (sys.settings.IsDisabled()) {
        return false;
    }
    return firmware::RunValidatedPowerProtectionClear(
        hardware::HW_IS_S3,
        [&sys, nowMs]() {
            ResetUsbCcSourceAuthorization(sys);
            RefreshLedPowerSourceStatus(sys, nowMs, true);
            // A fresh complete pair validates the existing explicit action;
            // unknown/default CC does not prevent a full normal-budget retry.
            return sys.usbCcSampleValid;
        },
        [&sys]() {
            // Recompute before and after changing persisted recovery state.
            sys.ledPowerLimitMa = GetActiveLedCurrentLimitMa(sys);
        },
        [&sys]() {
            (void)SetLedBrownoutLimitLatched(sys.settings, false);
        },
        [&sys]() {
            runtime::RuntimeHealthFact fact;
            fact.tMs = fxe::Millis();
            fact.kind =
                runtime::RuntimeHealthFactKind::ResetLatchedProtection;
            (void)ApplyRuntimeHealthFactAndForward(sys, fact);
        });
}

} // namespace firmware_internal

#endif
