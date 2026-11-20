#pragma once

#ifdef ARDUINO

#include "firmware/sleep_entry_orchestration.hpp"

namespace firmware_internal {
struct FirmwareSystem;

void ConfigureLitchcraftPersistence(FirmwareSystem& sys);
bool BeginLifecyclePersistence(FirmwareSystem& sys);
bool LifecycleStoreSavePending(const FirmwareSystem& sys);
bool BeginPersistenceBeforeSleep(void* context);
firmware::SleepPersistenceStatus PollPersistenceBeforeSleep(void* context);
// Returns true while the reboot transition owns the loop, including its final
// success/failure tick. The caller keeps rendering while normal input is held.
bool PollPendingReboot(FirmwareSystem& sys);

} // namespace firmware_internal

#endif
