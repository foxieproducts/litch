#include "firmware/support_diagnostics.hpp"
#ifdef ARDUINO
#include "firmware/firmware_internal.hpp"
#include "firmware/firmware_version.hpp"
#include "modules/support_snapshot.hpp"
#include "runtime/progress_watchdog.hpp"

namespace firmware_internal {
namespace {
size_t Capture(void* context, bool publicOnly, uint8_t kind, uint32_t request,
               uint8_t* out, size_t capacity) {
    auto& sys = *static_cast<FirmwareSystem*>(context);
    if (publicOnly) {
        modules::support_snapshot::PublicHealth h{};
        const auto health = sys.runtimeHealth.Snapshot();
        // A never-observed healthy default is not proof of healthy hardware.
        const bool healthKnown = sys.startupBoardSnapshotObserved || health.causes != 0U;
        const bool storageAttempted = sys.settingsLoadAttempted;
        h.validity = 4U | (healthKnown ? 1U : 0U) |
            (sys.runtimeHardware.revisionSampled ? 2U : 0U) |
            (storageAttempted ? 8U : 0U);
        h.healthRevision = health.revision;
        h.healthState = static_cast<uint8_t>(health.state);
        h.healthCauses = health.causes;
        const auto progress = runtime::GetProgressSnapshot();
        h.resetReason = progress.resetReason <= 255U ? static_cast<uint8_t>(progress.resetReason) : 255U;
        h.storageError = static_cast<uint8_t>(sys.settings.GetLastError());
        h.bootLoadError = sys.bootStorageError;
        h.uptimeMs = modules::SupportUptimeMs();
        const auto revision = sys.runtimeHardware.detectedRevision;
        h.hardwareMajor = revision == hardware::BoardRevision::V1C3 ? 1U
            : revision == hardware::BoardRevision::V2S3 ? 2U
            : revision == hardware::BoardRevision::V3S3 || revision == hardware::BoardRevision::V3_3S3 ? 3U : 0U;
        h.hardwareMinor = revision == hardware::BoardRevision::V3_3S3 ? 3U : 0U;
        h.digestAvailability = static_cast<uint8_t>(sys.ble.CachedRunningFirmwareDigest(h.digest.data(), h.digest.size()));
        h.releaseLabel = firmware::FIRMWARE_VERSION;
        const auto boot = sys.bootGuard.GetSnapshot();
        const auto recovery = sys.recoveryPolicy.GetSnapshot();
        h.recoveryMode = sys.safeModeActive ? 1U : 0U;
        h.recoveryCause = static_cast<uint8_t>(recovery.cause);
        h.recoveryState = static_cast<uint8_t>(recovery.state);
        h.recoveryFailure = static_cast<uint8_t>(recovery.actionFailure);
        h.boots = boot.bootCount;
        h.earlyCrashes = boot.earlyCrashCount;
        h.resetReason = boot.lastResetReason <= 255U ? static_cast<uint8_t>(boot.lastResetReason) : 255U;
        h.updater = sys.ble.IsOtaInProgress() ? 3U : sys.otaPlatformAdapterBound ? 2U : 1U;
        h.storageAccess = storageAttempted ? 2U : sys.safeModeActive ? 1U : 0U;
        h.priorStageValid = progress.previousValid;
        if (h.priorStageValid) {
            h.priorStage = progress.previous.stages[0U];
            h.priorStageAtMs = progress.previous.stageAtMs[0U];
        }
        return modules::support_snapshot::EncodePublic(out, capacity, request, h);
    }
    if (kind == 1U) {
        return modules::support_snapshot::EncodeIncident(out, capacity, request,
            runtime::GetProgressSnapshot(), sys.settings.GetSaveCount(),
            sys.settings.GetErrorCount(), sys.bootStorageError);
    }
#ifdef LITCH_TRACE
    return modules::support_snapshot::EncodeHistory(out, capacity, request,
        sys.supportRuntimeStats.Snapshot(modules::SupportUptimeMs()),
        sys.settings.GetPersistedSaveTotal(), sys.settings.GetSaveCount(),
        sys.settings.GetErrorCount(), GetTraceRecorder().UserActions());
#else
    return 0U;
#endif
}
}
void ConfigureSupportDiagnostics(FirmwareSystem& sys) {
    sys.ble.SetSupportCapture(&Capture, &sys);
#ifdef LITCH_TRACE
    auto& actions = GetTraceRecorder().UserActions();
    sys.controlMutation.SetSupportActions(&actions);
    sys.gamePersistence.Litchcraft().SetSupportActions(&actions);
#endif
}
} // namespace firmware_internal
#endif
