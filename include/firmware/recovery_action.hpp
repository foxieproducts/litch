#pragma once

#include <cstdint>

#include "firmware/recovery_policy.hpp"

namespace firmware {

/**
 * \brief Narrow device boundary used to verify recovery mutation ordering.
 * \note Restart is expected not to return on hardware. Returning is treated
 *       as an explicit action failure by ExecuteRecoveryActionSequence().
 */
class IRecoveryActionTarget {
  public:
    virtual ~IRecoveryActionTarget() = default;

    virtual void PrepareForRetryReboot() = 0;
    virtual bool DeletePersistedState() = 0;
    virtual void ClearBootGuard() = 0;
    virtual void AcknowledgeFactoryReset() {}
    virtual void Restart() = 0;
};

/**
 * \brief Execute one already-authorized recovery action in safety order.
 * \return None for no action, otherwise the reason execution could not finish.
 */
RecoveryActionFailure ExecuteRecoveryActionSequence(
    RecoveryAction action,
    IRecoveryActionTarget& target);

} // namespace firmware
