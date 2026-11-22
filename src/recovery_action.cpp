#include "firmware/recovery_action.hpp"

namespace firmware {

RecoveryActionFailure ExecuteRecoveryActionSequence(
    const RecoveryAction action,
    IRecoveryActionTarget& target) {
    switch (action) {
        case RecoveryAction::None:
            return RecoveryActionFailure::None;

        case RecoveryAction::RetryBoot:
            target.PrepareForRetryReboot();
            target.Restart();
            return RecoveryActionFailure::RestartReturned;

        case RecoveryAction::FactoryReset:
            if (!target.DeletePersistedState()) {
                return RecoveryActionFailure::PersistedStateDeleteFailed;
            }
            target.ClearBootGuard();
            target.AcknowledgeFactoryReset();
            target.Restart();
            return RecoveryActionFailure::RestartReturned;
    }

    return RecoveryActionFailure::None;
}

} // namespace firmware
