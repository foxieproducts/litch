#include <cstdio>

#include "modules/ble_ota_reboot_grace.hpp"

namespace {

bool Expect(const bool condition, const char* const message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        return false;
    }
    return true;
}

bool TestRebootWaitsForBoundedNotifyGrace() {
    constexpr uint32_t armedAtMs = 1000U;
    return Expect(!modules::IsBleOtaRebootGraceElapsed(
                      armedAtMs +
                          modules::BLE_OTA_REBOOT_NOTIFY_GRACE_MS - 1U,
                      armedAtMs),
                  "reboot waits for the full notification grace") &&
           Expect(modules::IsBleOtaRebootGraceElapsed(
                      armedAtMs + modules::BLE_OTA_REBOOT_NOTIFY_GRACE_MS,
                      armedAtMs),
                  "reboot proceeds when the notification grace elapses");
}

bool TestRebootGraceHandlesMillisWrap() {
    constexpr uint32_t armedAtMs = 0xFFFFFF00U;
    const uint32_t readyAtMs =
        armedAtMs + modules::BLE_OTA_REBOOT_NOTIFY_GRACE_MS;
    return Expect(modules::IsBleOtaRebootGraceElapsed(readyAtMs, armedAtMs),
                  "notification grace is wrap-safe");
}

} // namespace

int main() {
    if (!TestRebootWaitsForBoundedNotifyGrace() ||
        !TestRebootGraceHandlesMillisWrap()) {
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
