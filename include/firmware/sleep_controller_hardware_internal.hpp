#pragma once

#include "firmware/sleep_wake_pin_policy.hpp"
#include "hardware/board_hardware_profile.hpp"

namespace firmware::sleep_controller_internal {

bool StopI2cForSleep(const hardware::BoardHardwareProfile& profile);
void ApplyGpioLeakagePolicyForSleep(
    const hardware::BoardHardwareProfile& profile,
    const SleepWakePinSet& runtimeButtonPins,
    const SleepWakePinSet& preservedInputPins);
bool TryDetachUsbForSleep();

} // namespace firmware::sleep_controller_internal
