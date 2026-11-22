#include <cmath>
#include <cstdio>

#include "hardware/usb_cc_sense.hpp"
#include "modules/led_power_envelope.hpp"
#include "modules/power_budget_policy.hpp"
#include "modules/usb_cc_source_policy.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

void CheckNear(const float a, const float b, const float eps, const char* msg) {
    const float d = std::fabs(a - b);
    if (d <= eps) {
        return;
    }

    std::printf("[FAIL] %s (a=%.6f b=%.6f d=%.6f eps=%.6f)\n", msg, a, b, d, eps);
    ++g_failures;
}

modules::UsbCcSourcePolicyState ConfirmReportedCc2Source(
    const hardware::UsbCcThresholds& thresholds,
    const uint32_t firstSampleMs = 0U) {
    modules::UsbCcSourcePolicyState state;
    for (uint8_t sample = 0U;
         sample < modules::S3_STRONG_SOURCE_CONFIRMATION_SAMPLES;
         ++sample) {
        const modules::UsbCcSourcePolicyUpdate update =
            modules::UpdateUsbCcSourceFromFullSample(
                state,
                80U,
                651U,
                thresholds,
                firstSampleMs +
                    static_cast<uint32_t>(sample) *
                        modules::S3_CC_CONFIRMATION_INTERVAL_MS);
        state = update.state;
    }
    return state;
}

void TwoSecondSourceEntryJourney() {
    const auto thresholds = hardware::GetDefaultUsbCcThresholds();
    for (const uint32_t start : {0U, UINT32_MAX - 1499U}) {
        for (const uint16_t raw : {620U, 850U}) {
            for (const bool cc1Active : {false, true}) {
                modules::UsbCcSourcePolicyState state;
                auto read = [&](uint32_t elapsed) {
                    const auto update = modules::UpdateUsbCcSourceFromFullSample(
                        state, cc1Active ? raw : 80U, cc1Active ? 80U : raw,
                        thresholds, start + elapsed);
                    state = update.state;
                    return update.transition;
                };
                Check(read(0U) == modules::StrongSourceConfirmationTransition::None &&
                      state.confirmation.samples == 1U,
                      "first guarded 1.5A/3A pair starts entry even at zero/wrapped time");
                for (const uint32_t elapsed : {0U, 1U, 10U, 999U}) {
                    read(elapsed);
                    Check(state.confirmation.samples == 1U &&
                          !modules::IsStrongSourceConfirmed(state.confirmation),
                          "duplicate and rapid diagnostic reads cannot shorten source entry");
                }
                read(1000U);
                Check(state.confirmation.samples == 2U &&
                      !modules::IsStrongSourceConfirmed(state.confirmation),
                      "two spaced strong pairs at one second remain unconfirmed");
                read(1999U);
                Check(!modules::IsStrongSourceConfirmed(state.confirmation) &&
                      modules::SelectS3LedPowerEnvelope(false).maxTotalCurrentMa == 1234U,
                      "normal budget does not wait for strong-source confirmation");
                Check(read(2000U) == modules::StrongSourceConfirmationTransition::Gained &&
                      modules::IsStrongSourceConfirmed(state.confirmation) &&
                      state.activePin == (cc1Active ? hardware::UsbCcActivePin::Cc1
                                                   : hardware::UsbCcActivePin::Cc2),
                      "same-pin guarded source confirms at exactly two seconds");
                Check(modules::SelectS3LedPowerEnvelope(false).maxTotalCurrentMa == 1234U,
                      "source confirmation retains the existing normal envelope");
                auto monitor = modules::UpdateUsbCcSourceFromActivePinSample(
                    state, 580U, thresholds, start + 2010U);
                Check(modules::IsStrongSourceConfirmed(monitor.state.confirmation),
                      "confirmed source retains the existing unguarded 580 threshold");
                monitor = modules::UpdateUsbCcSourceFromActivePinSample(
                    monitor.state, 579U, thresholds, start + 2020U);
                monitor = modules::UpdateUsbCcSourceFromActivePinSample(
                    monitor.state, 579U, thresholds, start + 2039U);
                Check(modules::IsStrongSourceConfirmed(monitor.state.confirmation),
                      "two-second entry preserves pending downgrade through 19ms");
                monitor = modules::UpdateUsbCcSourceFromActivePinSample(
                    monitor.state, 579U, thresholds, start + 2040U);
                Check(monitor.transition == modules::StrongSourceConfirmationTransition::Lost &&
                      monitor.state.confirmation.samples == 0U,
                      "existing 20ms debounce revokes source confirmation");
                state = monitor.state;
                read(3000U); read(4000U); read(5000U);
                Check(modules::IsStrongSourceConfirmed(state.confirmation),
                      "fresh strong readings automatically requalify the source");
                state = modules::ResetUsbCcSourcePolicy(state);
                read(6000U); read(7000U);
                Check(!modules::IsStrongSourceConfirmed(state.confirmation),
                      "source reset requires a fresh two-second confirmation window");
                Check(read(8000U) == modules::StrongSourceConfirmationTransition::Gained,
                      "source reset gains confirmation only with fresh two-second evidence");
                std::printf("[TWO_SECOND_SOURCE_ENTRY] start=%u raw=%u pin=%u gained_elapsed=2000 downgrade_elapsed=20\n",
                            start, raw, cc1Active ? 1U : 2U);
            }
        }
    }

    for (const uint16_t failedRaw : {0U, 320U, 619U, 620U}) {
        modules::UsbCcSourcePolicyState state;
        state = modules::UpdateUsbCcSourceFromFullSample(state, 620U, 80U, thresholds, 0U).state;
        state = modules::UpdateUsbCcSourceFromFullSample(state, 620U, 80U, thresholds, 1000U).state;
        const bool ambiguous = failedRaw == 620U;
        state = modules::UpdateUsbCcSourceFromFullSample(
            state, failedRaw, ambiguous ? 620U : 80U, thresholds, 1500U).state;
        Check(state.confirmation.samples == 0U,
              "unknown, weak, guard-band and ambiguous entry evidence restart qualification");
        state = modules::UpdateUsbCcSourceFromFullSample(state, 620U, 80U, thresholds, 2000U).state;
        state = modules::UpdateUsbCcSourceFromFullSample(state, 620U, 80U, thresholds, 3000U).state;
        state = modules::UpdateUsbCcSourceFromFullSample(state, 620U, 80U, thresholds, 3999U).state;
        Check(!modules::IsStrongSourceConfirmed(state.confirmation),
              "dropout recovery cannot borrow earlier strong evidence");
        const auto recovered = modules::UpdateUsbCcSourceFromFullSample(state, 620U, 80U, thresholds, 4000U);
        Check(recovered.transition == modules::StrongSourceConfirmationTransition::Gained,
              "dropout recovery gains only after a new complete two-second window");
    }
    modules::UsbCcSourcePolicyState changedPin;
    changedPin = modules::UpdateUsbCcSourceFromFullSample(changedPin, 620U, 80U, thresholds, 0U).state;
    changedPin = modules::UpdateUsbCcSourceFromFullSample(changedPin, 620U, 80U, thresholds, 1000U).state;
    changedPin = modules::UpdateUsbCcSourceFromFullSample(changedPin, 80U, 850U, thresholds, 1500U).state;
    Check(changedPin.confirmation.samples == 1U && changedPin.activePin == hardware::UsbCcActivePin::Cc2,
          "changing active pin starts a new source window on the new pin");
    changedPin = modules::UpdateUsbCcSourceFromFullSample(changedPin, 80U, 850U, thresholds, 2500U).state;
    changedPin = modules::UpdateUsbCcSourceFromFullSample(changedPin, 80U, 850U, thresholds, 3499U).state;
    Check(!modules::IsStrongSourceConfirmed(changedPin.confirmation),
          "a new active pin cannot borrow elapsed time from the previous pin");
    Check(modules::UpdateUsbCcSourceFromFullSample(changedPin, 80U, 850U, thresholds, 3500U).transition ==
              modules::StrongSourceConfirmationTransition::Gained,
          "new active pin qualifies after its own two seconds");

    for (const uint32_t start : {0U, UINT32_MAX - 1499U}) {
        modules::UsbCcSourcePolicyState timely;
        timely = modules::UpdateUsbCcSourceFromFullSample(timely, 620U, 80U, thresholds, start).state;
        timely = modules::UpdateUsbCcSourceFromFullSample(timely, 620U, 80U, thresholds, start + 1000U).state;
        Check(modules::UpdateUsbCcSourceFromFullSample(timely, 620U, 80U, thresholds, start + 2500U).transition ==
                  modules::StrongSourceConfirmationTransition::Gained,
              "a 1500ms confirmation gap remains accepted at the exact boundary");
        auto stale = modules::UpdateUsbCcSourceFromFullSample(timely, 620U, 80U, thresholds, start + 2501U);
        Check(stale.state.confirmation.samples == 1U && !modules::IsStrongSourceConfirmed(stale.state.confirmation),
              "a stale 1501ms gap restarts entry rather than accumulating old evidence");
        stale = modules::UpdateUsbCcSourceFromFullSample(stale.state, 620U, 80U, thresholds, start + 3501U);
        stale = modules::UpdateUsbCcSourceFromFullSample(stale.state, 620U, 80U, thresholds, start + 4500U);
        Check(!modules::IsStrongSourceConfirmed(stale.state.confirmation),
              "stale-gap recovery remains pending until two fresh seconds elapse");
        Check(modules::UpdateUsbCcSourceFromFullSample(stale.state, 620U, 80U, thresholds, start + 4501U).transition ==
                  modules::StrongSourceConfirmationTransition::Gained,
              "stale-gap recovery completes with fresh bounded pairs across wrap");
    }
}

} // namespace

int main() {
    std::puts("[POWER_BUDGET_POLICY] start");
    TwoSecondSourceEntryJourney();

    modules::PowerBudgetPolicyConfig cfg;
    cfg.maxLedCurrentMa = 500;
    cfg.maxTotalCurrentMa = 600;
    cfg.allowUsbPower = true;
    cfg.usbReserveCurrentMa = 150;
    cfg.hysteresisMa = 25;

    modules::PowerBudgetPolicy policy(cfg);

    // Under the LED cap: no scaling.
    policy.SetEstimatedLedCurrentMa(400);
    Check(policy.GetAllowedLedCurrentMa() == 400, "allowed follows estimate under cap");
    CheckNear(policy.GetLedBrightnessScale01(), 1.0f, 0.0001f, "scale=1 under cap");

    // Over the LED cap: clamp + scale.
    policy.SetEstimatedLedCurrentMa(800);
    Check(policy.GetAllowedLedCurrentMa() == 500, "allowed clamps to maxLedCurrentMa");
    CheckNear(policy.GetLedBrightnessScale01(), 0.625f, 0.0001f, "scale=max/estimate when limited");

    // Hysteresis: once limiting, small dips shouldn't immediately exit.
    policy.SetEstimatedLedCurrentMa(490);
    Check(policy.GetAllowedLedCurrentMa() == 500, "still limiting slightly under cap (hysteresis)");

    // Exit limiting only when estimate <= (cap - hysteresis).
    policy.SetEstimatedLedCurrentMa(475); // 500 - 25
    Check(policy.GetAllowedLedCurrentMa() == 475, "exit limiting at cap-hysteresis");
    CheckNear(policy.GetLedBrightnessScale01(), 1.0f, 0.0001f, "scale returns to 1 after exiting limit");

    // USB decision: requested + enough headroom enables.
    policy.Reset();
    policy.SetUsbPowerRequested(true);
    policy.SetEstimatedLedCurrentMa(300);
    // enable threshold = maxTotal - hysteresis = 575
    // total = allowed(300) + reserve(150) = 450 => enable
    Check(policy.GetUsbPowerEnabled(), "usb enables when within (maxTotal-hysteresis)");

    // If we go over hard total, USB disables.
    policy.SetEstimatedLedCurrentMa(550); // allowed=500 (LED cap), total=650 > 600 => disable
    Check(!policy.GetUsbPowerEnabled(), "usb disables when exceeding maxTotal");

    // Once disabled, it should not re-enable until we are back under (maxTotal-hysteresis).
    policy.SetEstimatedLedCurrentMa(460); // allowed=460, total=610 > 575 => stay disabled
    Check(!policy.GetUsbPowerEnabled(), "usb stays disabled until maxTotal-hysteresis");

    policy.SetEstimatedLedCurrentMa(420); // allowed=420, total=570 <= 575 => enable
    Check(policy.GetUsbPowerEnabled(), "usb re-enables under maxTotal-hysteresis");

    // A total-current cap must reserve the board's non-LED load before
    // authorizing LED current.
    modules::PowerBudgetPolicyConfig baselineCfg;
    baselineCfg.maxLedCurrentMa = 500;
    baselineCfg.maxTotalCurrentMa = 600;
    baselineCfg.fixedSystemCurrentMa = 234;
    modules::PowerBudgetPolicy baselinePolicy(baselineCfg);
    baselinePolicy.SetEstimatedLedCurrentMa(500);
    Check(baselinePolicy.GetAllowedLedCurrentMa() == 366,
          "fixed system current is subtracted from the total-current cap");
    CheckNear(baselinePolicy.GetLedBrightnessScale01(), 366.0f / 500.0f, 0.0001f,
              "scale honors total-current headroom after fixed load");

    const modules::LedPowerEnvelope weakSource =
        modules::SelectS3LedPowerEnvelope(false);
    Check(weakSource.maxTotalCurrentMa == 1234U,
          "unknown/default-current source attempts the normal total envelope");

    const modules::LedPowerEnvelope latchedStrongSource =
        modules::SelectS3LedPowerEnvelope(true);
    Check(latchedStrongSource.maxTotalCurrentMa == 450U,
          "recovery latch remains conservative on a strong source");

    const modules::LedPowerEnvelope latchedWeakSource =
        modules::SelectS3LedPowerEnvelope(true);
    Check(latchedWeakSource.maxTotalCurrentMa == 450U,
          "recovery latch remains conservative on a default-current source");

    const modules::LedPowerEnvelope strongSource =
        modules::SelectS3LedPowerEnvelope(false);
    Check(strongSource.maxTotalCurrentMa == 1234U,
          "strong source selects measured normal total envelope");
    Check(strongSource.maxLedCurrentMa == 1000U &&
              strongSource.fixedSystemCurrentMa == 234U,
          "normal envelope retains calibrated LED and fixed-load limits");

    modules::StrongSourceConfirmationState strongSourceState;
    for (uint8_t sample = 0U;
         sample < modules::S3_STRONG_SOURCE_CONFIRMATION_SAMPLES - 1U;
         ++sample) {
        strongSourceState = modules::UpdateStrongSourceConfirmationState(
                                strongSourceState, true, true)
                                .state;
    }
    Check(!modules::IsStrongSourceConfirmed(strongSourceState.samples),
          "strong source remains unconfirmed during its qualification window");
    const modules::StrongSourceConfirmationUpdate gained =
        modules::UpdateStrongSourceConfirmationState(
            strongSourceState, true, true);
    strongSourceState = gained.state;
    Check(modules::IsStrongSourceConfirmed(strongSourceState.samples),
          "strong source unlocks only after every confirmation sample");
    Check(gained.transition ==
              modules::StrongSourceConfirmationTransition::Gained,
          "confirmation reducer reports the strong-source transition");

    const modules::StrongSourceConfirmationUpdate held =
        modules::UpdateStrongSourceConfirmationState(
            strongSourceState, true, false);
    Check(modules::IsStrongSourceConfirmed(held.state.samples),
          "confirmed source holds through the entry guard band");

    const modules::StrongSourceConfirmationUpdate lost =
        modules::UpdateStrongSourceConfirmationState(
            held.state, false, false);
    Check(lost.state.samples == 0U,
          "debounced weak evidence revokes confirmation without latching protection");
    Check(lost.transition ==
              modules::StrongSourceConfirmationTransition::Lost,
          "confirmation reducer reports the source-loss transition");

    strongSourceState = lost.state;
    for (uint8_t sample = 0U;
         sample < modules::S3_STRONG_SOURCE_CONFIRMATION_SAMPLES;
         ++sample) {
        strongSourceState = modules::UpdateStrongSourceConfirmationState(
                                strongSourceState, true, true)
                                .state;
    }
    Check(modules::IsStrongSourceConfirmed(strongSourceState.samples),
          "fresh strong evidence can requalify without an explicit action");
    const modules::StrongSourceConfirmationState revokedUnstable =
        modules::RevokeStrongSourceConfirmation(strongSourceState);
    Check(revokedUnstable.samples == 0U,
          "revoking confirmation discards old source samples");

    const modules::StrongSourceConfirmationState revokedConfirmed =
        modules::RevokeStrongSourceConfirmation(gained.state);
    Check(revokedConfirmed.samples == 0U,
          "revoking a healthy source requires a fresh confirmation window");

    Check(modules::ShouldContinueAutomaticStrongSourceSampling(
              modules::StrongSourceConfirmationState{}, false),
          "automatic source sampling starts before the first valid reading");
    Check(modules::ShouldContinueAutomaticStrongSourceSampling(
              modules::StrongSourceConfirmationState{}, true),
          "an unadvertised source remains eligible for later strong-source recovery");
    const modules::StrongSourceConfirmationState entryInProgress{1U};
    Check(modules::ShouldContinueAutomaticStrongSourceSampling(
              entryInProgress, true),
          "a clean partial confirmation keeps automatic sampling active");
    Check(!modules::ShouldContinueAutomaticStrongSourceSampling(
              gained.state, true),
          "automatic source sampling stops once confirmation is gained");
    Check(modules::ShouldContinueAutomaticStrongSourceSampling(
              lost.state, true),
          "automatic source sampling resumes after confirmed-source loss");

    modules::StrongSourceMonitorState monitorState;
    modules::StrongSourceMonitorUpdate monitorUpdate =
        modules::UpdateStrongSourceMonitorState(
            monitorState, false, 100U);
    monitorState = monitorUpdate.state;
    Check(!monitorUpdate.downgradeStable,
          "one weak monitor reading starts debounce without downgrading");
    monitorUpdate = modules::UpdateStrongSourceMonitorState(
        monitorState, false, 119U);
    Check(!monitorUpdate.downgradeStable,
          "weak CC shorter than the debounce window is ignored");
    monitorUpdate = modules::UpdateStrongSourceMonitorState(
        monitorUpdate.state, true, 120U);
    Check(!monitorUpdate.state.downgradeCandidateActive,
          "a strong monitor reading cancels a transient downgrade candidate");
    monitorUpdate = modules::UpdateStrongSourceMonitorState(
        monitorUpdate.state, false, 200U);
    monitorUpdate = modules::UpdateStrongSourceMonitorState(
        monitorUpdate.state, false, 210U);
    Check(!monitorUpdate.downgradeStable,
          "two weak readings at the 10ms cadence remain pending");
    monitorUpdate = modules::UpdateStrongSourceMonitorState(
        monitorUpdate.state, false, 219U);
    Check(!monitorUpdate.downgradeStable,
          "a weak run remains pending one millisecond before debounce");
    monitorUpdate = modules::UpdateStrongSourceMonitorState(
        monitorUpdate.state, false, 220U);
    Check(monitorUpdate.downgradeStable,
          "CC below the retention threshold for 20ms requests downgrade");

    modules::StrongSourceMonitorUpdate alternatingMonitor =
        modules::UpdateStrongSourceMonitorState(
            modules::StrongSourceMonitorState{}, false, 300U);
    alternatingMonitor = modules::UpdateStrongSourceMonitorState(
        alternatingMonitor.state, true, 310U);
    alternatingMonitor = modules::UpdateStrongSourceMonitorState(
        alternatingMonitor.state, false, 320U);
    Check(alternatingMonitor.state.downgradeCandidateActive &&
              alternatingMonitor.state.downgradeCandidateSinceMs == 320U &&
              !alternatingMonitor.downgradeStable,
          "a strong reading fully resets an alternating weak candidate");

    modules::StrongSourceMonitorUpdate wrappedMonitor =
        modules::UpdateStrongSourceMonitorState(
            modules::StrongSourceMonitorState{},
            false,
            UINT32_MAX - 9U);
    wrappedMonitor = modules::UpdateStrongSourceMonitorState(
        wrappedMonitor.state, false, 9U);
    Check(!wrappedMonitor.downgradeStable,
          "monitor debounce remains pending for 19ms across millis wrap");
    wrappedMonitor = modules::UpdateStrongSourceMonitorState(
        wrappedMonitor.state, false, 10U);
    Check(wrappedMonitor.downgradeStable,
          "monitor debounce expires at 20ms across millis wrap");

    const hardware::UsbCcThresholds ccThresholds =
        hardware::GetDefaultUsbCcThresholds();
    modules::UsbCcSourcePolicyState reportedPolicy =
        ConfirmReportedCc2Source(ccThresholds);
    Check(modules::IsStrongSourceConfirmed(reportedPolicy.confirmation) &&
              reportedPolicy.activePin == hardware::UsbCcActivePin::Cc2,
          "reported stable readings confirm and bind the Cc2 orientation");

    modules::UsbCcSourcePolicyUpdate reportedPolicyUpdate =
        modules::UpdateUsbCcSourceFromActivePinSample(
            reportedPolicy, 567U, ccThresholds, 10000U);
    Check(reportedPolicyUpdate.transition ==
              modules::StrongSourceConfirmationTransition::None &&
              reportedPolicyUpdate.state.monitor.downgradeCandidateActive,
          "one weak selected-pin window starts downgrade without deauthorization");
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromActivePinSample(
        reportedPolicyUpdate.state, 651U, ccThresholds, 10010U);
    Check(!reportedPolicyUpdate.state.monitor.downgradeCandidateActive &&
              modules::IsStrongSourceConfirmed(
                  reportedPolicyUpdate.state.confirmation),
          "a recovered selected-pin window cancels a transient downgrade");

    reportedPolicy = reportedPolicyUpdate.state;
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromFullSample(
        reportedPolicy, 567U, 73U, ccThresholds, 11000U);
    Check(reportedPolicyUpdate.state.activePin ==
              hardware::UsbCcActivePin::Cc2 &&
              reportedPolicyUpdate.state.fullPairRecheckPending,
          "an opposite-orientation full read cannot migrate the authorized pin");
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromActivePinSample(
        reportedPolicyUpdate.state, 651U, ccThresholds, 11010U);
    Check(reportedPolicyUpdate.state.fullPairRecheckPending &&
              reportedPolicyUpdate.state.monitor.downgradeCandidateActive,
          "an active-pin sample cannot cancel a full-pair mismatch");
    Check(!modules::IsUsbCcFullPairRecheckDue(
              reportedPolicyUpdate.state, 11019U) &&
              modules::IsUsbCcFullPairRecheckDue(
                  reportedPolicyUpdate.state, 11020U),
          "a full-pair mismatch is rechecked at the 20ms boundary");
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromFullSample(
        reportedPolicyUpdate.state, 80U, 651U, ccThresholds, 11020U);
    Check(!reportedPolicyUpdate.state.fullPairRecheckPending &&
              !reportedPolicyUpdate.state.monitor.downgradeCandidateActive &&
              reportedPolicyUpdate.state.activePin ==
                  hardware::UsbCcActivePin::Cc2,
          "a clean full-pair recheck cancels transient pair ambiguity");

    reportedPolicy = reportedPolicyUpdate.state;
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromActivePinSample(
        reportedPolicy, 567U, ccThresholds, 12000U);
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromActivePinSample(
        reportedPolicyUpdate.state, 567U, ccThresholds, 12010U);
    Check(reportedPolicyUpdate.transition ==
              modules::StrongSourceConfirmationTransition::None,
          "a selected-pin downgrade remains pending at the 10ms cadence");
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromActivePinSample(
        reportedPolicyUpdate.state, 567U, ccThresholds, 12020U);
    Check(reportedPolicyUpdate.transition ==
              modules::StrongSourceConfirmationTransition::Lost &&
              reportedPolicyUpdate.state.confirmation.samples == 0U &&
              reportedPolicyUpdate.state.activePin ==
                  hardware::UsbCcActivePin::None,
          "a selected-pin downgrade persisting 20ms revokes confirmation");

    const modules::UsbCcSourcePolicyState lostPolicy =
        reportedPolicyUpdate.state;
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromFullSample(
        lostPolicy, 80U, 651U, ccThresholds, 13000U);
    Check(reportedPolicyUpdate.transition ==
              modules::StrongSourceConfirmationTransition::None &&
              reportedPolicyUpdate.state.activePin ==
                  hardware::UsbCcActivePin::Cc2 &&
              reportedPolicyUpdate.state.confirmation.samples == 1U,
          "source loss permits a fresh guarded recovery window");
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromActivePinSample(
        reportedPolicyUpdate.state, 651U, ccThresholds, 13010U);
    Check(reportedPolicyUpdate.transition ==
              modules::StrongSourceConfirmationTransition::None &&
              !modules::IsStrongSourceConfirmed(
                  reportedPolicyUpdate.state.confirmation),
          "an active-pin reading cannot shortcut full-pair recovery confirmation");

    modules::UsbCcSourcePolicyState pairAmbiguityPolicy =
        ConfirmReportedCc2Source(ccThresholds);
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromFullSample(
        pairAmbiguityPolicy, 638U, 651U, ccThresholds, 20000U);
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromFullSample(
        reportedPolicyUpdate.state, 638U, 651U, ccThresholds, 20020U);
    Check(reportedPolicyUpdate.transition ==
              modules::StrongSourceConfirmationTransition::Lost &&
              reportedPolicyUpdate.state.confirmation.samples == 0U,
          "persistent dual-active ambiguity loses authorization on full recheck");

    modules::UsbCcSourcePolicyState wrappedPairPolicy =
        ConfirmReportedCc2Source(ccThresholds);
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromFullSample(
        wrappedPairPolicy,
        638U,
        651U,
        ccThresholds,
        UINT32_MAX - 9U);
    Check(!modules::IsUsbCcFullPairRecheckDue(
              reportedPolicyUpdate.state, 9U) &&
              modules::IsUsbCcFullPairRecheckDue(
                  reportedPolicyUpdate.state, 10U),
          "full-pair recheck timing remains correct across millis wrap");

    modules::UsbCcSourcePolicyState wrappedPolicy =
        ConfirmReportedCc2Source(ccThresholds);
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromActivePinSample(
        wrappedPolicy, 567U, ccThresholds, UINT32_MAX - 9U);
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromActivePinSample(
        reportedPolicyUpdate.state, 567U, ccThresholds, 9U);
    Check(reportedPolicyUpdate.transition ==
              modules::StrongSourceConfirmationTransition::None,
          "composed source debounce remains pending for 19ms across wrap");
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromActivePinSample(
        reportedPolicyUpdate.state, 567U, ccThresholds, 10U);
    Check(reportedPolicyUpdate.transition ==
              modules::StrongSourceConfirmationTransition::Lost,
          "composed source debounce expires at 20ms across wrap");

    modules::UsbCcSourcePolicyState cadencePolicy;
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromFullSample(
        cadencePolicy, 80U, 651U, ccThresholds, 100U);
    cadencePolicy = reportedPolicyUpdate.state;
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromFullSample(
        cadencePolicy, 80U, 651U, ccThresholds, 1099U);
    Check(reportedPolicyUpdate.state.confirmation.samples == 1U,
          "forced full samples cannot accelerate the one-second confirmation cadence");
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromFullSample(
        reportedPolicyUpdate.state, 80U, 651U, ccThresholds, 1100U);
    Check(reportedPolicyUpdate.state.confirmation.samples == 2U,
          "confirmation accepts the sample exactly one second later");
    modules::UsbCcSourcePolicyState wrappedCadencePolicy;
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromFullSample(
        wrappedCadencePolicy,
        80U,
        651U,
        ccThresholds,
        UINT32_MAX - 499U);
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromFullSample(
        reportedPolicyUpdate.state, 80U, 651U, ccThresholds, 499U);
    Check(reportedPolicyUpdate.state.confirmation.samples == 1U,
          "confirmation cadence remains pending for 999ms across wrap");
    reportedPolicyUpdate = modules::UpdateUsbCcSourceFromFullSample(
        reportedPolicyUpdate.state, 80U, 651U, ccThresholds, 500U);
    Check(reportedPolicyUpdate.state.confirmation.samples == 2U,
          "confirmation cadence expires at one second across wrap");
    Check(!modules::IsUsbCcActivePinMonitorDue(100U, 109U) &&
              modules::IsUsbCcActivePinMonitorDue(100U, 110U),
          "selected-pin monitoring becomes due at the 10ms cadence");
    Check(!modules::IsUsbCcActivePinMonitorDue(UINT32_MAX - 4U, 4U) &&
              modules::IsUsbCcActivePinMonitorDue(UINT32_MAX - 4U, 5U),
          "selected-pin cadence remains correct across millis wrap");

    modules::PowerBudgetPolicyConfig recoveryCfg;
    recoveryCfg.maxLedCurrentMa = latchedWeakSource.maxLedCurrentMa;
    recoveryCfg.maxTotalCurrentMa = latchedWeakSource.maxTotalCurrentMa;
    recoveryCfg.fixedSystemCurrentMa = latchedWeakSource.fixedSystemCurrentMa;
    modules::PowerBudgetPolicy recoveryPolicy(recoveryCfg);
    recoveryPolicy.SetEstimatedLedCurrentMa(1000U);
    Check(recoveryPolicy.GetAllowedLedCurrentMa() == 216U,
          "conservative source envelope leaves only measured LED headroom");

    modules::PowerBudgetPolicyConfig exhaustedCfg;
    exhaustedCfg.maxLedCurrentMa = 1000U;
    exhaustedCfg.maxTotalCurrentMa = 200U;
    exhaustedCfg.fixedSystemCurrentMa = 234U;
    modules::PowerBudgetPolicy exhaustedPolicy(exhaustedCfg);
    exhaustedPolicy.SetEstimatedLedCurrentMa(100U);
    Check(exhaustedPolicy.GetAllowedLedCurrentMa() == 0U,
          "fixed load that consumes the total cap disables LEDs");

    modules::PowerBudgetPolicyConfig usbBaselineCfg;
    usbBaselineCfg.maxLedCurrentMa = 500U;
    usbBaselineCfg.maxTotalCurrentMa = 600U;
    usbBaselineCfg.fixedSystemCurrentMa = 100U;
    usbBaselineCfg.allowUsbPower = true;
    usbBaselineCfg.usbReserveCurrentMa = 150U;
    modules::PowerBudgetPolicy usbBaselinePolicy(usbBaselineCfg);
    usbBaselinePolicy.SetUsbPowerRequested(true);
    usbBaselinePolicy.SetEstimatedLedCurrentMa(300U);
    Check(usbBaselinePolicy.GetUsbPowerEnabled(),
          "USB decision includes fixed load while total remains under cap");
    usbBaselinePolicy.SetEstimatedLedCurrentMa(400U);
    Check(!usbBaselinePolicy.GetUsbPowerEnabled(),
          "USB decision disables when fixed plus LED plus reserve exceeds cap");

    if (g_failures == 0) {
        std::puts("[POWER_BUDGET_POLICY] ok");
        return 0;
    }

    std::printf("[POWER_BUDGET_POLICY] failures=%d\n", g_failures);
    return 1;
}
