#pragma once

#ifdef ARDUINO

#include "firmware/recovery_policy.hpp"

namespace firmware_internal {

struct FirmwareSystem;

firmware::RecoveryCause DetectBootRecoveryCause(
    const FirmwareSystem& sys);

void EnsurePowerRailInitialized(FirmwareSystem& sys);

void EnterSafeModeRecovery(FirmwareSystem& sys,
                           firmware::RecoveryCause cause);

void RunSafeModeRecovery(FirmwareSystem& sys);

void MaybeRunFactoryResetBootFlow(FirmwareSystem& sys);

} // namespace firmware_internal

#endif
