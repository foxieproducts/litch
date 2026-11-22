#pragma once

#include <cstdint>

#include "hardware/usb_cc_sense.hpp"
#include "modules/led_power_envelope.hpp"

namespace modules {

constexpr uint32_t S3_CC_CONFIRMATION_INTERVAL_MS = 1000U;
constexpr uint32_t S3_CC_CONFIRMATION_MAX_GAP_MS = 1500U;

struct UsbCcSourcePolicyState {
    StrongSourceConfirmationState confirmation;
    StrongSourceMonitorState monitor;
    hardware::UsbCcActivePin activePin = hardware::UsbCcActivePin::None;
    uint32_t lastConfirmationSampleMs = 0U;
    bool fullPairRecheckPending = false;
};

struct UsbCcSourcePolicyUpdate {
    UsbCcSourcePolicyState state;
    StrongSourceConfirmationTransition transition =
        StrongSourceConfirmationTransition::None;
};

inline bool ShouldEvaluateUsbCcConfirmationSample(
    const UsbCcSourcePolicyState& state,
    const bool advertisesAtLeast1500Ma,
    const bool meetsEntryThreshold,
    const uint32_t nowMs) {
    if (!advertisesAtLeast1500Ma || !meetsEntryThreshold ||
        IsStrongSourceConfirmed(state.confirmation)) {
        return true;
    }
    return state.confirmation.samples == 0U ||
           (nowMs - state.lastConfirmationSampleMs) >=
               S3_CC_CONFIRMATION_INTERVAL_MS;
}

inline UsbCcSourcePolicyUpdate LoseUsbCcSourceAuthorization(
    UsbCcSourcePolicyState state) {
    const StrongSourceConfirmationUpdate confirmationUpdate =
        UpdateStrongSourceConfirmationState(
            state.confirmation, false, false);
    state.confirmation = confirmationUpdate.state;
    state.monitor = StrongSourceMonitorState{};
    state.fullPairRecheckPending = false;
    if (confirmationUpdate.transition ==
        StrongSourceConfirmationTransition::Lost) {
        state.activePin = hardware::UsbCcActivePin::None;
    }
    return UsbCcSourcePolicyUpdate{
        state, confirmationUpdate.transition};
}

inline UsbCcSourcePolicyState TrackUnconfirmedUsbCcActivePin(
    UsbCcSourcePolicyState state,
    const uint16_t cc1,
    const uint16_t cc2,
    const hardware::UsbCcThresholds& thresholds) {
    const hardware::UsbCcActivePin detectedPin =
        hardware::SelectUsbCcActivePin(cc1, cc2, thresholds);
    const hardware::UsbCcActivePinTrackingUpdate pinUpdate =
        hardware::UpdateUsbCcActivePinTracking(
            state.activePin, false, detectedPin);
    if (pinUpdate.confirmationMustRestart) {
        state.confirmation =
            RevokeStrongSourceConfirmation(state.confirmation);
    }
    state.activePin = pinUpdate.activePin;
    return state;
}

inline UsbCcSourcePolicyUpdate ApplyUsbCcEntryEvidence(
    UsbCcSourcePolicyState state,
    const uint16_t cc1,
    const uint16_t cc2,
    const hardware::UsbCcThresholds& thresholds,
    const uint32_t nowMs) {
    const hardware::UsbCcLevel advertised =
        hardware::ClassifyUsbCcReadings(cc1, cc2, thresholds);
    const bool advertisesAtLeast1500Ma =
        hardware::UsbCcLevelAdvertisesAtLeast1500Ma(advertised);
    const bool meetsEntryThreshold =
        hardware::UsbCcReadingsMeetStrongConfirmationThreshold(
            cc1, cc2, thresholds);
    // Stale partial evidence cannot authorize a source after a long pause.
    // Confirmed sources retain their separate frozen-pin monitor/debounce.
    if (state.confirmation.samples > 0U &&
        !IsStrongSourceConfirmed(state.confirmation) &&
        (nowMs - state.lastConfirmationSampleMs) > S3_CC_CONFIRMATION_MAX_GAP_MS) {
        state.confirmation = RevokeStrongSourceConfirmation(state.confirmation);
    }
    if (!ShouldEvaluateUsbCcConfirmationSample(
            state,
            advertisesAtLeast1500Ma,
            meetsEntryThreshold,
            nowMs)) {
        return UsbCcSourcePolicyUpdate{state};
    }

    const StrongSourceConfirmationUpdate confirmationUpdate =
        UpdateStrongSourceConfirmationState(
            state.confirmation,
            advertisesAtLeast1500Ma,
            meetsEntryThreshold);
    state.confirmation = confirmationUpdate.state;
    state.lastConfirmationSampleMs = nowMs;
    if (confirmationUpdate.transition !=
        StrongSourceConfirmationTransition::None) {
        state.monitor = StrongSourceMonitorState{};
        state.fullPairRecheckPending = false;
    }
    return UsbCcSourcePolicyUpdate{
        state, confirmationUpdate.transition};
}

inline UsbCcSourcePolicyUpdate UpdateUnconfirmedUsbCcSourceFromFullSample(
    UsbCcSourcePolicyState state,
    const uint16_t cc1,
    const uint16_t cc2,
    const hardware::UsbCcThresholds& thresholds,
    const uint32_t nowMs) {
    state = TrackUnconfirmedUsbCcActivePin(
        state, cc1, cc2, thresholds);
    return ApplyUsbCcEntryEvidence(
        state, cc1, cc2, thresholds, nowMs);
}

inline UsbCcSourcePolicyUpdate UpdateAuthorizedUsbCcSourceFromFullSample(
    UsbCcSourcePolicyState state,
    const uint16_t cc1,
    const uint16_t cc2,
    const hardware::UsbCcThresholds& thresholds,
    const uint32_t nowMs) {
    const hardware::UsbCcActivePin detectedPin =
        hardware::SelectUsbCcActivePin(cc1, cc2, thresholds);
    const hardware::UsbCcActivePinTrackingUpdate pinUpdate =
        hardware::UpdateUsbCcActivePinTracking(
            state.activePin, true, detectedPin);
    const hardware::UsbCcLevel advertised =
        hardware::ClassifyUsbCcReadings(cc1, cc2, thresholds);
    const bool sourceStillStrong =
        pinUpdate.observationMatchesAuthorizedPin &&
        hardware::UsbCcLevelAdvertisesAtLeast1500Ma(advertised);
    if (sourceStillStrong) {
        state.monitor = StrongSourceMonitorState{};
        state.fullPairRecheckPending = false;
        return UsbCcSourcePolicyUpdate{state};
    }

    state.fullPairRecheckPending =
        !pinUpdate.observationMatchesAuthorizedPin;
    const StrongSourceMonitorUpdate monitorUpdate =
        UpdateStrongSourceMonitorState(state.monitor, false, nowMs);
    state.monitor = monitorUpdate.state;
    return monitorUpdate.downgradeStable
               ? LoseUsbCcSourceAuthorization(state)
               : UsbCcSourcePolicyUpdate{state};
}

inline UsbCcSourcePolicyUpdate UpdateUsbCcSourceFromFullSample(
    const UsbCcSourcePolicyState& current,
    const uint16_t cc1,
    const uint16_t cc2,
    const hardware::UsbCcThresholds& thresholds,
    const uint32_t nowMs) {
    if (IsStrongSourceConfirmed(current.confirmation)) {
        return UpdateAuthorizedUsbCcSourceFromFullSample(
            current, cc1, cc2, thresholds, nowMs);
    }
    return UpdateUnconfirmedUsbCcSourceFromFullSample(
        current, cc1, cc2, thresholds, nowMs);
}

inline UsbCcSourcePolicyUpdate UpdateUsbCcSourceFromActivePinSample(
    UsbCcSourcePolicyState state,
    const uint16_t activePinRaw,
    const hardware::UsbCcThresholds& thresholds,
    const uint32_t nowMs) {
    if (!IsStrongSourceConfirmed(state.confirmation) ||
        state.fullPairRecheckPending) {
        return UsbCcSourcePolicyUpdate{state};
    }

    const hardware::UsbCcLevel advertised =
        state.activePin == hardware::UsbCcActivePin::None
            ? hardware::UsbCcLevel::Unknown
            : hardware::ClassifyUsbCcRaw(activePinRaw, thresholds);
    const StrongSourceMonitorUpdate monitorUpdate =
        UpdateStrongSourceMonitorState(
            state.monitor,
            hardware::UsbCcLevelAdvertisesAtLeast1500Ma(advertised),
            nowMs);
    state.monitor = monitorUpdate.state;
    return monitorUpdate.downgradeStable
               ? LoseUsbCcSourceAuthorization(state)
               : UsbCcSourcePolicyUpdate{state};
}

inline bool IsUsbCcFullPairRecheckDue(
    const UsbCcSourcePolicyState& state,
    const uint32_t nowMs) {
    return IsStrongSourceFullPairRecheckDue(
        state.monitor, state.fullPairRecheckPending, nowMs);
}

inline bool IsUsbCcActivePinMonitorDue(
    const uint32_t lastSampleMs,
    const uint32_t nowMs) {
    return (nowMs - lastSampleMs) >= S3_CC_MONITOR_INTERVAL_MS;
}

inline UsbCcSourcePolicyState ResetUsbCcSourcePolicy(
    UsbCcSourcePolicyState state) {
    state.confirmation = StrongSourceConfirmationState{};
    state.monitor = StrongSourceMonitorState{};
    state.activePin = hardware::UsbCcActivePin::None;
    state.lastConfirmationSampleMs = 0U;
    state.fullPairRecheckPending = false;
    return state;
}

} // namespace modules
