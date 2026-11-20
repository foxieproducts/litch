#include <cstdio>
#include <initializer_list>

#include "modules/usb_bus_power_policy.hpp"

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
    using modules::SelectS3UsbPowerAuthority;
    using modules::UsbBusPowerState;

    std::puts("[USB_BUS_POWER_POLICY] start");

    const auto unconfigured =
        SelectS3UsbPowerAuthority(false, UsbBusPowerState::Unconfigured);
    Check(unconfigured.maxTotalCurrentMa ==
              modules::S3_DEFAULT_USB_UNCONFIGURED_TOTAL_CURRENT_MA &&
              !unconfigured.switchedRailMayRemainEnabled,
          "Default USB Power before configuration keeps the switched rail off");

    const auto configured =
        SelectS3UsbPowerAuthority(false, UsbBusPowerState::Configured);
    Check(configured.maxTotalCurrentMa ==
              modules::S3_DEFAULT_USB_CONFIGURED_TOTAL_CURRENT_MA &&
              configured.switchedRailMayRemainEnabled,
          "configured Default USB Power uses the 450mA Litch envelope");

    const auto suspended =
        SelectS3UsbPowerAuthority(false, UsbBusPowerState::Suspended);
    Check(suspended.maxTotalCurrentMa ==
              modules::S3_DEFAULT_USB_SUSPENDED_TOTAL_CURRENT_MA &&
              !suspended.switchedRailMayRemainEnabled,
          "suspended Default USB Power uses a sub-2.5mA integer ceiling");

    for (const UsbBusPowerState state : {
             UsbBusPowerState::Unconfigured,
             UsbBusPowerState::Configured,
             UsbBusPowerState::Suspended,
         }) {
        const auto typeCCurrent = SelectS3UsbPowerAuthority(true, state);
        Check(typeCCurrent.maxTotalCurrentMa ==
                  modules::S3_CONFIRMED_TYPE_C_TOTAL_CURRENT_MA &&
                  typeCCurrent.switchedRailMayRemainEnabled,
              "confirmed Type-C current is independent of USB data-bus state");
    }

    std::printf("[USB_BUS_POWER_POLICY] %s\n",
                failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
