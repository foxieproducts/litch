#include "firmware/ota_boot_health.hpp"
#include "firmware/ota_platform_adapter.hpp"
#include "hardware/rmt_flash_arbiter.hpp"
#include "modules/ota_post_boot_executor.hpp"

#if defined(ARDUINO) && defined(LITCH_ESP32S3)
#include <sdkconfig.h>

#if !defined(CONFIG_APP_ROLLBACK_ENABLE) || !CONFIG_APP_ROLLBACK_ENABLE
#error "Litch S3 OTA health confirmation requires ESP-IDF app rollback"
#endif

// Arduino's weak default confirms PENDING_VERIFY before setup(). This strong
// application-owned symbol keeps rollback eligibility until the reducer
// reaches Litch's explicit health boundary.
extern "C" bool verifyRollbackLater() {
    return true;
}
#endif

namespace firmware {
namespace {

OtaBootHealthCommitResult Classify(const modules::OtaPostBootExecutionResult result) {
    switch (result) {
        case modules::OtaPostBootExecutionResult::Unsupported:
            return OtaBootHealthCommitResult::Unsupported;
        case modules::OtaPostBootExecutionResult::NoAction:
            return OtaBootHealthCommitResult::NoPendingImage;
        case modules::OtaPostBootExecutionResult::AwaitingHealth:
            return OtaBootHealthCommitResult::AwaitingHealth;
        case modules::OtaPostBootExecutionResult::Confirmed:
            return OtaBootHealthCommitResult::Confirmed;
        case modules::OtaPostBootExecutionResult::RollbackRequested:
            return OtaBootHealthCommitResult::RollbackRequested;
        case modules::OtaPostBootExecutionResult::SnapshotUnavailable:
            return OtaBootHealthCommitResult::SnapshotUnavailable;
        case modules::OtaPostBootExecutionResult::ImageEvidenceUnavailable:
            return OtaBootHealthCommitResult::ImageEvidenceUnavailable;
        case modules::OtaPostBootExecutionResult::EvidenceChanged:
            return OtaBootHealthCommitResult::EvidenceChanged;
        case modules::OtaPostBootExecutionResult::NoValidFallback:
            return OtaBootHealthCommitResult::NoValidFallback;
        case modules::OtaPostBootExecutionResult::ConfirmFailed:
            return OtaBootHealthCommitResult::ConfirmFailed;
        case modules::OtaPostBootExecutionResult::ConfirmReadbackFailed:
            return OtaBootHealthCommitResult::ConfirmReadbackFailed;
        case modules::OtaPostBootExecutionResult::RollbackFailed:
            return OtaBootHealthCommitResult::RollbackFailed;
    }
    return OtaBootHealthCommitResult::SnapshotUnavailable;
}

OtaBootHealthCommitResult PollHealth(
    RuntimeBootHealthState& state, const modules::OtaPostBootHealth health) {
    using Status = OtaBootHealthRequestStatus;
    if (state.healthTicket == 0U) {
        switch (BeginEsp32OtaBootHealth(health, state.healthTicket)) {
            case Status::Pending:
                state.requestedHealth = health;
                return OtaBootHealthCommitResult::Pending;
            case Status::Busy:
                return OtaBootHealthCommitResult::Pending;
            case Status::Unsupported:
                return OtaBootHealthCommitResult::Unsupported;
            default:
                return OtaBootHealthCommitResult::SnapshotUnavailable;
        }
    }
    if (state.requestedHealth == modules::OtaPostBootHealth::NotReported &&
        health != modules::OtaPostBootHealth::NotReported) {
        const auto resolved = ResolveEsp32OtaBootHealth(state.healthTicket, health);
        if (resolved == Status::Busy) return OtaBootHealthCommitResult::Pending;
        if (resolved != Status::Pending) return OtaBootHealthCommitResult::SnapshotUnavailable;
        state.requestedHealth = health;
    }
    modules::OtaPostBootExecutionResult result{};
    const auto status = PollEsp32OtaBootHealth(state.healthTicket, result, state.imageSha256);
    if (status == Status::Pending || status == Status::Busy)
        return OtaBootHealthCommitResult::Pending;
    state.healthTicket = 0U;
    // The native job cannot be cancelled once submitted. Retire its response
    // without borrowing normal acceptance after runtime enters recovery.
    if (status != Status::Complete || state.requestedHealth != health)
        return OtaBootHealthCommitResult::SnapshotUnavailable;
    return Classify(result);
}

} // namespace

OtaBootHealthCommitResult CheckPendingOtaAtBoot(hardware::RmtFlashArbiter& arbiter) {
    (void)arbiter;
    return Classify(ProcessEsp32OtaBootHealth(modules::OtaPostBootHealth::NotReported));
}

OtaBootHealthCommitResult PreparePendingOtaBootHealth(RuntimeBootHealthState& state) {
    return PollHealth(state, modules::OtaPostBootHealth::NotReported);
}

OtaBootHealthCommitResult CommitPendingOtaAtHealthBoundary(RuntimeBootHealthState& state) {
    return PollHealth(state, modules::OtaPostBootHealth::Passed);
}

OtaBootHealthCommitResult ReportPendingOtaBootFailure(RuntimeBootHealthState& state) {
    return PollHealth(state, modules::OtaPostBootHealth::Failed);
}

} // namespace firmware
