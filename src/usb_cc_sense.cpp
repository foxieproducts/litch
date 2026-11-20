#include "hardware/usb_cc_sense.hpp"

#ifdef ARDUINO
#include <Arduino.h>

#include "hardware/hardware.hpp"
#endif

namespace hardware {

UsbCcThresholds GetDefaultUsbCcThresholds() {
    // Empirical S3 ranges on current release hardware:
    // - inactive/orientation-opposite pin: ~55..106
    // - legacy USB-A to C default-current advertisement: ~496..545
    // - intermediate 1.5A-capable USB-C path: ~638
    // - strong 3.0A-capable USB-C advertisement: ~1080
    return DEFAULT_USB_CC_THRESHOLDS;
}

bool AreUsbCcThresholdsValid(const UsbCcThresholds& thresholds) {
    return thresholds.level500 > 0U &&
           thresholds.level500 < thresholds.level1500 &&
           thresholds.level1500 < thresholds.level3000;
}

UsbCcThresholds ResolveUsbCcThresholds(const uint32_t level500,
                                       const uint32_t level1500,
                                       const uint32_t level3000) {
    if (level500 > UINT16_MAX || level1500 > UINT16_MAX ||
        level3000 > UINT16_MAX) {
        return GetDefaultUsbCcThresholds();
    }

    const UsbCcThresholds candidate{
        static_cast<uint16_t>(level500),
        static_cast<uint16_t>(level1500),
        static_cast<uint16_t>(level3000),
    };
    return AreUsbCcThresholdsValid(candidate)
               ? candidate
               : GetDefaultUsbCcThresholds();
}

bool UsbCcLevelAdvertisesAtLeast1500Ma(const UsbCcLevel level) {
    return level == UsbCcLevel::Ma1500 || level == UsbCcLevel::Ma3000;
}

uint16_t CalculateUsbCcTrimmedMean(const UsbCcSampleWindow& samples) {
    uint32_t sum = 0U;
    uint16_t minimum = UINT16_MAX;
    uint16_t maximum = 0U;
    for (const uint16_t sample : samples) {
        sum += sample;
        minimum = (sample < minimum) ? sample : minimum;
        maximum = (sample > maximum) ? sample : maximum;
    }

    constexpr uint32_t FILTERED_SAMPLE_COUNT =
        USB_CC_SAMPLE_WINDOW_SIZE - 2U;
    const uint32_t trimmedSum = sum - minimum - maximum;
    return static_cast<uint16_t>(
        (trimmedSum + (FILTERED_SAMPLE_COUNT / 2U)) /
        FILTERED_SAMPLE_COUNT);
}

uint16_t CalculateUsbCcMedian(const UsbCcMonitorSampleWindow& samples) {
    uint32_t sum = 0U;
    uint16_t minimum = UINT16_MAX;
    uint16_t maximum = 0U;
    for (const uint16_t sample : samples) {
        sum += sample;
        minimum = (sample < minimum) ? sample : minimum;
        maximum = (sample > maximum) ? sample : maximum;
    }
    return static_cast<uint16_t>(sum - minimum - maximum);
}

uint16_t GetUsbCcStrongConfirmationRawThreshold(
    const UsbCcThresholds& thresholds) {
    if (!AreUsbCcThresholdsValid(thresholds)) {
        return UINT16_MAX;
    }

    const uint32_t guarded =
        static_cast<uint32_t>(thresholds.level1500) +
        USB_CC_STRONG_CONFIRMATION_GUARD_RAW;
    return static_cast<uint16_t>(
        (guarded < thresholds.level3000)
            ? guarded
            : thresholds.level3000);
}

bool UsbCcReadingsMeetStrongConfirmationThreshold(
    const uint16_t cc1,
    const uint16_t cc2,
    const UsbCcThresholds& thresholds) {
    if (!AreUsbCcThresholdsValid(thresholds)) {
        return false;
    }

    const bool cc1Active =
        ClassifyUsbCcRaw(cc1, thresholds) != UsbCcLevel::Unknown;
    const bool cc2Active =
        ClassifyUsbCcRaw(cc2, thresholds) != UsbCcLevel::Unknown;
    if (cc1Active == cc2Active) {
        return false;
    }

    const uint16_t activeRaw = cc1Active ? cc1 : cc2;
    return activeRaw >= GetUsbCcStrongConfirmationRawThreshold(thresholds);
}

UsbCcActivePin SelectUsbCcActivePin(
    const uint16_t cc1,
    const uint16_t cc2,
    const UsbCcThresholds& thresholds) {
    const bool cc1Active =
        ClassifyUsbCcRaw(cc1, thresholds) != UsbCcLevel::Unknown;
    const bool cc2Active =
        ClassifyUsbCcRaw(cc2, thresholds) != UsbCcLevel::Unknown;
    if (cc1Active == cc2Active) {
        return UsbCcActivePin::None;
    }
    return cc1Active ? UsbCcActivePin::Cc1 : UsbCcActivePin::Cc2;
}

UsbCcActivePinTrackingUpdate UpdateUsbCcActivePinTracking(
    const UsbCcActivePin currentPin,
    const bool sourceAuthorized,
    const UsbCcActivePin detectedPin) {
    if (sourceAuthorized) {
        return UsbCcActivePinTrackingUpdate{
            currentPin,
            false,
            currentPin != UsbCcActivePin::None && detectedPin == currentPin};
    }

    return UsbCcActivePinTrackingUpdate{
        detectedPin,
        detectedPin != currentPin,
        false};
}

UsbCcLevel ClassifyUsbCcRaw(const uint16_t raw, const UsbCcThresholds& thresholds) {
    if (!AreUsbCcThresholdsValid(thresholds)) {
        return UsbCcLevel::Unknown;
    }

    if (raw >= thresholds.level3000) {
        return UsbCcLevel::Ma3000;
    }

    if (raw >= thresholds.level1500) {
        return UsbCcLevel::Ma1500;
    }

    if (raw >= thresholds.level500) {
        return UsbCcLevel::Ma500;
    }

    return UsbCcLevel::Unknown;
}

UsbCcLevel MaxUsbCcLevel(const UsbCcLevel lhs, const UsbCcLevel rhs) {
    return (static_cast<uint8_t>(lhs) >= static_cast<uint8_t>(rhs)) ? lhs : rhs;
}

UsbCcLevel ClassifyUsbCcReadings(const uint16_t cc1, const uint16_t cc2, const UsbCcThresholds& thresholds) {
    const UsbCcLevel level1 = ClassifyUsbCcRaw(cc1, thresholds);
    const UsbCcLevel level2 = ClassifyUsbCcRaw(cc2, thresholds);
    if (level1 != UsbCcLevel::Unknown && level2 != UsbCcLevel::Unknown) {
        return UsbCcLevel::Unknown;
    }
    return MaxUsbCcLevel(level1, level2);
}

const char* UsbCcLevelToString(const UsbCcLevel level) {
    switch (level) {
        case UsbCcLevel::Ma500:
            return "500mA";
        case UsbCcLevel::Ma1500:
            return "1.5A";
        case UsbCcLevel::Ma3000:
            return "3A";
        case UsbCcLevel::Unknown:
        default:
            return "unknown";
    }
}

const char* UsbCcActivePinToString(const UsbCcActivePin pin) {
    switch (pin) {
        case UsbCcActivePin::Cc1:
            return "cc1";
        case UsbCcActivePin::Cc2:
            return "cc2";
        case UsbCcActivePin::None:
        default:
            return "none";
    }
}

#ifdef ARDUINO
UsbCcFilteredReading ReadUsbCcFiltered(const uint8_t pin) {
    const uint16_t priming = analogRead(pin);
    delay(2);

    UsbCcSampleWindow samples{};
    uint16_t minimum = UINT16_MAX;
    uint16_t maximum = 0U;
    for (uint16_t& sample : samples) {
        sample = analogRead(pin);
        minimum = (sample < minimum) ? sample : minimum;
        maximum = (sample > maximum) ? sample : maximum;
        delay(2);
    }
    return UsbCcFilteredReading{
        CalculateUsbCcTrimmedMean(samples), minimum, maximum, priming};
}

uint16_t ReadUsbCcMonitorMedian(const uint8_t pin) {
    (void)analogRead(pin);

    UsbCcMonitorSampleWindow samples{};
    for (uint16_t& sample : samples) {
        sample = analogRead(pin);
    }
    return CalculateUsbCcMedian(samples);
}

UsbCcReadings ReadUsbCcPins() {
    const UsbCcFilteredReading cc1 =
        ReadUsbCcFiltered(hardware::PIN_USB_CC1);
    const UsbCcFilteredReading cc2 =
        ReadUsbCcFiltered(hardware::PIN_USB_CC2);
    UsbCcReadings readings;
    readings.cc1 = cc1.value;
    readings.cc2 = cc2.value;
    readings.cc1Minimum = cc1.minimum;
    readings.cc1Maximum = cc1.maximum;
    readings.cc1Priming = cc1.priming;
    readings.cc2Minimum = cc2.minimum;
    readings.cc2Maximum = cc2.maximum;
    readings.cc2Priming = cc2.priming;
    return readings;
}
#endif

} // namespace hardware
