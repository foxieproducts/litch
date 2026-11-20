#ifdef ARDUINO

#include <Arduino.h>

#include "firmware/firmware_app_internal.hpp"
#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_tasks_internal.hpp"
#include "firmware/ota_boot_health.hpp"
#include "firmware/ota_platform_adapter.hpp"
#include "logger.hpp"
#include "modules/ota_runtime_acceptance.hpp"

namespace firmware_internal {
namespace {
constexpr uint32_t OTA_HEALTH_COMPLETION_TARGET_MS = 5000U;
constexpr uint32_t CRASH_GUARD_HEALTH_BOUNDARY_MS = 90000U;
constexpr uint32_t HEALTH_FAST_RETRY_MS = 250U;
constexpr uint32_t HEALTH_SLOW_RETRY_MS = 5000U;
constexpr uint8_t HEALTH_FAST_RETRIES = 3U;

void NoteHealthRetry(firmware::RuntimeBootHealthState& state,
                     const firmware::OtaBootHealthCommitResult result) {
    if (state.commitErrors < HEALTH_FAST_RETRIES) {
        ++state.commitErrors;
        logger::Warn("ota_boot: health processing deferred: result=%u attempt=%u/3",
                     static_cast<unsigned>(result),
                     static_cast<unsigned>(state.commitErrors));
    }
    if (state.commitErrors == HEALTH_FAST_RETRIES) {
        logger::Warn("ota_boot: unresolved boot evidence; continuing quiet slow retries");
        ++state.commitErrors;
    }
    state.sinceCommitAttempt.Reset();
}

#if defined(LITCH_ESP32S3)
bool RuntimeTransportReady(const FirmwareSystem& sys) {
    const bool bleConfigured =
        sys.factoryBootOnly || sys.settings.ReadValue("BLE_ENABLED", 1U) != 0U;
    return modules::IsOtaRuntimeCandidateAcceptable({
        true, !bleConfigured || sys.ble.IsAdvertising() || sys.ble.IsConnected()});
}

void RefreshPendingBootState(FirmwareSystem& sys) {
    firmware::OtaBootEvidence evidence{};
    if (firmware::CopyEsp32OtaBootEvidence(evidence) && evidence.available &&
        evidence.runningState == modules::OtaPostBootRunningState::Valid) {
        // Startup's unreadable state is only suspicion. The existing complete
        // RAM observation clears it without waiting on the verification worker;
        // it cannot publish acceptance or authorize a boot mutation.
        sys.otaBootAwaitingHealth = false;
    }
}
#endif
} // namespace

bool PollRuntimeBootHealth(FirmwareSystem& sys) {
    auto& state = sys.runtimeBootHealth;
    if (!state.sinceLoopStart.Started()) {
        state.sinceLoopStart.Reset();
        state.sinceCommitAttempt.Reset();
    }
    const uint32_t retryMs = state.commitErrors < HEALTH_FAST_RETRIES
                                 ? HEALTH_FAST_RETRY_MS
                                 : HEALTH_SLOW_RETRY_MS;
    const bool retryDue = state.sinceCommitAttempt.HasElapsed(retryMs);

    if (sys.safeModeActive) {
#if defined(LITCH_ESP32S3)
        if (!state.commitFinished && (state.healthTicket != 0U ||
                                     (sys.otaBootAwaitingHealth && retryDue))) {
            if (state.healthTicket == 0U) RefreshPendingBootState(sys);
            if (sys.otaBootAwaitingHealth || state.healthTicket != 0U) {
                const auto result = firmware::ReportPendingOtaBootFailure(state);
                if (result == firmware::OtaBootHealthCommitResult::RollbackRequested ||
                    result == firmware::OtaBootHealthCommitResult::Unsupported) {
                    state.commitFinished = true;
                } else if (result != firmware::OtaBootHealthCommitResult::Pending) {
                    // Only the verifier may ask the SDK to invalidate a matched
                    // pending image with a fully verified VALID fallback.
                    NoteHealthRetry(state, result);
                }
            }
            if (state.healthTicket == 0U) state.sinceCommitAttempt.Reset();
        }
#endif
        if (state.healthTicket == 0U) sys.ble.ReleaseBootFirmwareDigest();
        // Recovery is not normal runtime health, even after ninety seconds.
        return false;
    }

#if defined(LITCH_ESP32S3)
    const bool startupPresentationComplete = StartupPresentationComplete(sys);
    const bool modeResolved = sys.factoryBootOnly ||
        !sys.gamePersistence.Litchcraft().IsInitialStoreModePending();
    // Essential mode recovery gets the first storage window. A completed
    // nonzero output admits hashing; valid dark/Off startup uses completion.
    // This is software output evidence, not optical visibility of Fox pixels.
    const bool startupLightingReady = sys.factoryBootOnly ||
        (modeResolved && (sys.bootFirstNonzeroRenderLogged || startupPresentationComplete));
    if (sys.otaBootAwaitingHealth && retryDue) RefreshPendingBootState(sys);
    if (!state.commitFinished && state.healthTicket == 0U && retryDue && startupLightingReady) {
        sys.ble.ReserveBootFirmwareDigest();
        const auto preparation = firmware::PreparePendingOtaBootHealth(state);
        if (preparation != firmware::OtaBootHealthCommitResult::Pending) {
            NoteHealthRetry(state, preparation);
        }
    }
    const auto shown = sys.ledAnimation.Receipt();
    if (state.startupCompletedAtMs == UINT32_MAX && modeResolved && startupPresentationComplete) {
        state.startupCompletedAtMs = shown.startupCompletedAtMs;
        logger::Info("ota_boot: startup presentation complete at_ms=%lu target_ms=%lu",
            static_cast<unsigned long>(state.startupCompletedAtMs),
            static_cast<unsigned long>(OTA_HEALTH_COMPLETION_TARGET_MS));
    }
    // Health is a working normal loop, confirmed startup output and configured
    // transport. Five seconds is the completion target, not an added hold.
    state.boundaryReached = modeResolved &&
        state.startupCompletedAtMs != UINT32_MAX && RuntimeTransportReady(sys);
#else
    state.boundaryReached = true;
#endif
    if (!state.commitFinished && state.boundaryReached &&
#if defined(LITCH_ESP32S3)
        startupLightingReady &&
#endif
        (state.healthTicket != 0U || retryDue)) {
        const auto result = firmware::CommitPendingOtaAtHealthBoundary(state);
        switch (result) {
            case firmware::OtaBootHealthCommitResult::Confirmed:
#if defined(LITCH_ESP32S3)
                if (!RuntimeTransportReady(sys)) {
                    state.boundaryReached = false;
                    break;
                }
#endif
                sys.otaBootAwaitingHealth = false;
                sys.ble.PublishVerifiedRunningFirmwareDigest(state.imageSha256.data());
                sys.ble.PublishOtaRuntimeAccepted();
                logger::Info("ota_boot: running image accepted after health boundary startup_complete_ms=%lu post_startup_ms=%lu target_ms=%lu",
                    static_cast<unsigned long>(state.startupCompletedAtMs),
                    static_cast<unsigned long>(fxe::Millis() - state.startupCompletedAtMs),
                    static_cast<unsigned long>(OTA_HEALTH_COMPLETION_TARGET_MS));
                state.commitFinished = true;
                break;
            case firmware::OtaBootHealthCommitResult::NoPendingImage:
                NoteHealthRetry(state, result);
                break;
            case firmware::OtaBootHealthCommitResult::Unsupported:
            case firmware::OtaBootHealthCommitResult::RollbackRequested:
                state.commitFinished = true;
                break;
            case firmware::OtaBootHealthCommitResult::Pending:
                break;
            default:
                // Unavailable or changed selection is not failed runtime health.
                NoteHealthRetry(state, result);
                break;
        }
        if (state.healthTicket == 0U) state.sinceCommitAttempt.Reset();
    }

    if (!state.crashGuardHealthy &&
        state.sinceLoopStart.HasElapsed(CRASH_GUARD_HEALTH_BOUNDARY_MS)) {
        sys.bootGuard.MarkBootHealthy();
        state.crashGuardHealthy = true;
    }
    return state.commitFinished;
}

} // namespace firmware_internal

#endif
