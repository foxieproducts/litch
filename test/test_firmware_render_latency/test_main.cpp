#include <cstdio>
#include "hardware/usb_cc_sampler.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "firmware/led_runtime_admission.hpp"
#include "firmware/periodic_task_cadence.hpp"
#include "firmware/led_brightness_attempt.hpp"
#include "modules/ambient_lux_averager.hpp"
#include "modules/ota_led_presentation.hpp"
#include "modules/usb_cc_source_policy.hpp"
#include "modules/upward_output_slew.hpp"

namespace {
int failures = 0;
void Check(bool okay, const char* text) {
    if (!okay) { ++failures; std::printf("[FAIL] %s\n", text); }
}
void AmbientHandoff() {
    modules::AmbientLuxAverager average;
    Check(average.ComputeSmoothedAmbientFactor01(0U) == 0.0f,
          "boot begins at the no-sample fallback");
    average.OnLuxSample(0U, 10000.0f);
    Check(average.ComputeSmoothedAmbientFactor01(0U) == 0.0f,
          "same-turn first ambient sample cannot flash the whole board");
    const float first = average.ComputeSmoothedAmbientFactor01(20U);
    Check(first > 0.0f && first < 0.1f, "first bright ambient sample eases upward");
}
void IncrementalSourceJourney(uint32_t start) {
    hardware::UsbCcSampler sampler;
    modules::UsbCcSourcePolicyState policy;
    const auto thresholds = hardware::GetDefaultUsbCcThresholds();
    unsigned inputTurns = 0U;
    for (unsigned cycle = 0U; cycle < 3U; ++cycle) {
        const uint32_t begin = start + cycle * 1036U;
        sampler.Begin(begin);
        unsigned reads = 0U;
        hardware::UsbCcReadings result;
        bool finished = false;
        for (unsigned elapsed = 0U; elapsed <= 36U; ++elapsed) {
            ++inputTurns; // Input dispatch remains possible between every conversion.
            const unsigned before = reads;
            const bool complete = sampler.Update(begin + elapsed, [&](uint8_t pin) {
                ++reads;
                return uint16_t(pin == 0U ? 1100U : 70U);
            }, result);
            Check(reads - before <= 1U, "a runtime service performs at most one ADC read");
            if (!complete) continue;
            Check(elapsed == 36U && reads == 18U, "full pair preserves 36ms settling and 18 conversions");
            Check(result.cc1 == 1100U && result.cc2 == 70U, "published pair uses complete filtered windows");
            policy = modules::UpdateUsbCcSourceFromFullSample(
                policy, result.cc1, result.cc2, thresholds, begin + elapsed).state;
            finished = true;
        }
        Check(finished, "scheduled pair completes");
        Check(modules::IsStrongSourceConfirmed(policy.confirmation) ==
                  (cycle == 2U),
              "authorization gains on the third complete pair after two spaced intervals");
    }
    Check(inputTurns == 111U, "input and frame owner can run throughout all three source pairs");
    sampler.Begin(start);
    sampler.Cancel();
    hardware::UsbCcReadings result;
    Check(!sampler.Update(start + 100U, [](uint8_t) { return uint16_t(1100U); }, result),
          "revocation cancels incomplete source evidence");
}
void SourceBrightnessHandoff() {
    modules::UpwardOutputSlew output;
    output.Reset(500U, 0U);
    Check(output.Update(1250U, 0U) == 500U, "source authorization cannot cause an instant whole-board step");
    Check(output.Update(1250U, 20U) == 520U, "source headroom ramps at bounded rate");
    Check(output.Update(1250U, 5000U) == 560U, "a flash stall cannot bank a large brightness jump");
    Check(output.Update(500U, 5001U) == 500U, "power loss applies conservative cap immediately");
}
void DeniedFramesCannotBankBrightness() {
    modules::AmbientLuxAverager ambient;
    (void)ambient.ComputeSmoothedAmbientFactor01(0U);
    ambient.OnLuxSample(0U, 10000.0f);
    modules::UpwardOutputSlew source;
    source.Reset(450U, 0U);
    const auto ambientCommitted = ambient.CaptureSmoothing();
    auto smoothing = ambientCommitted;
    for (uint32_t nowMs = 1U; nowMs <= 1000U; ++nowMs) {
        firmware::LedBrightnessAttempt attempt(smoothing, source);
        ambient.OnLuxSample(nowMs, 10000.0f);
        (void)modules::AmbientLuxAverager::SampleSmoothing(ambient.Config(),
            smoothing, nowMs, ambient.ComputeAmbientFactor01(nowMs));
        (void)source.Update(1234U, nowMs);
        Check(!attempt.Finish(false), "a denied attempt reports incomplete");
    }
    const auto ambientRestored = smoothing;
    Check(ambientRestored.factor01 == ambientCommitted.factor01 &&
              ambientRestored.lastUpdateMs == ambientCommitted.lastUpdateMs &&
              ambientRestored.lastUpdateMsValid ==
                  ambientCommitted.lastUpdateMsValid &&
              source.Update(1234U, 0U) == 450U,
          "production transaction restores both brightness owners");
    ambient.OnLuxSample(5000U, 10000.0f);
    firmware::LedBrightnessAttempt accepted(smoothing, source);
    const float firstAcceptedAmbient = modules::AmbientLuxAverager::SampleSmoothing(
        ambient.Config(), smoothing, 5000U, ambient.ComputeAmbientFactor01(5000U));
    const uint16_t firstAcceptedSource = source.Update(1234U, 5000U);
    Check(accepted.Finish(true), "a completed transfer commits brightness");
    Check(firstAcceptedAmbient > 0.0f && firstAcceptedAmbient < 0.1f,
          "denied retries cannot bank an ambient flash");
    Check(firstAcceptedSource == 490U,
          "denied retries cannot bank source headroom");
}
void FlashDeniedFrameRetry() {
    firmware::LedRuntimeAdmission admission;
    constexpr uint32_t CADENCE_REMAINING_MS = 10U;
    uint32_t nowMs = 100U;
    Check(admission.ConstrainNextWakeMs(CADENCE_REMAINING_MS, nowMs) ==
              CADENCE_REMAINING_MS,
          "ordinary cadences own sleep when no retry is pending");
    Check(admission.Update(false, true, nowMs), "normal cadence admits a frame");
    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Denied, nowMs);
    Check(!admission.Update(false, false, nowMs),
          "a denied transfer does not spin in the same millisecond");
    const uint32_t retryWakeMs =
        admission.ConstrainNextWakeMs(CADENCE_REMAINING_MS, nowMs);
    Check(retryWakeMs == 1U && nowMs + retryWakeMs == 101U,
          "the production sleep decision wakes for the retry before ordinary cadences");
    Check(admission.ConstrainNextWakeMs(0U, nowMs) == 0U,
          "a retry cannot postpone already due ordinary work");
    nowMs += retryWakeMs;
    Check(admission.Update(false, false, nowMs),
          "the scheduled wake admits a retry before normal frame cadence");
    Check(admission.ConstrainNextWakeMs(CADENCE_REMAINING_MS, nowMs) == 0U,
          "an already due retry cannot wait for ordinary cadences");

    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Denied, nowMs);
    Check(!admission.Update(false, false, nowMs),
          "repeated denial still yields before the next retry");
    const uint32_t repeatedWakeMs =
        admission.ConstrainNextWakeMs(CADENCE_REMAINING_MS, nowMs);
    Check(repeatedWakeMs == 1U, "repeated denial schedules another bounded wake");
    nowMs += repeatedWakeMs;
    Check(nowMs == 102U && admission.Update(false, false, nowMs),
          "a repeatedly denied transfer retains its near-immediate retry");
    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Completed, nowMs);
    Check(admission.ConstrainNextWakeMs(CADENCE_REMAINING_MS, nowMs) ==
              CADENCE_REMAINING_MS && !admission.Update(false, false, nowMs + 1U),
          "a completed retry returns sleep and rendering to normal cadence");

    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Denied, 120U);
    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::NotAttempted, 121U);
    Check(admission.ConstrainNextWakeMs(CADENCE_REMAINING_MS, 121U) ==
              CADENCE_REMAINING_MS && !admission.Update(false, false, 122U),
          "an intentional no-visual interval clears retry admission and sleep");

    nowMs = UINT32_MAX;
    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Denied, nowMs);
    const uint32_t wrapWakeMs =
        admission.ConstrainNextWakeMs(CADENCE_REMAINING_MS, nowMs);
    Check(wrapWakeMs == 1U, "retry sleep remains bounded before millis wrap");
    nowMs += wrapWakeMs;
    Check(nowMs == 0U && admission.Update(false, false, nowMs) &&
              admission.ConstrainNextWakeMs(CADENCE_REMAINING_MS, nowMs) == 0U,
          "the scheduled retry survives unsigned millis wrap");
    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Completed, nowMs);
    Check(admission.ConstrainNextWakeMs(CADENCE_REMAINING_MS, nowMs) ==
              CADENCE_REMAINING_MS,
          "a completed wraparound retry clears the sleep deadline");
}

void CadenceIncludesRenderingTime(uint32_t startMs, uint32_t periodMs) {
    const fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(startMs);
    firmware_internal::PeriodicTaskCadence cadence;
    cadence.periodMs = periodMs;
    Check(cadence.IsDue() && cadence.RemainingMs() == 0U,
          "uninitialized cadence can be serviced immediately");
    cadence.EnsureInitialized();
    Check(!cadence.IsDue() && cadence.RemainingMs() == periodMs,
          "initialization starts the existing period once");
    fxe::ManualMillis::AdvanceMs(3U);
    cadence.EnsureInitialized();
    Check(cadence.RemainingMs() == periodMs - 3U,
          "repeated initialization cannot postpone scheduled work");
    fxe::ManualMillis::AdvanceMs(periodMs - 3U);
    Check(cadence.IsDue(), "the first period becomes due");
    // The production-binding probe checks that LoopApp arms at this point.
    cadence.RescheduleFromNow();
    fxe::ManualMillis::AdvanceMs(12U);
    Check(!cadence.IsDue() && cadence.RemainingMs() == periodMs - 12U,
          "synchronous render duration consumes the existing period");
    fxe::ManualMillis::AdvanceMs(periodMs - 13U);
    Check(!cadence.IsDue() && cadence.RemainingMs() == 1U,
          "render-start cadence stays pending until its actual deadline");
    fxe::ManualMillis::AdvanceMs(1U);
    Check(cadence.IsDue() && cadence.RemainingMs() == 0U,
          "the next render is due one period after its previous start");
    cadence.RescheduleFromNow();
    fxe::ManualMillis::AdvanceMs(periodMs + 12U);
    Check(cadence.IsDue() && cadence.RemainingMs() == 0U,
          "an overrun remains due instead of adding another full period");
    cadence.RescheduleFromNow();
    Check(!cadence.IsDue() && cadence.RemainingMs() == periodMs,
          "a serviced overrun anchors new work now instead of replaying missed frames");
}

void SlowRenderRetryAndOtaTransition(uint32_t startMs) {
    const fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(startMs);
    firmware_internal::PeriodicTaskCadence cadence;
    cadence.periodMs = 35U;
    cadence.EnsureInitialized();
    fxe::ManualMillis::AdvanceMs(35U);
    firmware::LedRuntimeAdmission admission;
    Check(admission.Update(false, cadence.IsDue(), fxe::Millis()),
          "the ordinary CPU cadence admits its due frame");
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        cadence.RescheduleFromNow();
        fxe::ManualMillis::AdvanceMs(12U);
        admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Denied,
                                    fxe::Millis());
        Check(admission.ConstrainNextWakeMs(cadence.RemainingMs(), fxe::Millis()) == 1U,
              "slow denied rendering retains the one-millisecond retry");
        fxe::ManualMillis::AdvanceMs(1U);
        Check(!cadence.IsDue() && admission.Update(false, false, fxe::Millis()),
              "repeated denial retries before the render-start deadline");
    }
    cadence.RescheduleFromNow();
    fxe::ManualMillis::AdvanceMs(12U);
    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Completed,
                                fxe::Millis());
    Check(admission.ConstrainNextWakeMs(cadence.RemainingMs(), fxe::Millis()) == 23U,
          "completed retry returns to the successful frame-start deadline");
    Check(admission.Update(true, false, fxe::Millis()),
          "OTA entry still admits its first frame before an ordinary deadline");
    cadence.periodMs = modules::OtaLedRefreshMs(
        modules::ProjectOtaLedPresentation(true, false, false, 100U));
    cadence.RescheduleFromNow();
    fxe::ManualMillis::AdvanceMs(12U);
    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Completed,
                                fxe::Millis());
    Check(cadence.RemainingMs() == 8U &&
              !admission.Update(true, false, fxe::Millis()),
          "OTA finalization includes render work and does not repeatedly force entry");
    cadence.periodMs = 35U;
    Check(cadence.RemainingMs() == 23U &&
              admission.Update(false, false, fxe::Millis()),
          "OTA exit preserves its immediate restore before the CPU deadline");
    cadence.RescheduleFromNow();
    fxe::ManualMillis::AdvanceMs(12U);
    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::Completed,
                                fxe::Millis());
    Check(cadence.RemainingMs() == 23U &&
              !admission.Update(false, false, fxe::Millis()),
          "the restored frame returns to ordinary CPU timing");
    admission.NoteRenderOutcome(firmware::LedRenderAttemptResult::NotAttempted,
                                fxe::Millis());
    Check(admission.ConstrainNextWakeMs(cadence.RemainingMs(), fxe::Millis()) == 23U,
          "intentional no-visual output preserves ordinary timing without a retry");
}

void OtaFinalizationFlashHandoff() {
    // The physical V3 had a pending settings receipt 1.6 s after ota_end.
    // Two subsequent settings saves used 35 flash windows in total. The
    // handoff wait alone exceeded OTA's gate at the old transfer cadence.
    const auto presentation = modules::ProjectOtaLedPresentation(
        true, false, false, 100U);
    Check(modules::OtaLedRefreshMs(
              modules::ProjectOtaLedPresentation(true, false, false, 99U)) ==
              modules::OTA_LED_REFRESH_MS &&
              modules::OtaLedRefreshMs({}) == modules::OTA_LED_REFRESH_MS,
          "transfer and simulated OTA keep their existing render cadence");
    Check(modules::RuntimeLedRefreshMs({}, true, 20U) == 100U &&
              modules::RuntimeLedRefreshMs({}, true, 33U) == 100U &&
              modules::RuntimeLedRefreshMs({}, false, 20U) == 20U &&
              modules::RuntimeLedRefreshMs({}, false, 33U) == 33U,
          "main capture and autonomous LED owner share 10fps bulk and restore their normal periods");
    Check(modules::RuntimeLedRefreshMs({true, 99U}, true, 33U) == 200U &&
              modules::RuntimeLedRefreshMs(presentation, true, 33U) == 20U &&
              modules::RuntimeLedRefreshMs({}, true, 20U, true) == 100U &&
              modules::RuntimeLedRefreshMs({}, false, 20U, true) == 200U,
          "actual OTA keeps data/finalization priority while RAM bulk uses 10fps and other simulation keeps its cadence");
    hardware::RmtFlashArbiter arbiter;
    uint32_t elapsedMs = 0U;
    for (unsigned access = 0U; access < 35U; ++access) {
        const uint32_t nextRender = arbiter.NextRenderGeneration();
        Check(!arbiter.TryBeginFlash(nextRender),
              "settings flash waits for the next completed OTA render");
        elapsedMs += modules::OtaLedRefreshMs(presentation);
        Check(arbiter.TryBeginRender(),
              "finalization admits a render between flash windows");
        arbiter.EndRender();
        Check(arbiter.TryBeginFlash(nextRender),
              "settings flash resumes after the completed render");
        arbiter.EndFlash();
    }
    Check(elapsedMs < 3000U,
          "observed flash handoffs alone do not exhaust the OTA persistence gate");
}
} // namespace
int main() {
    AmbientHandoff();
    IncrementalSourceJourney(0U);
    IncrementalSourceJourney(UINT32_MAX - 10U);
    SourceBrightnessHandoff();
    DeniedFramesCannotBankBrightness();
    FlashDeniedFrameRetry();
    CadenceIncludesRenderingTime(0U, 20U);
    CadenceIncludesRenderingTime(0U, 35U);
    CadenceIncludesRenderingTime(UINT32_MAX - 10U, 35U);
    SlowRenderRetryAndOtaTransition(0U);
    SlowRenderRetryAndOtaTransition(UINT32_MAX - 10U);
    OtaFinalizationFlashHandoff();
    std::printf("[FIRMWARE_RENDER_LATENCY] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
