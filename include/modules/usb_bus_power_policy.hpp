#pragma once

#include <cstdint>

namespace modules {

enum class UsbBusPowerState : uint8_t {
    Unconfigured = 0,
    Configured,
    Suspended,
};

struct UsbPowerAuthority {
    uint16_t maxTotalCurrentMa = 0U;
    bool switchedRailMayRemainEnabled = false;
};

inline constexpr uint16_t S3_DEFAULT_USB_UNCONFIGURED_TOTAL_CURRENT_MA = 100U;
inline constexpr uint16_t S3_DEFAULT_USB_CONFIGURED_TOTAL_CURRENT_MA = 450U;
// USB 2.0 permits 2.5 mA average during suspend. Whole-milliamp policy uses
// the next lower value rather than rounding that ceiling upward.
inline constexpr uint16_t S3_DEFAULT_USB_SUSPENDED_TOTAL_CURRENT_MA = 2U;
inline constexpr uint16_t S3_CONFIRMED_TYPE_C_TOTAL_CURRENT_MA = 1234U;

inline UsbPowerAuthority SelectS3UsbPowerAuthority(
    const bool typeCCurrentAboveDefaultConfirmed,
    const UsbBusPowerState busState) {
    if (typeCCurrentAboveDefaultConfirmed) {
        return UsbPowerAuthority{S3_CONFIRMED_TYPE_C_TOTAL_CURRENT_MA, true};
    }

    switch (busState) {
    case UsbBusPowerState::Configured:
        return UsbPowerAuthority{
            S3_DEFAULT_USB_CONFIGURED_TOTAL_CURRENT_MA, true};
    case UsbBusPowerState::Suspended:
        return UsbPowerAuthority{
            S3_DEFAULT_USB_SUSPENDED_TOTAL_CURRENT_MA, false};
    case UsbBusPowerState::Unconfigured:
    default:
        return UsbPowerAuthority{
            S3_DEFAULT_USB_UNCONFIGURED_TOTAL_CURRENT_MA, false};
    }
}

} // namespace modules
