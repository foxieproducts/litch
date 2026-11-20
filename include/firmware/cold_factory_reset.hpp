#pragma once

#include "firmware/recovery_action.hpp"
#include "firmware/recovery_led_frame.hpp"

namespace hardware { struct BoardHardwareProfile; }

namespace firmware {

struct ColdFactoryResetBoot {
    bool coldPowerOn = false;
    bool deepSleepWake = false;
};

/** \brief Platform I/O for the shared cold-power consent and acknowledgement. */
class ColdFactoryResetTarget : public IRecoveryActionTarget {
  public:
    ColdFactoryResetTarget(RecoveryPolicy& policy,
                           const hardware::BoardHardwareProfile& profile);

    virtual RecoveryControlLevels ReadResetControls() = 0;
    virtual bool DeliverResetFrame(const hardware::PhysicalLedFrame& frame) = 0;
    virtual void WaitForResetInput(uint32_t durationMs) = 0;

    RecoveryPolicy& Policy() { return m_policy; }
    bool ShowResetControls(modules::Rgb color);
    void AcknowledgeFactoryReset() final;

  private:
    RecoveryPolicy& m_policy;
    const hardware::BoardHardwareProfile& m_profile;
};

/** \brief Run before normal boot owners start. None allows startup to continue. */
RecoveryActionFailure RunColdFactoryReset(ColdFactoryResetBoot boot,
                                          ColdFactoryResetTarget& target);

} // namespace firmware
