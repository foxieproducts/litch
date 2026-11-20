#include "firmware/sleep_entry_orchestration.hpp"

namespace firmware {

namespace {

bool EntryHooksAreComplete(const SleepEntryHooks& hooks) {
    return hooks.clearLeds != nullptr && hooks.wait != nullptr &&
           hooks.shutdownAmbient != nullptr &&
           hooks.preparePowerRail != nullptr && hooks.enterSleep != nullptr &&
           hooks.restartAfterEntryReturn != nullptr;
}

} // namespace

SleepLifecycleResult RunSleepLifecycleStep(
    SleepController& controller,
    const SleepPreparationHooks& preparationHooks,
    const SleepEntryHooks& entryHooks) {
    if (!EntryHooksAreComplete(entryHooks)) {
        return SleepLifecycleResult::EntryHooksUnavailable;
    }

    switch (controller.RunSleepPreparation(preparationHooks)) {
        case SleepPreparationResult::NotPreparing:
            return SleepLifecycleResult::NotPreparing;
        case SleepPreparationResult::Pending:
            return SleepLifecycleResult::Pending;
        case SleepPreparationResult::Failed:
            return SleepLifecycleResult::PreparationFailed;
        case SleepPreparationResult::Authorized:
            break;
    }

    if (!controller.ConsumeSleepAuthorization()) {
        return SleepLifecycleResult::AuthorizationUnavailable;
    }

    if (!entryHooks.clearLeds(entryHooks.context)) {
        // Preserve the live rail and any RMT quarantine. A failed clear cannot
        // authorize teardown; use the existing restart recovery owner.
        entryHooks.restartAfterEntryReturn(entryHooks.context, false);
        return SleepLifecycleResult::LedQuiescenceFailed;
    }
    entryHooks.wait(entryHooks.context, SLEEP_LED_OFF_HOLD_MS);
    entryHooks.shutdownAmbient(entryHooks.context);
    entryHooks.preparePowerRail(entryHooks.context);
    entryHooks.wait(entryHooks.context, SLEEP_POWER_RAIL_OFF_HOLD_MS);
    const bool enteredSleep = entryHooks.enterSleep(entryHooks.context);
    entryHooks.restartAfterEntryReturn(entryHooks.context, enteredSleep);
    return SleepLifecycleResult::RestartReturned;
}

} // namespace firmware
