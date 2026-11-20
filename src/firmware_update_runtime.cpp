#ifdef ARDUINO

#if defined(ESP32)
#include <esp_system.h>
#endif

#include "firmware/firmware_app_internal.hpp"
#include "firmware/ota_boot_health.hpp"
#include "firmware/support_diagnostics.hpp"
#include "firmware/ota_platform_adapter.hpp"
#include "foxie_embedded/elapsed_time.hpp"
#include "logger.hpp"
#include "modules/ota_runtime_acceptance.hpp"

namespace firmware_internal {

void InitializeEarlyUpdater(FirmwareSystem& sys) {
    // These owners need no settings load, filesystem or normal runtime.
    // Normal radio, settings worker, gameplay and sensors await the settled fade.
    if (!sys.earlyUpdaterInitialized) {
        sys.earlyUpdaterInitialized = true;
        sys.ble.SetBootGuard(&sys.bootGuard);
        sys.ble.SetHardwareRevision(
            modules::ble_device_capabilities::HardwareRevisionFromId(
                static_cast<uint8_t>(sys.runtimeHardware.resolved.revision)));
        const uint64_t bootNonce =
            (static_cast<uint64_t>(sys.rngSeed) << 32U) |
#if defined(ESP32)
            static_cast<uint64_t>(esp_random());
#else
            static_cast<uint64_t>(~sys.rngSeed);
#endif
        sys.ble.SetConnectionSessionBootNonce(bootNonce == 0U ? 1U : bootNonce);
        ConfigureSupportDiagnostics(sys);
    }
    if (sys.otaPlatformAdapterBound) return;
    // A failed worker creation can retry at an explicit startup/recovery entry;
    // the boot identity must remain stable across that retry.
    auto elapsed = fxe::ElapsedTime::StartNow();
    auto* const adapter =
        firmware::InitializeEsp32OtaPlatformAdapter(sys.rmtFlashArbiter);
    sys.ble.SetOtaPlatformAdapter(adapter);
    sys.otaPlatformAdapterBound = adapter != nullptr;
    sys.startupOtaWorkerMs = elapsed.ElapsedMs();
    elapsed.Reset();
    modules::OtaPostBootRunningState runningState =
        modules::OtaPostBootRunningState::Unknown;
    bool stateReadable = false;
#if defined(LITCH_ESP32S3)
    stateReadable = firmware::ReadEsp32OtaRunningState(runningState);
    sys.otaBootAwaitingHealth = modules::RequiresOtaRuntimeAcceptance(
        stateReadable, runningState);
#endif
    // Boot preflight is read-only. Integrity and health decisions belong to
    // their worker at the normal health boundary or failed-boot recovery.
    const auto result = firmware::CheckPendingOtaAtBoot(sys.rmtFlashArbiter);
    sys.startupOtaCheckMs = elapsed.ElapsedMs();
    logger::Info(
        "ota_boot: preflight check=%u awaiting_health=%u state_readable=%u running_state=%u",
        static_cast<unsigned>(result),
        static_cast<unsigned>(sys.otaBootAwaitingHealth),
        static_cast<unsigned>(stateReadable),
        static_cast<unsigned>(runningState));
}

void BeginRecoveryUpdater(FirmwareSystem& sys) {
    InitializeEarlyUpdater(sys);
    if (sys.recoveryUpdaterStarted) {
        return;
    }
    sys.recoveryUpdaterStarted = true;
    sys.ble.BeginRecovery();
}

void PumpRecoveryUpdater(FirmwareSystem& sys) {
    if (!sys.recoveryUpdaterStarted) {
        return;
    }
    sys.ble.DrainEventQueue();
    sys.ble.Update(sys.settings);
}

} // namespace firmware_internal

#endif
