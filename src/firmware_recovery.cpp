#ifdef ARDUINO

#include "firmware/firmware_recovery_internal.hpp"

#include <Arduino.h>

#if defined(LITCH_HW_VARIANT_S3)
#include <esp_ota_ops.h>
#endif

#include "firmware/firmware_internal.hpp"
#include "firmware/firmware_app_internal.hpp"
#include "firmware/cold_factory_reset.hpp"
#include "firmware/recovery_led_frame.hpp"
#include "firmware/recovery_status.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "hardware/led_layout.hpp"
#include "logger.hpp"

namespace firmware_internal {
namespace {

bool IsPressed(const uint8_t pin) {
    return pin != hardware::NO_GPIO && digitalRead(pin) == LOW;
}

void ConfigureRecoveryControlPins(FirmwareSystem& sys) {
    if (sys.recoveryControlsConfigured) {
        return;
    }

    const hardware::BoardHardwareProfile& profile =
        sys.runtimeHardware.resolved;
    pinMode(profile.p1.x, INPUT_PULLUP);
    pinMode(profile.p1.o, INPUT_PULLUP);
    if (profile.hasP2Controls) {
        pinMode(profile.p2.x, INPUT_PULLUP);
        pinMode(profile.p2.o, INPUT_PULLUP);
    }
    sys.recoveryControlsConfigured = true;
}

firmware::RecoveryControlLevels ReadFaceControlLevels(
    const FirmwareSystem& sys) {
    const hardware::BoardHardwareProfile& profile =
        sys.runtimeHardware.resolved;
    firmware::RecoveryControlLevels controls;
    controls.p1X = IsPressed(profile.p1.x);
    controls.p1O = IsPressed(profile.p1.o);
    controls.p2X = profile.hasP2Controls && IsPressed(profile.p2.x);
    controls.p2O = profile.hasP2Controls && IsPressed(profile.p2.o);
    controls.hasP2Controls = profile.hasP2Controls;
    return controls;
}

bool FramesEqual(const firmware::RecoveryLedFrame& lhs,
                 const firmware::RecoveryLedFrame& rhs) {
    for (size_t index = 0U; index < lhs.size(); ++index) {
        if (lhs[index].r != rhs[index].r ||
            lhs[index].g != rhs[index].g ||
            lhs[index].b != rhs[index].b) {
            return false;
        }
    }
    return true;
}

void EnsureRecoveryOutputInitialized(FirmwareSystem& sys) {
    EnsurePowerRailInitialized(sys);
    auto& leds = sys.Leds();
    leds.SetRmtFlashArbiter(&sys.rmtFlashArbiter);
    if (!sys.ledOutputInitialized) {
        leds.Begin();
        sys.ledOutputInitialized = true;
    }
}

void RenderRecoverySurface(FirmwareSystem& sys,
                           const uint32_t nowMs,
                           const bool force) {
    if (!sys.Leds().IsOutputEnabled()) {
        sys.recoveryFrameInitialized = false;
        return;
    }
    const firmware::RecoveryLedFrame semanticFrame =
        firmware::ComposeRecoveryLedFrame(
            sys.recoveryPolicy.GetSnapshot(), nowMs);
    if (!force && sys.recoveryFrameInitialized &&
        FramesEqual(semanticFrame, sys.recoveryLastFrame)) {
        return;
    }

    PhysicalLedFrame physicalFrame{};
    hardware::ApplySemanticStatusFrame(
        {sys.runtimeHardware.resolved.revision}, semanticFrame, physicalFrame);
    RenderPhysicalFrameWithRecoveryPowerBudget(
        sys, physicalFrame, 256U);
    sys.recoveryLastFrame = semanticFrame;
    sys.recoveryFrameInitialized = firmware_internal::LedOutputCompleted(sys);
}

void BeginRecoverySession(FirmwareSystem& sys,
                          const firmware::RecoveryCause cause,
                          const firmware::RecoveryBeginMode beginMode) {
    ConfigureRecoveryControlPins(sys);
    EnsureRecoveryOutputInitialized(sys);
    sys.recoveryPolicy.Begin(cause, beginMode, fxe::Millis());
    sys.recoveryConsoleCadence.initialized = false;
    sys.recoveryFrameInitialized = false;
    RenderRecoverySurface(sys, fxe::Millis(), true);
}

void ReportRecoveryActionFailure(FirmwareSystem& sys,
                                 const firmware::RecoveryActionFailure failure) {
    logger::Error("recovery: action failed (%s)",
                  firmware::RecoveryActionFailureToken(failure));
    sys.safeModeActive = true;
    BeginRecoveryUpdater(sys);
    RenderRecoverySurface(sys, fxe::Millis(), true);
}

class FirmwareRecoveryActionTarget final
    : public firmware::ColdFactoryResetTarget {
  public:
    explicit FirmwareRecoveryActionTarget(FirmwareSystem& sys)
        : ColdFactoryResetTarget(sys.recoveryPolicy, sys.runtimeHardware.resolved),
          m_sys(sys) {}

    firmware::RecoveryControlLevels ReadResetControls() override {
        const auto& profile = m_sys.runtimeHardware.resolved;
        if (!m_resetPinsConfigured) {
            ConfigureRecoveryControlPins(m_sys);
            pinMode(profile.p1.down, INPUT_PULLUP);
            if (profile.hasP2Controls) pinMode(profile.p2.down, INPUT_PULLUP);
            m_resetPinsConfigured = true;
        }
        auto controls = ReadFaceControlLevels(m_sys);
        controls.p1Down = IsPressed(profile.p1.down);
        controls.p2Down = profile.hasP2Controls && IsPressed(profile.p2.down);
        return controls;
    }

    bool DeliverResetFrame(const hardware::PhysicalLedFrame& frame) override {
        EnsureRecoveryOutputInitialized(m_sys);
        if (!m_sys.Leds().IsOutputEnabled()) return false;
        RenderPhysicalFrameWithRecoveryPowerBudget(m_sys, frame, 256U);
        return LedOutputCompleted(m_sys);
    }

    void WaitForResetInput(const uint32_t durationMs) override { delay(durationMs); }

    void PrepareForRetryReboot() override { m_sys.bootGuard.PrepareForRetryReboot(); }

    bool DeletePersistedState() override {
        m_sys.safeModeActive = true;
        logger::Error(
            "recovery: destructive consent complete; deleting persisted state once");
        return m_sys.settings.DeleteFile();
    }

    void ClearBootGuard() override { m_sys.bootGuard.Clear(); }

    void Restart() override {
        delay(50);
        ESP.restart();
    }

  private:
    FirmwareSystem& m_sys;
    bool m_resetPinsConfigured = false;
};

void ExecuteRecoveryAction(FirmwareSystem& sys,
                           const firmware::RecoveryAction action) {
    if (action == firmware::RecoveryAction::None) {
        return;
    }

    const runtime::BootGuard::Snapshot diagnostics =
        sys.bootGuard.GetSnapshot();
    if (action == firmware::RecoveryAction::RetryBoot) {
        logger::Warn(
            "recovery: explicit retry boots=%lu early_crashes=%lu reset_reason=%lu",
            static_cast<unsigned long>(diagnostics.bootCount),
            static_cast<unsigned long>(diagnostics.earlyCrashCount),
            static_cast<unsigned long>(diagnostics.lastResetReason));
    }

    FirmwareRecoveryActionTarget target(sys);
    const auto failure = firmware::ExecuteRecoveryActionSequence(action, target);
    if (failure != firmware::RecoveryActionFailure::None) {
        sys.recoveryPolicy.ReportActionFailure(failure, fxe::Millis());
        ReportRecoveryActionFailure(sys, failure);
    }
}

} // namespace

firmware::RecoveryCause DetectBootRecoveryCause(
    const FirmwareSystem& sys) {
    firmware::RecoveryEvidence evidence;
    evidence.earlyCrashDetected = sys.bootGuard.IsSafeModeActive();

#if defined(LITCH_HW_VARIANT_S3)
    if (sys.runtimeHardware.resolved.supportsBleOta) {
        const esp_partition_t* const running =
            esp_ota_get_running_partition();
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        if (running != nullptr &&
            esp_ota_get_state_partition(running, &state) == ESP_OK) {
            evidence.updatePending =
                state == ESP_OTA_IMG_NEW ||
                state == ESP_OTA_IMG_PENDING_VERIFY;
        }
        evidence.updateFailed =
            esp_ota_get_last_invalid_partition() != nullptr;
    }
#endif

    return firmware::SelectRecoveryCause(evidence);
}

void EnsurePowerRailInitialized(FirmwareSystem& sys) {
    if (sys.powerRailInitialized) {
        return;
    }
    sys.powerRail.Begin();
    sys.powerRailInitialized = true;
}

void EnterSafeModeRecovery(FirmwareSystem& sys,
                           const firmware::RecoveryCause cause) {
    sys.safeModeActive = true;
    BeginRecoveryUpdater(sys);
    BeginRecoverySession(
        sys, cause, firmware::RecoveryBeginMode::RequireNeutralRelease);
    logger::Warn("recovery: active cause=%s",
                 firmware::RecoveryCauseToken(cause));
}

void RunSafeModeRecovery(FirmwareSystem& sys) {
    auto& ctx = GetSystemContext();
    sys.recoveryConsoleCadence.EnsureInitialized();
    if (sys.recoveryConsoleCadence.IsDue()) {
        PumpRecoveryUpdater(sys);
        MaybeEnableConsoleOutput();
        if (ctx.console != nullptr) {
            ctx.console->Update();
        }
        sys.recoveryConsoleCadence.RescheduleFromNow();
    }

    const uint32_t nowMs = fxe::Millis();
    const firmware::RecoveryUpdate update = sys.recoveryPolicy.Update(
        firmware::NormalizeSafeRecoveryInput(ReadFaceControlLevels(sys)), nowMs);
    if (update.action != firmware::RecoveryAction::None) {
        RenderRecoverySurface(sys, nowMs, true);
        ExecuteRecoveryAction(sys, update.action);
    }
    RenderRecoverySurface(sys, nowMs, false);
    delay(1);
}

void MaybeRunFactoryResetBootFlow(FirmwareSystem& sys) {
    FirmwareRecoveryActionTarget target(sys);
    const auto failure = firmware::RunColdFactoryReset(
        {sys.bootGuard.IsColdPowerOnReset(), sys.sleep.IsDeepSleepWake()}, target);
    if (failure != firmware::RecoveryActionFailure::None) {
        ReportRecoveryActionFailure(sys, failure);
    } else {
        sys.recoveryFrameInitialized = false;
    }
}

} // namespace firmware_internal

#endif
