#include "firmware/recovery_status.hpp"

#include <cstdio>

namespace firmware {

const char* RecoveryCauseToken(const RecoveryCause cause) {
    switch (cause) {
        case RecoveryCause::EarlyCrash:
            return "early_crash";
        case RecoveryCause::PersistedStateCorrupt:
            return "persisted_state_corrupt";
        case RecoveryCause::UpdatePending:
            return "update_pending";
        case RecoveryCause::UpdateFailed:
            return "update_failed";
        case RecoveryCause::ManualFactoryReset:
            return "manual_factory_reset";
        case RecoveryCause::None:
        default:
            return "none";
    }
}

const char* RecoveryStateToken(const RecoveryState state) {
    switch (state) {
        case RecoveryState::WaitForRelease:
            return "wait_for_release";
        case RecoveryState::Ready:
            return "ready";
        case RecoveryState::RetryHold:
            return "retry_hold";
        case RecoveryState::DestructiveHold:
            return "destructive_hold";
        case RecoveryState::CancelWindow:
            return "cancel_window";
        case RecoveryState::Cancelled:
            return "cancelled";
        case RecoveryState::ActionPending:
            return "action_pending";
        case RecoveryState::ActionFailed:
            return "action_failed";
        case RecoveryState::Inactive:
        default:
            return "inactive";
    }
}

const char* RecoveryOwnerToken(const RecoveryOwner owner) {
    switch (owner) {
        case RecoveryOwner::P1:
            return "p1";
        case RecoveryOwner::P2:
            return "p2";
        case RecoveryOwner::None:
        default:
            return "none";
    }
}

const char* RecoveryActionToken(const RecoveryAction action) {
    switch (action) {
        case RecoveryAction::RetryBoot:
            return "retry_boot";
        case RecoveryAction::FactoryReset:
            return "factory_reset";
        case RecoveryAction::None:
        default:
            return "none";
    }
}

const char* RecoveryActionFailureToken(
    const RecoveryActionFailure failure) {
    switch (failure) {
        case RecoveryActionFailure::PersistedStateDeleteFailed:
            return "persisted_state_delete_failed";
        case RecoveryActionFailure::RestartReturned:
            return "restart_returned";
        case RecoveryActionFailure::None:
        default:
            return "none";
    }
}

RecoveryStatusLines ComposeRecoveryStatusLines(
    const runtime::BootGuard::Snapshot& bootGuard,
    const bool systemSafeModeActive,
    const RecoverySnapshot& recovery) {
    RecoveryStatusLines lines;
    std::snprintf(lines.safemodeSummary.data(),
                  lines.safemodeSummary.size(),
                  "safemode: active=%u sys_flag=%u retry_reboot_armed=%u",
                  static_cast<unsigned>(bootGuard.safeModeActive ? 1U : 0U),
                  static_cast<unsigned>(systemSafeModeActive ? 1U : 0U),
                  static_cast<unsigned>(bootGuard.retryRebootArmed ? 1U : 0U));
    std::snprintf(lines.bootGuard.data(),
                  lines.bootGuard.size(),
                  "safemode: boots=%lu early_crashes=%lu last_reset_reason=%lu",
                  static_cast<unsigned long>(bootGuard.bootCount),
                  static_cast<unsigned long>(bootGuard.earlyCrashCount),
                  static_cast<unsigned long>(bootGuard.lastResetReason));
    std::snprintf(lines.recovery.data(),
                  lines.recovery.size(),
                  "recovery: cause=%s state=%s owner=%s progress=%u/1000 pending=%s failure=%s",
                  RecoveryCauseToken(recovery.cause),
                  RecoveryStateToken(recovery.state),
                  RecoveryOwnerToken(recovery.activeOwner),
                  static_cast<unsigned>(recovery.totalProgressPermille),
                  RecoveryActionToken(recovery.pendingAction),
                  RecoveryActionFailureToken(recovery.actionFailure));
    return lines;
}

} // namespace firmware
