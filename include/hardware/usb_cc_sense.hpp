#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hardware {

enum class UsbCcLevel : uint8_t {
    Unknown = 0,
    Ma500 = 1,
    Ma1500 = 2,
    Ma3000 = 3,
};

enum class UsbCcActivePin : uint8_t {
    None = 0,
    Cc1,
    Cc2,
};

struct UsbCcActivePinTrackingUpdate {
    UsbCcActivePin activePin = UsbCcActivePin::None;
    bool confirmationMustRestart = false;
    bool observationMatchesAuthorizedPin = false;
};

struct UsbCcThresholds {
    // Raw ADC thresholds for CC voltage levels.
    uint16_t level500 = 0;
    uint16_t level1500 = 0;
    uint16_t level3000 = 0;
};

/** \brief Calibrated USB-C CC thresholds for current release hardware. */
inline constexpr UsbCcThresholds DEFAULT_USB_CC_THRESHOLDS{320U, 580U, 850U};
inline constexpr uint16_t USB_CC_STRONG_CONFIRMATION_GUARD_RAW = 40U;
inline constexpr std::size_t USB_CC_SAMPLE_WINDOW_SIZE = 8U;
inline constexpr std::size_t USB_CC_MONITOR_SAMPLE_WINDOW_SIZE = 3U;

using UsbCcSampleWindow =
    std::array<uint16_t, USB_CC_SAMPLE_WINDOW_SIZE>;
using UsbCcMonitorSampleWindow =
    std::array<uint16_t, USB_CC_MONITOR_SAMPLE_WINDOW_SIZE>;

struct UsbCcReadings {
    uint16_t cc1 = 0;
    uint16_t cc2 = 0;
    uint16_t cc1Minimum = 0;
    uint16_t cc1Maximum = 0;
    uint16_t cc1Priming = 0;
    uint16_t cc2Minimum = 0;
    uint16_t cc2Maximum = 0;
    uint16_t cc2Priming = 0;
};

struct UsbCcFilteredReading {
    uint16_t value = 0;
    uint16_t minimum = 0;
    uint16_t maximum = 0;
    uint16_t priming = 0;
};

UsbCcThresholds GetDefaultUsbCcThresholds();
/** \brief Return whether all thresholds are nonzero and strictly ascending. */
[[nodiscard]] bool AreUsbCcThresholdsValid(const UsbCcThresholds& thresholds);
/** \brief Resolve persisted raw values, falling back when invalid or out of range. */
UsbCcThresholds ResolveUsbCcThresholds(uint32_t level500,
                                       uint32_t level1500,
                                       uint32_t level3000);
/** \brief Return whether a classified source advertises at least 1.5 A. */
bool UsbCcLevelAdvertisesAtLeast1500Ma(UsbCcLevel level);
/** \brief Reject one low and one high outlier, then average the sample window. */
uint16_t CalculateUsbCcTrimmedMean(const UsbCcSampleWindow& samples);
/** \brief Return the median of a three-sample monitoring window. */
uint16_t CalculateUsbCcMedian(const UsbCcMonitorSampleWindow& samples);
/** \brief Return the raw threshold required to begin strong-source confirmation. */
uint16_t GetUsbCcStrongConfirmationRawThreshold(
    const UsbCcThresholds& thresholds);
/** \brief Return whether exactly one CC pin clears the strong-entry guard band. */
bool UsbCcReadingsMeetStrongConfirmationThreshold(
    uint16_t cc1,
    uint16_t cc2,
    const UsbCcThresholds& thresholds);
/** \brief Select the one electrically active CC pin, or None when ambiguous. */
UsbCcActivePin SelectUsbCcActivePin(
    uint16_t cc1,
    uint16_t cc2,
    const UsbCcThresholds& thresholds);
/** \brief Bind confirmation to one pin and freeze it while authorized. */
UsbCcActivePinTrackingUpdate UpdateUsbCcActivePinTracking(
    UsbCcActivePin currentPin,
    bool sourceAuthorized,
    UsbCcActivePin detectedPin);
UsbCcLevel ClassifyUsbCcRaw(const uint16_t raw, const UsbCcThresholds& thresholds);
UsbCcLevel MaxUsbCcLevel(UsbCcLevel lhs, UsbCcLevel rhs);
UsbCcLevel ClassifyUsbCcReadings(uint16_t cc1, uint16_t cc2, const UsbCcThresholds& thresholds);
const char* UsbCcLevelToString(const UsbCcLevel level);
const char* UsbCcActivePinToString(UsbCcActivePin pin);

#ifdef ARDUINO
UsbCcFilteredReading ReadUsbCcFiltered(uint8_t pin);
uint16_t ReadUsbCcMonitorMedian(uint8_t pin);
UsbCcReadings ReadUsbCcPins();
#endif

} // namespace hardware
