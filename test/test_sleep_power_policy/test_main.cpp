#include <cstdio>

#include "firmware/sleep_controller.hpp"
#include "firmware/sleep_power_policy.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }

    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[SLEEP_POWER_POLICY] start");

    Check(firmware::LightSleepTimerWakeIntervalUs() == 250000ULL, "timer wake interval stays at 250ms");
    Check(!firmware::ShouldEnableLightSleepTimerWake(true), "detached USB never needs timer wake fallback");

    Check(firmware::IsSleepEntrySafeToTearDown(true, true),
          "sleep teardown requires released buttons and a configured wake source");
    Check(!firmware::IsSleepEntrySafeToTearDown(false, true),
          "held wake button blocks sleep teardown");
    Check(!firmware::IsSleepEntrySafeToTearDown(true, false),
          "missing wake source blocks sleep teardown");
    Check(!firmware::IsSleepEntrySafeToTearDown(false, false),
          "held wake button remains unsafe when wake setup also failed");

    Check(!firmware::ShouldRestoreRtcWakePinsForRuntime(false, false),
          "cold boot does not require RTC wake-pin restoration");
    Check(firmware::ShouldRestoreRtcWakePinsForRuntime(true, false),
          "deep-sleep reset restores RTC wake pins before runtime input");
    Check(firmware::ShouldRestoreRtcWakePinsForRuntime(false, true),
          "EXT1 wake restores RTC wake pins even if reset metadata is incomplete");

    firmware::SleepController nativeSleep;
    Check(nativeSleep.CanBeginSleepTeardown(), "native preflight has no held hardware buttons");
    Check(!nativeSleep.PrepareWakeSourceForSleep(),
          "native wake preparation never claims a hardware wake source");
    Check(!nativeSleep.ConfigureWakeOnButtons(), "native wake setup never claims a hardware wake source");
    Check(!nativeSleep.EnterSleepNow(), "native sleep entry reports that hardware sleep was not entered");

#ifdef LITCH_RELEASE_PROFILE
    Check(!firmware::ShouldEnableLightSleepTimerWake(false), "release profile uses pure GPIO wake when USB stays attached");
#else
    Check(firmware::ShouldEnableLightSleepTimerWake(false), "non-release profile keeps timer wake for attached USB debugging");
#endif

    if (g_failures == 0) {
        std::puts("[SLEEP_POWER_POLICY] ok");
        return 0;
    }

    std::printf("[SLEEP_POWER_POLICY] failures=%d\n", g_failures);
    return 1;
}
