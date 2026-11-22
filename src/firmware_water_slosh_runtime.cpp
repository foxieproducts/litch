#ifdef ARDUINO

#include "firmware/water_slosh_runtime.hpp"

#include "firmware/firmware_internal.hpp"
#include "hardware/motion_board_mapping.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/water_slosh_presentation.hpp"
#include "runtime/square_mapping.hpp"

#if defined(LITCH_ESP32S3)
#pragma GCC diagnostic error "-Wframe-larger-than=128"
#endif

namespace firmware_internal {

void SuppressWaterSlosh(FirmwareSystem& sys) {
    // Litchcraft is another ambient full-board presentation, so it can safely
    // become the baseline under the shared fade instead of hard-cutting water.
    sys.waterSlosh.FadeOut(fxe::Millis());
}

uint8_t UpdateWaterSlosh(
    FirmwareSystem& sys,
    const modules::led_frame_composer::ComposeInputs& inputs) {
    const uint32_t nowMs = inputs.interactionSnapshot->system.nowMs;
    // Only the physically measured V3 transform is an embedded launch target.
    // Keep its signs constant so generic future mapping machinery costs no RAM
    // or repeated per-sample validation on this fixed hardware revision.
    constexpr auto mapping = hardware::MotionBoardMappingFor(hardware::BoardRevision::V3S3);
    const bool admitted = hardware::HasFourLedSquares(sys.runtimeHardware.resolved.revision) &&
        mapping.IsValid() && sys.motionWake.IsAvailable() &&
        sys.motionWake.GetSensor().GetAppliedConfig().enabled &&
        modules::CanPresentWaterSlosh(*inputs.interactionSnapshot,
            {inputs.runtimeHealth.state,
             sys.stateMachine.GetChessInteractionModel().board.valid,
             inputs.higherPriorityStatusOverlayActive ||
                 sys.settings.IsDisabled() ||
                 sys.settings.GetLastError() != modules::SettingsStoreError::None ||
                 inputs.menuActive || inputs.menuIntent.menuActive ||
                 inputs.edgeCueIntent.kind != modules::EdgeLedCueKind::None ||
                 sys.amptestVerify.IsActive(),
             sys.sleep.GetState() == firmware::SleepState::Awake,
             sys.settings.ReadValue("MINIMAL_ANIMATIONS", false)});
    sys.waterSlosh.SetAdmitted(admitted, nowMs);
    // Reuse the ordinary polling owner's filtered mean. A render must never
    // acquire light samples or reinterpret sensor indices as physical ends.
    const auto& ambient = sys.ambientLights;
    const bool ambientAvailable = sys.runtimeHardware.resolved.hasLightSensors &&
        ambient.IsPoweredOn() && ambient.HasUsableSampleForBrightness() &&
        ambient.GetLastSampleAgeMs() < 240U;
    sys.waterSlosh.SetAmbientLight(ambient.GetLastLux(), ambientAvailable);
    sys.waterSlosh.Update(nowMs);
    return modules::WaterSloshRiderSquare(*inputs.interactionSnapshot);
}

void PollWaterSloshMotion(FirmwareSystem& sys) {
    const uint32_t nowMs = fxe::Millis();
    constexpr auto mapping = hardware::MotionBoardMappingFor(hardware::BoardRevision::V3S3);
    auto& sensor = sys.motionWake.GetSensor();
    // Cap interactive reads at 40 Hz. Tap-enabled boards already acquire much
    // faster internally, while motion-only boards safely return the most recent
    // 25 Hz sample; neither case increases the global LED/render cadence.
    constexpr uint32_t SAMPLE_PERIOD_MS = 25U;
    if (sys.waterSlosh.IsAdmitted() &&
        nowMs - sys.waterSloshSampleMs >= SAMPLE_PERIOD_MS) {
        sys.waterSloshSampleMs = nowMs;
        hardware::Lis2dw12Sample sample;
        if (sensor.ReadSample(sample)) {
            modules::WaterSloshMotion motion;
            (void)mapping.Transform(sample, motion); // constexpr, verified valid above
            sys.waterSlosh.Observe(motion, nowMs);
        } else {
            sys.waterSlosh.Cancel(nowMs);
        }
    }
    const uint16_t previousOdr = sensor.GetAppliedConfig().odrTenthsHz;
    if (!sys.motionWake.SetInteractiveSampling(
            firmware::InteractiveSamplingOwner::WaterSlosh,
            sys.waterSlosh.IsActive())) {
        sys.waterSlosh.Cancel(nowMs);
    }
    if (previousOdr != sensor.GetAppliedConfig().odrTenthsHz) {
        // Mode changes never block for a sample in the render/input task.
        sys.waterSloshSampleMs = nowMs;
    }
}

} // namespace firmware_internal

#endif
