#include <cstdio>

#include "modules/status_led_overlay.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[STATUS_LED_OVERLAY] start");

    modules::status_led_overlay::Buffer buffer;
    buffer.Clear();

    buffer.Set(5U, modules::status_led_overlay::PRIORITY_OTA, modules::RGB_MAGENTA);
    buffer.Set(5U, modules::status_led_overlay::PRIORITY_BASE, modules::RGB_GREEN);

    uint8_t appliedIndex = 255U;
    modules::Rgb appliedColor = modules::RGB_BLACK;
    buffer.Apply([&](const uint8_t localStatusIndex, const modules::Rgb color) {
        appliedIndex = localStatusIndex;
        appliedColor = color;
    });

    Check(appliedIndex == 5U, "buffer applies the expected status LED index");
    Check(appliedColor.r == modules::RGB_MAGENTA.r && appliedColor.b == modules::RGB_MAGENTA.b,
          "lower-priority colors do not replace a higher-priority slot");

    buffer.Set(5U, modules::status_led_overlay::PRIORITY_BLE, modules::RGB_BLUE);
    buffer.Apply([&](const uint8_t localStatusIndex, const modules::Rgb color) {
        appliedIndex = localStatusIndex;
        appliedColor = color;
    });

    Check(appliedIndex == 5U, "higher-priority replacement keeps the same slot");
    Check(appliedColor.b == modules::RGB_BLUE.b && appliedColor.g == modules::RGB_BLUE.g,
          "higher-priority colors replace lower-priority content");

    if (g_failures == 0) {
        std::puts("[STATUS_LED_OVERLAY] ok");
        return 0;
    }

    std::printf("[STATUS_LED_OVERLAY] failures=%d\n", g_failures);
    return 1;
}