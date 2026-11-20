#ifdef ARDUINO

#include "firmware/amptest_session.hpp"

#include "console.hpp"
#include "firmware/firmware_internal.hpp"

namespace firmware::amptest {

namespace {

std::string_view TrimWhitespace(std::string_view text) {
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) {
        ++start;
    }

    size_t end = text.size();
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }

    return text.substr(start, end - start);
}

} // namespace

void VerifySession::Reset() {
    m_active = false;
    m_hasResults = false;
    m_step = Step::CoreBaseline;
    m_measurements = {};
    m_appliedAmbientKnown = false;
    m_appliedAmbientOn = false;
    m_appliedRailKnown = false;
    m_appliedRailOn = false;
}

VerifySession::StepMeasurement& VerifySession::CurrentMeasurement() {
    return m_measurements[static_cast<size_t>(m_step)];
}

const VerifySession::StepMeasurement& VerifySession::CurrentMeasurement() const {
    return m_measurements[static_cast<size_t>(m_step)];
}

const char* VerifySession::CurrentStepName() const {
    switch (m_step) {
        case Step::CoreBaseline:
            return "core_baseline";
        case Step::AmbientBaseline:
            return "ambient_baseline";
        case Step::RailBaseline:
            return "rail_baseline";
        case Step::RedUnderLimit:
            return "red_under_limit";
        case Step::GreenUnderLimit:
            return "green_under_limit";
        case Step::BlueUnderLimit:
            return "blue_under_limit";
        case Step::NeutralUnderLimit:
            return "neutral_under_limit";
        case Step::NeutralOverLimit:
            return "neutral_over_limit";
        case Step::Count:
        default:
            return "unknown";
    }
}

const char* VerifySession::CurrentStepDescription() const {
    switch (m_step) {
        case Step::CoreBaseline:
            return "ambient off, 5v off, leds off";
        case Step::AmbientBaseline:
            return "ambient on, 5v off, leds off";
        case Step::RailBaseline:
            return "ambient on, 5v on, leds off";
        case Step::RedUnderLimit:
            return "all leds red at the highest non-limited level for the active cap";
        case Step::GreenUnderLimit:
            return "all leds green at the highest non-limited level for the active cap";
        case Step::BlueUnderLimit:
            return "all leds blue at the highest non-limited level for the active cap";
        case Step::NeutralUnderLimit:
            return "all leds neutral rgb at the highest non-limited level for the active cap";
        case Step::NeutralOverLimit:
            return "all leds neutral rgb at the first level that should exceed the active cap";
        case Step::Count:
        default:
            return "";
    }
}

size_t VerifySession::CurrentStepIndex1() const {
    return static_cast<size_t>(m_step) + 1U;
}

size_t VerifySession::RecordedStepCount() const {
    size_t count = 0U;
    for (const StepMeasurement& measurement : m_measurements) {
        count += measurement.recorded ? 1U : 0U;
    }
    return count;
}

void VerifySession::PrintCurrentStep(fxe::Console& console) const {
    const StepMeasurement& measurement = CurrentMeasurement();
    const unsigned long scalePct = static_cast<unsigned long>(measurement.prediction.appliedBrightnessScale01 * 100.0f + 0.5f);
    if (!CurrentRailShouldBeOn()) {
        console.PrintLn("amptest verify: step %lu/%lu %s predicted_total=%u mA actual=%s",
                        static_cast<unsigned long>(CurrentStepIndex1()),
                        static_cast<unsigned long>(STEP_COUNT),
                        CurrentStepName(),
                        static_cast<unsigned>(measurement.prediction.predictedTotalMilliAmps),
                        measurement.recorded ? "recorded" : "pending");
        console.PrintLn("amptest verify: %s", CurrentStepDescription());
        return;
    }

    console.PrintLn(
        "amptest verify: step %lu/%lu %s level=%u/30 pwm=%u predicted_total=%u mA baseline=%u mA led_requested=%u mA led_applied=%u mA limit=%u mA scale=%lu%% actual=%s",
        static_cast<unsigned long>(CurrentStepIndex1()),
        static_cast<unsigned long>(STEP_COUNT),
        CurrentStepName(),
        static_cast<unsigned>(measurement.prediction.requestedLevel1To30),
        static_cast<unsigned>(measurement.prediction.requestedPwm),
        static_cast<unsigned>(measurement.prediction.predictedTotalMilliAmps),
        static_cast<unsigned>(measurement.prediction.comparisonBaselineMilliAmps),
        static_cast<unsigned>(measurement.prediction.requestedLedMilliAmps),
        static_cast<unsigned>(measurement.prediction.appliedLedMilliAmps),
        static_cast<unsigned>(measurement.prediction.activeLedLimitMilliAmps),
        scalePct,
        measurement.recorded ? "recorded" : "pending");
    console.PrintLn("amptest verify: %s", CurrentStepDescription());
}

void VerifySession::PrintSummary(fxe::Console& console) const {
    console.BeginBulkOutput();
    console.PrintLn("amptest verify: recorded_entries=%lu",
                    static_cast<unsigned long>(RecordedStepCount()));

    uint32_t maxAbsDelta = 0U;
    for (size_t index = 0; index < STEP_COUNT; ++index) {
        const Step step = static_cast<Step>(index);
        const StepMeasurement& measurement = m_measurements[index];
        const char* name = "unknown";
        switch (step) {
            case Step::CoreBaseline:
                name = "core_baseline";
                break;
            case Step::AmbientBaseline:
                name = "ambient_baseline";
                break;
            case Step::RailBaseline:
                name = "rail_baseline";
                break;
            case Step::RedUnderLimit:
                name = "red_under_limit";
                break;
            case Step::GreenUnderLimit:
                name = "green_under_limit";
                break;
            case Step::BlueUnderLimit:
                name = "blue_under_limit";
                break;
            case Step::NeutralUnderLimit:
                name = "neutral_under_limit";
                break;
            case Step::NeutralOverLimit:
                name = "neutral_over_limit";
                break;
            case Step::Count:
            default:
                break;
        }

        if (!measurement.recorded) {
            console.PrintLn("amptest verify: %s predicted_total=%u mA actual=pending",
                            name,
                            static_cast<unsigned>(measurement.prediction.predictedTotalMilliAmps));
            continue;
        }

        const int32_t delta = static_cast<int32_t>(measurement.actualTotalMilliAmps) -
                              static_cast<int32_t>(measurement.prediction.predictedTotalMilliAmps);
        const uint32_t absDelta = (delta < 0) ? static_cast<uint32_t>(-delta) : static_cast<uint32_t>(delta);
        if (absDelta > maxAbsDelta) {
            maxAbsDelta = absDelta;
        }

        console.PrintLn("amptest verify: %s predicted_total=%u mA actual=%lu mA delta=%ld mA",
                        name,
                        static_cast<unsigned>(measurement.prediction.predictedTotalMilliAmps),
                        static_cast<unsigned long>(measurement.actualTotalMilliAmps),
                        static_cast<long>(delta));
    }

    console.PrintLn("amptest verify: max_abs_delta=%lu mA", static_cast<unsigned long>(maxAbsDelta));
    console.EndBulkOutput();
}

bool VerifySession::Advance(fxe::Console& console) {
    if (!m_active) {
        return false;
    }

    const size_t nextIndex = static_cast<size_t>(m_step) + 1U;
    if (nextIndex >= STEP_COUNT) {
        m_hasResults = true;
        PrintSummary(console);
        m_active = false;
        RestoreRuntimeHardwareState();
        console.PrintLn("amptest verify: complete");
        return false;
    }

    m_step = static_cast<Step>(nextIndex);
    ApplyCurrentHardwareState();
    PrintCurrentStep(console);
    return true;
}

void VerifySession::RecordActualMeasurement(fxe::Console& console, const uint32_t milliAmps) {
    StepMeasurement& measurement = CurrentMeasurement();
    measurement.recorded = true;
    measurement.actualTotalMilliAmps = milliAmps;
    m_hasResults = true;

    const int32_t delta = static_cast<int32_t>(measurement.actualTotalMilliAmps) -
                          static_cast<int32_t>(measurement.prediction.predictedTotalMilliAmps);
    console.PrintLn("amptest verify: recorded=%lu mA predicted_total=%u mA delta=%ld mA",
                    static_cast<unsigned long>(milliAmps),
                    static_cast<unsigned>(measurement.prediction.predictedTotalMilliAmps),
                    static_cast<long>(delta));
}

bool VerifySession::HandleEnteredLine(fxe::Console& console, const std::string_view line) {
    if (!m_active) {
        return false;
    }

    const std::string_view trimmed = TrimWhitespace(line);
    if (trimmed.empty() || trimmed == "next" || trimmed == "skip") {
        console.PrintLn("amptest verify: advance");
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
        console.PrintLn("amptest verify: invalid current input");
        return true;
    }

    RecordActualMeasurement(console, milliAmps);
    (void)Advance(console);
    return true;
}

void VerifySession::PrintStatus(fxe::Console& console) const {
    if (!m_active && !m_hasResults) {
        console.PrintLn("amptest verify: idle");
        return;
    }

    if (m_active) {
        PrintCurrentStep(console);
    }
    PrintSummary(console);
}

void VerifySession::Start(fxe::Console& console) {
    auto& sys = firmware_internal::GetSystem();
    if (!firmware_internal::QuiesceLedRail(sys)) {
        console.PrintLn("amptest verify: LED clear incomplete; runtime retained; retry start");
        return;
    }
    Reset();
    m_appliedRailKnown = true;
    m_appliedRailOn = false;

    m_active = true;
    m_step = Step::CoreBaseline;

    sys.gamePersistence.RequestCancelAllThinking();
    sys.quickSettingsOverlayActive = false;
    sys.quickSettingsOverlayOwner = modules::UiOwner::None;
    sys.menuOverlay.Stop();
    sys.gameResetHoldActive = false;
    sys.gameResetHoldTriggered = false;

    console.PrintLn("amptest verify: staged spot checks use fixed board states and reference totals from the calibrated S3 model");
    console.PrintLn("amptest verify: enter inline meter readings to record+advance; blank Enter skips; status/stop supported");
    ApplyCurrentHardwareState();
    PrintCurrentStep(console);
}

void VerifySession::Stop(fxe::Console& console) {
    if (!m_active) {
        console.PrintLn("amptest verify: idle");
        return;
    }

    m_hasResults = m_hasResults || (RecordedStepCount() != 0U);
    PrintSummary(console);
    m_active = false;
    RestoreRuntimeHardwareState();
    console.PrintLn("amptest verify: stopped");
}

} // namespace firmware::amptest

#endif
