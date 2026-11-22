#pragma once

#include <cstdint>

#include "firmware/sleep_controller.hpp"

namespace firmware {

inline constexpr uint32_t SLEEP_LED_OFF_HOLD_MS = 50U;
inline constexpr uint32_t SLEEP_POWER_RAIL_OFF_HOLD_MS = 100U;

struct SleepEntryHooks {
    using StepFn = void (*)(void* context);
    using WaitFn = void (*)(void* context, uint32_t milliseconds);
    using EnterSleepFn = bool (*)(void* context);
    using RestartFn = void (*)(void* context, bool enteredSleep);

    void* context = nullptr;
    EnterSleepFn clearLeds = nullptr;
    WaitFn wait = nullptr;
    StepFn shutdownAmbient = nullptr;
    StepFn preparePowerRail = nullptr;
    EnterSleepFn enterSleep = nullptr;
    RestartFn restartAfterEntryReturn = nullptr;
};

enum class SleepLifecycleResult : uint8_t {
    NotPreparing = 0,
    Pending,
    PreparationFailed,
    EntryHooksUnavailable,
    AuthorizationUnavailable,
    RestartReturned,
    LedQuiescenceFailed,
};

/**
 * \brief Advances preparation and performs one authorized sleep teardown.
 *
 * Wake-source preparation is completed by `SleepController` before any entry
 * hook can run. If the hardware sleep transition returns after teardown, the
 * restart hook is always invoked before this function can return.
 */
SleepLifecycleResult RunSleepLifecycleStep(
    SleepController& controller,
    const SleepPreparationHooks& preparationHooks,
    const SleepEntryHooks& entryHooks);

} // namespace firmware
