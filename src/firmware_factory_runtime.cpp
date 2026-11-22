#ifdef ARDUINO

#include <Arduino.h>

#include "firmware/firmware_app_internal.hpp"
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_tasks_internal.hpp"
#include "firmware/litchcraft_persistence_runtime.hpp"
#include "firmware/ota_boot_health.hpp"
#include "foxie_embedded/millis.hpp"
#include "logger.hpp"
#include "modules/ota_runtime_acceptance.hpp"
#include "runtime/progress_watchdog.hpp"

namespace firmware_internal {

void LoopFactoryRuntime(FirmwareSystem& sys) {
    const uint32_t loopStartedAtMs = fxe::Millis();
    (void)PollRuntimeBootHealth(sys);
    PollLedPowerSourceStatus(sys, loopStartedAtMs);
    sys.ble.DrainEventQueue();
    sys.ble.Update(sys.settings);
    MaybeEnableConsoleOutput();
    if (auto* console = GetSystemContext().console) console->Update();
    if (PollPendingReboot(sys)) {
        const auto result = RenderLeds();
        sys.ledRuntimeAdmission.NoteRenderOutcome(result, loopStartedAtMs);
        runtime::FeedProgressWatchdog();
        delay(20U);
        return;
    }
    sys.comptest.Update();
    if (!sys.ble.IsOtaInProgress()) {
        const bool motionOk = sys.motionWake.Update(sys.settings);
        PollButtonsAndDispatch();
        // A hold may start during button dispatch. Tick it with a later clock
        // sample so elapsed time stays valid even when polling crosses a millisecond.
        const uint32_t nowMs = fxe::Millis();
        if (sys.hardwareTest.IsActive()) {
            if (hardware::HasFourLedSquares(sys.runtimeHardware.resolved.revision)) {
                sys.hardwareTest.ReportMotionPresence(
                    sys.motionWake.CheckFactoryPresence(motionOk), nowMs);
            }
            sys.hardwareTest.Tick(nowMs);
            if (sys.runtimeHardware.resolved.hasLightSensors &&
                sys.ambientLights.UpdateAttempt().attemptCompleted) {
                sys.hardwareTest.ReportLightSensorLinks(sys.ambientLights.Sensor0Ok(),
                    sys.ambientLights.Sensor1Ok(), nowMs);
                sys.hardwareTest.ObserveLightSample(
                    {static_cast<uint8_t>((sys.ambientLights.Sensor0Ok() ? 1U : 0U) |
                         (sys.ambientLights.Sensor1Ok() ? 2U : 0U)),
                     {sys.ambientLights.GetLastLux0(), sys.ambientLights.GetLastLux1()}}, nowMs);
            }
            sys.hardwareTest.UpdateSensors(sys.boardInput, nowMs);
            sys.hardwareTest.ReportExpanderLinks(sys.inputScan.LastRespondingExpanderMask(), nowMs);
        }
        UpdateHardwareTestRadio(sys, nowMs);
    }
    if (sys.settingsPersistence.IsStarted()) sys.settings.Update();
    const auto result = RenderLeds(sys.factoryCompletion.IsPending());
    sys.ledRuntimeAdmission.NoteRenderOutcome(result, loopStartedAtMs);
    sys.factoryCompletion.NoteFrameAcknowledged(
        result == firmware::LedRenderAttemptResult::Completed);
    PollFactoryCompletionReboot(sys);
    runtime::FeedProgressWatchdog();
    delay(result == firmware::LedRenderAttemptResult::Denied ? 1U : 20U);
}

} // namespace firmware_internal

#endif
