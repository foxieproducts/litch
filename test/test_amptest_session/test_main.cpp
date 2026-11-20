#include <cstdio>

#include "firmware/amptest_session.hpp"

int RunAmpTestProductionJourney();

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
    uint32_t milliAmps = 0;
    Check(firmware::amptest::TryParseCurrentInput("412", milliAmps) && milliAmps == 412U,
          "plain integer input is treated as milliamps");
    Check(firmware::amptest::TryParseCurrentInput("412mA", milliAmps) && milliAmps == 412U,
          "mA suffix is accepted");
    Check(firmware::amptest::TryParseCurrentInput("0.412A", milliAmps) && milliAmps == 412U,
          "amp suffix is converted to milliamps");
    Check(firmware::amptest::TryParseCurrentInput("0.412", milliAmps) && milliAmps == 412U,
          "decimal input without suffix is treated as amps");
    Check(!firmware::amptest::TryParseCurrentInput("bad", milliAmps), "invalid input is rejected");

    Check(firmware::amptest::LedLevelToPwm(1U) == 9U, "level 1 maps to the first full-scale step");
    Check(firmware::amptest::LedLevelToPwm(15U) == 128U, "midpoint level maps to midpoint pwm");
    Check(firmware::amptest::LedLevelToPwm(30U) == 255U, "level 30 maps to full output");
    Check(firmware::amptest::SAFE_START_LEVEL == 1U,
          "raw characterization starts at its lowest LED level");

    const firmware::amptest::Measurement baseline{true, 210U};
    firmware::amptest::SampleSeries redSamples = {};
    redSamples[9] = firmware::amptest::Measurement{true, 510U};
    redSamples[19] = firmware::amptest::Measurement{true, 810U};
    redSamples[29] = firmware::amptest::Measurement{true, 1110U};

    const firmware::amptest::InterpolatedEstimate redEstimate =
        firmware::amptest::InterpolateEstimate(baseline, redSamples, 224U, 1U);
    Check(redEstimate.valid, "interpolation succeeds with recorded red samples");
    Check(redEstimate.sampleCount == 3U, "sample count reflects the recorded levels");
    Check(redEstimate.estimatedFullScaleDeltaMilliAmps == 900,
          "interpolation recovers the red full-scale delta");
    Check(redEstimate.estimatedFullScaleMilliAmps == 1110U,
          "interpolation adds the baseline back to the full-scale estimate");
    Check(redEstimate.milliAmpsPerLedTimes100 == 402U,
          "per-led interpolation output is rounded to hundredths");
    Check(redEstimate.milliAmpsPerChannelTimes100 == 402U,
          "single-channel interpolation has matching per-channel output");
    Check(redEstimate.maxResidualMilliAmps == 0U,
          "perfectly linear samples produce zero residual");

    firmware::amptest::SampleSeries invalidSamples = {};
    invalidSamples[4] = firmware::amptest::Measurement{true, 180U};
    const firmware::amptest::InterpolatedEstimate invalidEstimate =
        firmware::amptest::InterpolateEstimate(baseline, invalidSamples, 224U, 1U);
    Check(!invalidEstimate.valid, "samples below baseline are ignored and do not produce a fit");

    g_failures += RunAmpTestProductionJourney();

    if (g_failures == 0) {
        std::puts("[AMPTEST_SESSION] ok");
        return 0;
    }

    std::printf("[AMPTEST_SESSION] failures=%d\n", g_failures);
    return 1;
}
