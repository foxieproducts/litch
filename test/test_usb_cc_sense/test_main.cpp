#include <array>
#include <cstdio>

#include "hardware/usb_cc_sense.hpp"
#include "modules/led_power_envelope.hpp"
#include "modules/settings_registry.hpp"

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
    const hardware::UsbCcThresholds thresholds = hardware::GetDefaultUsbCcThresholds();

    Check(hardware::AreUsbCcThresholdsValid(thresholds), "calibrated defaults form a valid threshold triple");
    Check(thresholds.level500 == 320U, "default 500mA threshold matches calibrated value");
    Check(thresholds.level1500 == 580U, "default 1.5A threshold matches calibrated value");
    Check(thresholds.level3000 == 850U, "default 3.0A threshold matches calibrated value");

    const fxe::SettingDescriptor* registry500 = modules::settings_registry::LookupByKey("USB_CC_THRESH_500");
    const fxe::SettingDescriptor* registry1500 = modules::settings_registry::LookupByKey("USB_CC_THRESH_1500");
    const fxe::SettingDescriptor* registry3000 = modules::settings_registry::LookupByKey("USB_CC_THRESH_3000");
    Check(registry500 != nullptr && registry500->defaultUint == thresholds.level500,
          "registry publishes calibrated 500mA default");
    Check(registry1500 != nullptr && registry1500->defaultUint == thresholds.level1500,
          "registry publishes calibrated 1.5A default");
    Check(registry3000 != nullptr && registry3000->defaultUint == thresholds.level3000,
          "registry publishes calibrated 3.0A default");

    Check(!hardware::AreUsbCcThresholdsValid(hardware::UsbCcThresholds{0U, 580U, 850U}),
          "zero 500mA threshold is invalid");
    Check(!hardware::AreUsbCcThresholdsValid(hardware::UsbCcThresholds{320U, 0U, 850U}),
          "zero 1.5A threshold is invalid");
    Check(!hardware::AreUsbCcThresholdsValid(hardware::UsbCcThresholds{320U, 580U, 0U}),
          "zero 3.0A threshold is invalid");
    Check(!hardware::AreUsbCcThresholdsValid(hardware::UsbCcThresholds{320U, 320U, 850U}),
          "equal lower thresholds are invalid");
    Check(!hardware::AreUsbCcThresholdsValid(hardware::UsbCcThresholds{320U, 850U, 850U}),
          "equal upper thresholds are invalid");
    Check(!hardware::AreUsbCcThresholdsValid(hardware::UsbCcThresholds{580U, 320U, 850U}),
          "descending thresholds are invalid");
    Check(hardware::AreUsbCcThresholdsValid(hardware::UsbCcThresholds{1U, 2U, 65535U}),
          "strictly ascending nonzero boundary thresholds are valid");

    const hardware::UsbCcThresholds custom =
        hardware::ResolveUsbCcThresholds(100U, 200U, 300U);
    Check(custom.level500 == 100U && custom.level1500 == 200U &&
              custom.level3000 == 300U,
          "valid persisted thresholds are retained");
    const hardware::UsbCcThresholds zeroFallback =
        hardware::ResolveUsbCcThresholds(320U, 0U, 850U);
    Check(zeroFallback.level1500 == thresholds.level1500,
          "zero persisted threshold falls back to calibrated defaults");
    const hardware::UsbCcThresholds orderFallback =
        hardware::ResolveUsbCcThresholds(580U, 320U, 850U);
    Check(orderFallback.level500 == thresholds.level500,
          "descending persisted thresholds fall back to calibrated defaults");
    const hardware::UsbCcThresholds rangeFallback =
        hardware::ResolveUsbCcThresholds(320U, 580U, 65536U);
    Check(rangeFallback.level3000 == thresholds.level3000,
          "out-of-range persisted threshold falls back to calibrated defaults");

    Check(!hardware::UsbCcLevelAdvertisesAtLeast1500Ma(
              hardware::UsbCcLevel::Unknown),
          "unknown source is not treated as strong");
    Check(!hardware::UsbCcLevelAdvertisesAtLeast1500Ma(
              hardware::UsbCcLevel::Ma500),
          "default-current source is not treated as strong");
    Check(hardware::UsbCcLevelAdvertisesAtLeast1500Ma(
              hardware::UsbCcLevel::Ma1500),
          "1.5A source is treated as strong");
    Check(hardware::UsbCcLevelAdvertisesAtLeast1500Ma(
              hardware::UsbCcLevel::Ma3000),
          "3A source is treated as strong");

    Check(hardware::ClassifyUsbCcRaw(55U, thresholds) == hardware::UsbCcLevel::Unknown,
          "inactive pin stays unknown");
    Check(hardware::ClassifyUsbCcRaw(519U, thresholds) == hardware::UsbCcLevel::Ma500,
          "legacy USB-A to C cluster maps to 500mA");
    Check(hardware::ClassifyUsbCcRaw(545U, thresholds) == hardware::UsbCcLevel::Ma500,
          "upper legacy USB-A to C cluster still maps to 500mA");
    Check(hardware::ClassifyUsbCcRaw(638U, thresholds) == hardware::UsbCcLevel::Ma1500,
          "intermediate USB-C cluster maps to 1.5A");
    Check(hardware::ClassifyUsbCcRaw(1080U, thresholds) == hardware::UsbCcLevel::Ma3000,
          "strong USB-C cluster maps to 3.0A");

    Check(hardware::ClassifyUsbCcReadings(55U, 1084U, thresholds) == hardware::UsbCcLevel::Ma3000,
          "pair classifier picks 3.0A active pin across cable orientation");
    Check(hardware::ClassifyUsbCcReadings(496U, 62U, thresholds) == hardware::UsbCcLevel::Ma500,
          "pair classifier picks legacy USB-A advertisement");
    Check(hardware::ClassifyUsbCcReadings(638U, 73U, thresholds) == hardware::UsbCcLevel::Ma1500,
          "pair classifier picks intermediate 1.5A advertisement");

    Check(hardware::ClassifyUsbCcReadings(567U, 73U, thresholds) ==
              hardware::UsbCcLevel::Ma500,
          "reported marginal cable reading stays at default current");
    Check(hardware::ClassifyUsbCcReadings(80U, 651U, thresholds) ==
              hardware::UsbCcLevel::Ma1500,
          "reported stable cable reading advertises 1.5A");
    Check(hardware::ClassifyUsbCcReadings(638U, 651U, thresholds) ==
              hardware::UsbCcLevel::Unknown,
          "two active CC pins fail conservative as an ambiguous attachment");
    Check(hardware::SelectUsbCcActivePin(567U, 73U, thresholds) ==
              hardware::UsbCcActivePin::Cc1,
          "reported marginal orientation selects CC1");
    Check(hardware::SelectUsbCcActivePin(80U, 651U, thresholds) ==
              hardware::UsbCcActivePin::Cc2,
          "reported stable orientation selects CC2");
    Check(hardware::SelectUsbCcActivePin(73U, 73U, thresholds) ==
              hardware::UsbCcActivePin::None,
          "two inactive pins have no selected attachment pin");
    Check(hardware::SelectUsbCcActivePin(638U, 651U, thresholds) ==
              hardware::UsbCcActivePin::None,
          "two active pins have no unambiguous attachment pin");
    Check(hardware::SelectUsbCcActivePin(620U, 319U, thresholds) ==
              hardware::UsbCcActivePin::Cc1,
          "entry and inactive boundaries select CC1");
    Check(hardware::SelectUsbCcActivePin(319U, 620U, thresholds) ==
              hardware::UsbCcActivePin::Cc2,
          "entry and inactive boundaries select CC2");
    Check(hardware::SelectUsbCcActivePin(620U, 320U, thresholds) ==
              hardware::UsbCcActivePin::None,
          "the 500mA boundary makes a second pin ambiguous");

    const hardware::UsbCcActivePinTrackingUpdate partialPinChange =
        hardware::UpdateUsbCcActivePinTracking(
            hardware::UsbCcActivePin::Cc1,
            false,
            hardware::UsbCcActivePin::Cc2);
    Check(partialPinChange.activePin == hardware::UsbCcActivePin::Cc2 &&
              partialPinChange.confirmationMustRestart,
          "an orientation change restarts partial confirmation on the new pin");
    const hardware::UsbCcActivePinTrackingUpdate authorizedPinChange =
        hardware::UpdateUsbCcActivePinTracking(
            hardware::UsbCcActivePin::Cc1,
            true,
            hardware::UsbCcActivePin::Cc2);
    Check(authorizedPinChange.activePin == hardware::UsbCcActivePin::Cc1 &&
              !authorizedPinChange.observationMatchesAuthorizedPin,
          "an authorized attachment never migrates to the opposite pin");
    const hardware::UsbCcActivePinTrackingUpdate authorizedAmbiguous =
        hardware::UpdateUsbCcActivePinTracking(
            hardware::UsbCcActivePin::Cc1,
            true,
            hardware::UsbCcActivePin::None);
    Check(authorizedAmbiguous.activePin == hardware::UsbCcActivePin::Cc1 &&
              !authorizedAmbiguous.observationMatchesAuthorizedPin,
          "an ambiguous observation preserves the authorized monitor pin");
    const hardware::UsbCcActivePinTrackingUpdate authorizedMatch =
        hardware::UpdateUsbCcActivePinTracking(
            hardware::UsbCcActivePin::Cc1,
            true,
            hardware::UsbCcActivePin::Cc1);
    Check(authorizedMatch.observationMatchesAuthorizedPin,
          "the frozen authorized pin recognizes a matching observation");

    hardware::UsbCcActivePin alternatingPin = hardware::UsbCcActivePin::None;
    modules::StrongSourceConfirmationState alternatingConfirmation;
    uint8_t maximumAlternatingSamples = 0U;
    for (uint8_t sample = 0U; sample < 20U; ++sample) {
        const bool cc1Orientation = (sample % 2U) == 0U;
        const uint16_t cc1 = cc1Orientation ? 651U : 80U;
        const uint16_t cc2 = cc1Orientation ? 80U : 651U;
        const hardware::UsbCcActivePin detected =
            hardware::SelectUsbCcActivePin(cc1, cc2, thresholds);
        const hardware::UsbCcActivePinTrackingUpdate pinUpdate =
            hardware::UpdateUsbCcActivePinTracking(
                alternatingPin,
                modules::IsStrongSourceConfirmed(alternatingConfirmation),
                detected);
        if (pinUpdate.confirmationMustRestart) {
            alternatingConfirmation =
                modules::RevokeStrongSourceConfirmation(
                    alternatingConfirmation);
        }
        alternatingPin = pinUpdate.activePin;
        alternatingConfirmation =
            modules::UpdateStrongSourceConfirmationState(
                alternatingConfirmation, true, true)
                .state;
        if (alternatingConfirmation.samples > maximumAlternatingSamples) {
            maximumAlternatingSamples = alternatingConfirmation.samples;
        }
    }
    Check(!modules::IsStrongSourceConfirmed(alternatingConfirmation) &&
              maximumAlternatingSamples == 1U,
          "alternating CC orientations cannot accumulate source authorization");

    const hardware::UsbCcSampleWindow oneLowOutlier{
        73U, 638U, 638U, 638U, 638U, 638U, 638U, 638U};
    Check(hardware::CalculateUsbCcTrimmedMean(oneLowOutlier) == 638U,
          "one low ADC outlier cannot drag a strong CC window below threshold");
    const hardware::UsbCcSampleWindow oneHighOutlier{
        651U, 73U, 73U, 73U, 73U, 73U, 73U, 73U};
    Check(hardware::CalculateUsbCcTrimmedMean(oneHighOutlier) == 73U,
          "one high ADC outlier cannot make an inactive CC pin look active");

    const hardware::UsbCcMonitorSampleWindow monitorLowOutlier{
        73U, 638U, 638U};
    Check(hardware::CalculateUsbCcMedian(monitorLowOutlier) == 638U,
          "confirmed-source monitor rejects one low ADC outlier");
    const hardware::UsbCcMonitorSampleWindow monitorHighOutlier{
        651U, 73U, 73U};
    Check(hardware::CalculateUsbCcMedian(monitorHighOutlier) == 73U,
          "confirmed-source monitor rejects one high ADC outlier");
    const hardware::UsbCcMonitorSampleWindow reportedMostlyStrong{
        567U, 651U, 651U};
    Check(hardware::CalculateUsbCcMedian(reportedMostlyStrong) == 651U,
          "monitor keeps a strong majority around the reported boundary");
    const hardware::UsbCcMonitorSampleWindow reportedMostlyWeak{
        567U, 567U, 651U};
    Check(hardware::CalculateUsbCcMedian(reportedMostlyWeak) == 567U,
          "monitor keeps a weak majority around the reported boundary");

    Check(hardware::GetUsbCcStrongConfirmationRawThreshold(thresholds) == 620U,
          "default strong-source entry threshold includes a raw guard band");
    Check(!hardware::UsbCcReadingsMeetStrongConfirmationThreshold(
              73U, 619U, thresholds),
          "entry guard rejects the raw count immediately below its boundary");
    Check(hardware::UsbCcReadingsMeetStrongConfirmationThreshold(
              73U, 620U, thresholds),
          "entry guard accepts its exact raw boundary in either orientation");
    Check(!hardware::UsbCcReadingsMeetStrongConfirmationThreshold(
              567U, 73U, thresholds),
          "reported marginal cable reading cannot authorize normal power");
    Check(hardware::UsbCcReadingsMeetStrongConfirmationThreshold(
              80U, 651U, thresholds),
          "reported stable cable reading can enter strong-source confirmation");
    Check(!hardware::UsbCcReadingsMeetStrongConfirmationThreshold(
              638U, 651U, thresholds),
          "ambiguous dual-active CC readings cannot authorize normal power");
    Check(hardware::ClassifyUsbCcRaw(580U, thresholds) ==
              hardware::UsbCcLevel::Ma1500,
          "retention threshold itself remains a strong advertisement");
    Check(hardware::ClassifyUsbCcRaw(579U, thresholds) ==
              hardware::UsbCcLevel::Ma500,
          "one count below retention is a default-current advertisement");

    const hardware::UsbCcThresholds invalidThresholds{320U, 0U, 850U};
    Check(hardware::ClassifyUsbCcRaw(1080U, invalidThresholds) == hardware::UsbCcLevel::Unknown,
          "classifier rejects an invalid threshold triple");
    Check(!hardware::UsbCcReadingsMeetStrongConfirmationThreshold(
              73U, 1080U, invalidThresholds),
          "strong-entry helper rejects invalid thresholds");
    Check(hardware::SelectUsbCcActivePin(73U, 1080U, invalidThresholds) ==
              hardware::UsbCcActivePin::None,
          "active-pin selection rejects invalid thresholds");

    Check(hardware::GetUsbCcStrongConfirmationRawThreshold(custom) == 240U,
          "custom 1.5A threshold retains the raw entry guard");
    const hardware::UsbCcThresholds narrowUpperBand{100U, 280U, 300U};
    Check(hardware::GetUsbCcStrongConfirmationRawThreshold(narrowUpperBand) ==
              300U,
          "strong-entry guard is capped at the configured 3A threshold");

    Check(hardware::MaxUsbCcLevel(hardware::UsbCcLevel::Ma500, hardware::UsbCcLevel::Ma1500) ==
              hardware::UsbCcLevel::Ma1500,
          "max helper prefers higher advertised current");

    if (g_failures == 0) {
        std::puts("[USB_CC_SENSE] ok");
        return 0;
    }

    std::printf("[USB_CC_SENSE] failures=%d\n", g_failures);
    return 1;
}
