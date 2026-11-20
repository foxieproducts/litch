#include <cstdio>

#include "firmware/usb_device_power_descriptor.hpp"

namespace {

int failures = 0;

void Check(const bool value, const char* message) {
    if (!value) {
        std::printf("[FAIL] %s\n", message);
        ++failures;
    }
}

} // namespace

int main() {
    std::puts("[USB_DEVICE_POWER_DESCRIPTOR] start");

    const firmware::UsbDevicePowerDescriptorPolicy policy =
        firmware::S3_USB_DEVICE_POWER_DESCRIPTOR;
    Check(policy.attributes == 0U,
          "S3 descriptor declares VBUS-powered operation");
    Check(policy.configuredMaxPowerMa == 500U,
          "S3 descriptor requests USB 2.0 configured high power");

    std::printf("[USB_DEVICE_POWER_DESCRIPTOR] %s\n",
                failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
