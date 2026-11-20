#ifdef ARDUINO

#include "firmware/amptest_session.hpp"

#include <array>

#include "console.hpp"
#include "firmware/firmware_internal.hpp"

namespace firmware::amptest {

namespace {

constexpr std::array<LedMode, SAMPLE_LED_MODE_COUNT> SAMPLE_MODES = {
    LedMode::Red,
    LedMode::Green,
    LedMode::Blue,
    LedMode::White,
};

void PrintMeasurementLine(fxe::Console& console, const char* label, const Measurement& measurement) {
    if (!measurement.recorded) {
        console.PrintLn("amptest: %s=pending", label);
        return;
    }

    console.PrintLn("amptest: %s=%lu mA", label, static_cast<unsigned long>(measurement.milliAmps));
}

void PrintInterpolatedLine(fxe::Console& console,
                           const char* label,
                           const char* baselineLabel,
                           const InterpolatedEstimate& estimate) {
    if (!estimate.valid) {
        console.PrintLn("amptest: %s=insufficient_samples", label);
        return;
    }

    console.PrintLn(
        "amptest: %s samples=%u fullscale=%lu mA delta_vs_%s=%ld mA per_led=%lu.%02lu mA per_channel=%lu.%02lu mA residual_max=%lu mA",
        label,
        static_cast<unsigned>(estimate.sampleCount),
        static_cast<unsigned long>(estimate.estimatedFullScaleMilliAmps),
        baselineLabel,
        static_cast<long>(estimate.estimatedFullScaleDeltaMilliAmps),
        static_cast<unsigned long>(estimate.milliAmpsPerLedTimes100 / 100U),
        static_cast<unsigned long>(estimate.milliAmpsPerLedTimes100 % 100U),
        static_cast<unsigned long>(estimate.milliAmpsPerChannelTimes100 / 100U),
        static_cast<unsigned long>(estimate.milliAmpsPerChannelTimes100 % 100U),
        static_cast<unsigned long>(estimate.maxResidualMilliAmps));
}

} // namespace

void Session::PrintCurrentStep(fxe::Console& console) const {
    if (IsSamplePhase()) {
        const Measurement& measurement = m_ledMeasurements[CurrentSampleModeIndex()][m_level1To30 - 1U];
        console.PrintLn("amptest: step %lu/%lu mode=%s level=%u/%u pwm=%u recorded_levels=%lu current_level=%s",
                        static_cast<unsigned long>(CurrentStepIndex1()),
                        static_cast<unsigned long>(TotalStepCount()),
                        LedModeToString(CurrentLedMode()),
                        static_cast<unsigned>(m_level1To30),
                        static_cast<unsigned>(LED_LEVEL_COUNT),
                        static_cast<unsigned>(LedLevelToPwm(m_level1To30)),
                        static_cast<unsigned long>(RecordedSampleCount(CurrentSampleModeIndex())),
                        measurement.recorded ? "recorded" : "pending");
        return;
    }

    const char* label = "inactive";
    const Measurement* measurement = CurrentMeasurement();
    switch (m_phase) {
        case Phase::BaselineCore:
            label = "core_baseline (ambient off, 5v off)";
            break;
        case Phase::BaselineAmbient:
            label = "ambient_baseline (ambient on, 5v off)";
            break;
        case Phase::BaselineRail:
            label = "rail_baseline (ambient on, 5v on)";
            break;
        case Phase::Inactive:
        case Phase::SampleRed:
        case Phase::SampleGreen:
        case Phase::SampleBlue:
        case Phase::SampleWhite:
        default:
            break;
    }

    console.PrintLn("amptest: step %lu/%lu %s current=%s",
                    static_cast<unsigned long>(CurrentStepIndex1()),
                    static_cast<unsigned long>(TotalStepCount()),
                    label,
                    (measurement && measurement->recorded) ? "recorded" : "pending");
}

void Session::PrintSummary(fxe::Console& console) const {
    const uint16_t ledCount = firmware_internal::GetSystem().Leds().GetLedCount();
    console.BeginBulkOutput();
    console.PrintLn("amptest: recorded_entries=%lu",
                    static_cast<unsigned long>(RecordedStepCount()));

    PrintMeasurementLine(console, "core", m_coreBaseline);
    PrintMeasurementLine(console, "ambient", m_ambientBaseline);
    PrintMeasurementLine(console, "rail", m_railBaseline);
    if (m_coreBaseline.recorded && m_ambientBaseline.recorded) {
        console.PrintLn("amptest: ambient_delta_vs_core=%ld mA",
                        static_cast<long>(static_cast<int32_t>(m_ambientBaseline.milliAmps) -
                                          static_cast<int32_t>(m_coreBaseline.milliAmps)));
    }
    if (m_ambientBaseline.recorded && m_railBaseline.recorded) {
        console.PrintLn("amptest: rail_delta_vs_ambient=%ld mA",
                        static_cast<long>(static_cast<int32_t>(m_railBaseline.milliAmps) -
                                          static_cast<int32_t>(m_ambientBaseline.milliAmps)));
    }

    const Measurement* baseline = LedLoadBaseline();
    const char* baselineLabel = LedLoadBaselineLabel();
    std::array<InterpolatedEstimate, SAMPLE_LED_MODE_COUNT> fits = {};
    for (size_t modeIndex = 0; modeIndex < SAMPLE_LED_MODE_COUNT; ++modeIndex) {
        const LedMode mode = SAMPLE_MODES[modeIndex];
        if (!baseline) {
            console.PrintLn("amptest: %s=missing_led_baseline", LedModeToString(mode));
            continue;
        }

        fits[modeIndex] = InterpolateEstimate(*baseline,
                                              m_ledMeasurements[modeIndex],
                                              ledCount,
                                              LedModeActiveChannelCount(mode));
        PrintInterpolatedLine(console, LedModeToString(mode), baselineLabel, fits[modeIndex]);
    }

    if (baseline && fits[0].valid && fits[1].valid && fits[2].valid) {
        const int32_t rgbDelta = fits[0].estimatedFullScaleDeltaMilliAmps +
                                 fits[1].estimatedFullScaleDeltaMilliAmps +
                                 fits[2].estimatedFullScaleDeltaMilliAmps;
        const uint32_t rgbTotal = baseline->milliAmps + static_cast<uint32_t>(rgbDelta);
        const uint32_t perLedTimes100 =
            ((static_cast<uint64_t>(rgbDelta) * 100ULL) + (ledCount / 2U)) /
            ledCount;
        const uint32_t channelCount = static_cast<uint32_t>(ledCount) * 3U;
        const uint32_t perChannelTimes100 =
            ((static_cast<uint64_t>(rgbDelta) * 100ULL) + (channelCount / 2U)) / channelCount;
        console.PrintLn(
            "amptest: rgb_derived_fullscale=%lu mA delta_vs_%s=%ld mA per_led=%lu.%02lu mA per_channel=%lu.%02lu mA",
            static_cast<unsigned long>(rgbTotal),
            baselineLabel,
            static_cast<long>(rgbDelta),
            static_cast<unsigned long>(perLedTimes100 / 100U),
            static_cast<unsigned long>(perLedTimes100 % 100U),
            static_cast<unsigned long>(perChannelTimes100 / 100U),
            static_cast<unsigned long>(perChannelTimes100 % 100U));

        if (fits[3].valid) {
            const int32_t rgbwDelta = rgbDelta + fits[3].estimatedFullScaleDeltaMilliAmps;
            const uint32_t rgbwTotal = baseline->milliAmps + static_cast<uint32_t>(rgbwDelta);
            const uint32_t rgbwPerLedTimes100 =
                ((static_cast<uint64_t>(rgbwDelta) * 100ULL) + (ledCount / 2U)) /
                ledCount;
            const uint32_t rgbwChannelCount = static_cast<uint32_t>(ledCount) * 4U;
            const uint32_t rgbwPerChannelTimes100 =
                ((static_cast<uint64_t>(rgbwDelta) * 100ULL) + (rgbwChannelCount / 2U)) /
                rgbwChannelCount;
            console.PrintLn(
                "amptest: rgbw_derived_fullscale=%lu mA delta_vs_%s=%ld mA per_led=%lu.%02lu mA per_channel=%lu.%02lu mA",
                static_cast<unsigned long>(rgbwTotal),
                baselineLabel,
                static_cast<long>(rgbwDelta),
                static_cast<unsigned long>(rgbwPerLedTimes100 / 100U),
                static_cast<unsigned long>(rgbwPerLedTimes100 % 100U),
                static_cast<unsigned long>(rgbwPerChannelTimes100 / 100U),
                static_cast<unsigned long>(rgbwPerChannelTimes100 % 100U));
        }
    }
    console.EndBulkOutput();
}

bool Session::Advance(fxe::Console& console) {
    if (!m_active) {
        return false;
    }

    switch (m_phase) {
        case Phase::BaselineCore:
            m_phase = Phase::BaselineAmbient;
            break;
        case Phase::BaselineAmbient:
            m_phase = Phase::BaselineRail;
            break;
        case Phase::BaselineRail:
            m_phase = Phase::SampleRed;
            m_level1To30 = SAFE_START_LEVEL;
            break;
        case Phase::SampleRed:
            m_phase = Phase::SampleGreen;
            m_level1To30 = SAFE_START_LEVEL;
            break;
        case Phase::SampleGreen:
            m_phase = Phase::SampleBlue;
            m_level1To30 = SAFE_START_LEVEL;
            break;
        case Phase::SampleBlue:
            m_phase = Phase::SampleWhite;
            m_level1To30 = SAFE_START_LEVEL;
            break;
        case Phase::SampleWhite:
            m_hasResults = true;
            PrintSummary(console);
            m_active = false;
            m_phase = Phase::Inactive;
            RestoreRuntimeHardwareState();
            console.PrintLn("amptest: complete");
            return false;
        case Phase::Inactive:
        default:
            return false;
    }

    ApplyCurrentHardwareState();
    PrintCurrentStep(console);
    return true;
}

void Session::RecordCurrent(fxe::Console& console, const uint32_t milliAmps) {
    Measurement* measurement = CurrentMeasurement();
    if (!measurement) {
        return;
    }

    measurement->recorded = true;
    measurement->milliAmps = milliAmps;
    console.PrintLn("amptest: recorded %lu mA", static_cast<unsigned long>(milliAmps));
}

void Session::RecordSample(fxe::Console& console, const uint32_t milliAmps) {
    Measurement* measurement = CurrentMeasurement();
    const SampleSeries* samples = CurrentSamples();
    const Measurement* baseline = LedLoadBaseline();
    if (!measurement || !samples) {
        return;
    }

    measurement->recorded = true;
    measurement->milliAmps = milliAmps;
    console.PrintLn("amptest: %s level=%u recorded=%lu mA",
                    LedModeToString(CurrentLedMode()),
                    static_cast<unsigned>(m_level1To30),
                    static_cast<unsigned long>(milliAmps));

    if (baseline) {
        const uint16_t ledCount = firmware_internal::GetSystem().Leds().GetLedCount();
        const InterpolatedEstimate fit = InterpolateEstimate(*baseline,
                                                             *samples,
                                                             ledCount,
                                                             LedModeActiveChannelCount(CurrentLedMode()));
        if (fit.valid) {
            PrintInterpolatedLine(console, LedModeToString(CurrentLedMode()), LedLoadBaselineLabel(), fit);
        }
    }
}

bool Session::HandleEnteredLine(fxe::Console& console, const std::string_view line) {
    if (!m_active) {
        return false;
    }

    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    size_t end = line.size();
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) {
        --end;
    }
    const std::string_view trimmed = line.substr(start, end - start);
    if (trimmed.empty() || trimmed == "next" || trimmed == "skip") {
        console.PrintLn("amptest: advance");
        (void)Advance(console);
        return true;
    }
    if (trimmed == "status") {
        PrintStatus(console);
        return true;
    }
    if (trimmed == "stop" || trimmed == "quit") {
        Stop(console);
        return true;
    }

    const char firstChar = trimmed.front();
    if (!((firstChar >= '0' && firstChar <= '9') || firstChar == '.')) {
        return false;
    }

    uint32_t milliAmps = 0U;
    if (!TryParseCurrentInput(trimmed, milliAmps)) {
        console.PrintLn("amptest: invalid current input");
        return true;
    }

    if (IsSamplePhase()) {
        RecordSample(console, milliAmps);
        PrintCurrentStep(console);
        return true;
    }

    RecordCurrent(console, milliAmps);
    (void)Advance(console);
    return true;
}

bool Session::HandleButtonEvent(const modules::ButtonEvent& event) {
    if (!m_active) {
        return false;
    }
    if (!IsSamplePhase()) {
        return true;
    }

    const bool isPressLike = (event.event == fxe::Button::Event::Press) ||
                             (event.event == fxe::Button::Event::Repeat);
    if (!isPressLike || (event.button != modules::UserButton::Up && event.button != modules::UserButton::Down)) {
        return true;
    }

    const int delta = (event.button == modules::UserButton::Up) ? 1 : -1;
    int next = static_cast<int>(m_level1To30) + delta;
    if (next < static_cast<int>(MIN_LEVEL)) {
        next = MIN_LEVEL;
    }
    if (next > static_cast<int>(MAX_LEVEL)) {
        next = MAX_LEVEL;
    }
    if (next == static_cast<int>(m_level1To30)) {
        return true;
    }

    m_level1To30 = static_cast<uint8_t>(next);
    ApplyCurrentHardwareState();

    fxe::Console* console = firmware_internal::GetSystemContext().console;
    if (console && (event.event == fxe::Button::Event::Press || !m_lastLevelPrintElapsed.Started() ||
                    m_lastLevelPrintElapsed.HasElapsed(120U))) {
        PrintCurrentStep(*console);
        m_lastLevelPrintElapsed.Reset();
    }

    return true;
}

void Session::PrintStatus(fxe::Console& console) const {
    if (!m_active && !m_hasResults) {
        console.PrintLn("amptest: idle");
        return;
    }

    if (m_active) {
        PrintCurrentStep(console);
    }
    PrintSummary(console);
}

void Session::Render() const {
    if (!m_active || !IsSamplePhase()) {
        return;
    }

    auto& sys = firmware_internal::GetSystem();
    const uint8_t pwm = LedLevelToPwm(m_level1To30);
    const LedMode mode = CurrentLedMode();

    firmware::RawDieLedCommand command;
    switch (mode) {
        case LedMode::Red: command = {0U, pwm}; break;
        case LedMode::Green: command = {1U, pwm}; break;
        case LedMode::Blue: command = {2U, pwm}; break;
        case LedMode::White: command = {3U, pwm}; break;
        default: command = {}; break;
    }
    sys.ledLastCommandCompleted = sys.ledAnimation.Submit(command);

}

} // namespace firmware::amptest

#endif
