#ifdef ARDUINO

#include <Arduino.h>

#include "firmware/firmware_console_internal.hpp"

#include "firmware/firmware_internal.hpp"
#include "firmware/p1x_deep_wake.hpp"
#include "firmware/recovery_status.hpp"
#include "foxie_embedded/millis.hpp"

namespace firmware_internal::console_commands {
bool SafeModeCommand(Console& console, const Console::Args& args) {
    auto& sys = GetSystem();

    const std::string_view sub = args.Has(1) ? args.Get(1) : std::string_view("status");
    if (!(sub == "status" || sub == "clear" || sub == "reboot")) {
        console.PrintLn("usage: safemode [status|clear|reboot]");
        return false;
    }

    if (sub == "clear") {
        sys.bootGuard.ArmRetryReboot();
    }

    const runtime::BootGuard::Snapshot snapshot = sys.bootGuard.GetSnapshot();
    const firmware::RecoveryStatusLines status =
        firmware::ComposeRecoveryStatusLines(
            snapshot, sys.safeModeActive, sys.recoveryPolicy.GetSnapshot());
    console.PrintLn("%s", status.safemodeSummary.data());
    console.PrintLn("%s", status.bootGuard.data());
    console.PrintLn("%s", status.recovery.data());

    if (sub == "clear") {
        console.PrintLn(
            "safemode: later retry armed; reboot required to attempt normal boot");
        return true;
    }

    if (sub == "reboot") {
        sys.bootGuard.PrepareForRetryReboot();
        console.PrintLn("safemode: retrying normal boot without deleting data");
        delay(50);
        ESP.restart();
        return true;
    }

    return true;
}

bool SleepCommand(Console& console, const Console::Args& args) {
    auto& sys = GetSystem();
    const std::string_view sub =
        args.Has(1) ? args.Get(1) : std::string_view("enter");
    uint32_t wakeAfterMs = 0U;
    if (args.overflow ||
        (sub == "status" ? args.argc > 2U : sub != "enter" || args.argc > 3U) ||
        (sub == "enter" && args.Has(2) &&
         (!TryParseU32(args.Get(2), wakeAfterMs) ||
          !firmware::SleepController::IsValidWakeAfterMs(wakeAfterMs)))) {
        console.PrintLn("usage: sleep [enter [wake_ms]|status] (wake_ms: 0 or 100..60000; timer requires S3 deep sleep)");
        return false;
    }

    if (sub == "status") {
        console.PrintLn(
            "sleep: board_revision=%u s3=%u p1x_standard=%u release_stable_ms=%u effective_button_wake_mask=0x%llx",
            static_cast<unsigned>(sys.runtimeHardware.resolved.revision),
            static_cast<unsigned>(sys.runtimeHardware.resolved.isS3 ? 1U : 0U),
            static_cast<unsigned>(sys.sleep.IsP1XDeepWakeEnabled() ? 1U : 0U),
            static_cast<unsigned>(firmware::p1x_deep_wake::RELEASE_STABLE_SAMPLES *
                                  firmware::p1x_deep_wake::POLL_INTERVAL_US / 1000U),
            static_cast<unsigned long long>(
                sys.sleep.GetConfiguredButtonWakeMask()));
        console.PrintLn(
            "sleep: state=%u origin=%u remaining_ms=%lu cancel=%u preflight_failure=%u timer_wake=%u",
            static_cast<unsigned>(sys.sleep.GetState()),
            static_cast<unsigned>(sys.sleep.GetRequestOrigin()),
            static_cast<unsigned long>(
                sys.sleep.GetRemainingCountdownMs()),
            static_cast<unsigned>(sys.sleep.GetCancelCause()),
            static_cast<unsigned>(sys.sleep.GetLastPreparationFailure()),
            static_cast<unsigned>(sys.sleep.WokeFromTimer() ? 1U : 0U));
        return true;
    }

    if (!sys.sleep.RequestSleep(
            firmware::SleepRequestOrigin::Console, fxe::Millis(), false,
            wakeAfterMs)) {
        console.PrintLn("sleep: request rejected by current transition or wake capability");
        return false;
    }
    console.PrintLn(
        "sleep: requested; fading after the count-up; normal activity ignored; save and wake safety checks remain active");
    return true;
}

size_t SleepComplete(const Console::Args&,
                     const size_t argIndex,
                     const char* prefix,
                     const char** outMatches,
                     const size_t outMatchesCap) {
    if (argIndex != 1U) {
        return 0U;
    }
    size_t count = 0U;
    count = AddMatchIfPrefix(
        "enter", prefix, outMatches, count, outMatchesCap);
    count = AddMatchIfPrefix(
        "status", prefix, outMatches, count, outMatchesCap);
    return count;
}

} // namespace firmware_internal::console_commands

#endif
