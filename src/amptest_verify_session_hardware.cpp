#ifdef ARDUINO

#include <Arduino.h>

#include <array>

#include "firmware/amptest_session.hpp"
#include "firmware/firmware_internal.hpp"
#include "hardware/hardware.hpp"

namespace firmware::amptest {

namespace {

constexpr uint16_t REFERENCE_CORE_BASELINE_MA = 118U;
constexpr uint16_t REFERENCE_AMBIENT_BASELINE_MA = 120U;
constexpr uint16_t REFERENCE_RAIL_BASELINE_MA = 234U;
constexpr uint16_t UNDER_LIMIT_MARGIN_MA = 20U;

modules::Rgb MakeGray(const uint8_t value) {
    return modules::Rgb{value, value, value};
}

} // namespace

uint32_t VerifySession::CurrentReferenceBaselineMilliAmps() const {
    switch (m_step) {
        case Step::CoreBaseline:
            return REFERENCE_CORE_BASELINE_MA;
        case Step::AmbientBaseline:
            return REFERENCE_AMBIENT_BASELINE_MA;
        case Step::RailBaseline:
        case Step::RedUnderLimit:
        case Step::GreenUnderLimit:
        case Step::BlueUnderLimit:
        case Step::NeutralUnderLimit:
        case Step::NeutralOverLimit:
            return REFERENCE_RAIL_BASELINE_MA;
        case Step::Count:
        default:
            return 0U;
    }
}

uint32_t VerifySession::CurrentComparisonBaselineMilliAmps() const {
    if (!CurrentStepUsesLedLoad()) {
        return CurrentReferenceBaselineMilliAmps();
    }

    const StepMeasurement& railMeasurement = m_measurements[static_cast<size_t>(Step::RailBaseline)];
    if (railMeasurement.recorded) {
        return railMeasurement.actualTotalMilliAmps;
    }

    return REFERENCE_RAIL_BASELINE_MA;
}

bool VerifySession::CurrentAmbientShouldBeOn() const {
    return m_step != Step::CoreBaseline;
}

bool VerifySession::CurrentRailShouldBeOn() const {
    return m_step != Step::CoreBaseline && m_step != Step::AmbientBaseline;
}

bool VerifySession::CurrentStepUsesLedLoad() const {
    switch (m_step) {
        case Step::RedUnderLimit:
        case Step::GreenUnderLimit:
        case Step::BlueUnderLimit:
        case Step::NeutralUnderLimit:
        case Step::NeutralOverLimit:
            return true;
        case Step::CoreBaseline:
        case Step::AmbientBaseline:
        case Step::RailBaseline:
        case Step::Count:
        default:
            return false;
    }
}

bool VerifySession::CurrentStepExpectsBudgetLimit() const {
    return m_step == Step::NeutralOverLimit;
}

modules::Rgb VerifySession::RequestedColorForLevel(const uint8_t level1To30) const {
    const uint8_t pwm = LedLevelToPwm(level1To30);
    switch (m_step) {
        case Step::RedUnderLimit:
            return modules::Rgb{pwm, 0U, 0U};
        case Step::GreenUnderLimit:
            return modules::Rgb{0U, pwm, 0U};
        case Step::BlueUnderLimit:
            return modules::Rgb{0U, 0U, pwm};
        case Step::NeutralUnderLimit:
        case Step::NeutralOverLimit:
            return MakeGray(pwm);
        case Step::CoreBaseline:
        case Step::AmbientBaseline:
        case Step::RailBaseline:
        case Step::Count:
        default:
            return modules::RGB_BLACK;
    }
}

uint8_t VerifySession::SelectRequestedLevel(const uint16_t ledLimitMilliAmps) {
    if (!CurrentStepUsesLedLoad()) {
        return 0U;
    }

    auto& sys = firmware_internal::GetSystem();
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> physicalFrame{};

    if (CurrentStepExpectsBudgetLimit()) {
        for (uint8_t level = 1U; level <= LED_LEVEL_COUNT; ++level) {
            const modules::Rgb color = RequestedColorForLevel(level);
            physicalFrame.fill(color);
            if (firmware_internal::EstimatePhysicalFrameLedCurrentMa(sys, physicalFrame, 256U) > ledLimitMilliAmps) {
                return level;
            }
        }
        return LED_LEVEL_COUNT;
    }

    const uint16_t targetMilliAmps = (ledLimitMilliAmps > UNDER_LIMIT_MARGIN_MA)
                                         ? static_cast<uint16_t>(ledLimitMilliAmps - UNDER_LIMIT_MARGIN_MA)
                                         : ledLimitMilliAmps;
    uint8_t bestLevel = 1U;
    for (uint8_t level = 1U; level <= LED_LEVEL_COUNT; ++level) {
        const modules::Rgb color = RequestedColorForLevel(level);
        physicalFrame.fill(color);
        const uint16_t estimateMilliAmps = firmware_internal::EstimatePhysicalFrameLedCurrentMa(sys, physicalFrame, 256U);
        if (estimateMilliAmps <= targetMilliAmps) {
            bestLevel = level;
            continue;
        }
        break;
    }

    return bestLevel;
}

void VerifySession::ApplyCurrentHardwareState() {
    auto& sys = firmware_internal::GetSystem();
    const bool ambientShouldBeOn = CurrentAmbientShouldBeOn();
    const bool railShouldBeOn = CurrentRailShouldBeOn();

    firmware_internal::RefreshLedPowerSourceStatus(sys, fxe::Millis(), true);
    sys.ledPowerLimitMa = firmware_internal::GetActiveLedCurrentLimitMa(sys);

    if (!m_appliedAmbientKnown || ambientShouldBeOn != m_appliedAmbientOn) {
        if (ambientShouldBeOn) {
            sys.ambientLights.PowerOn();
        } else {
            sys.ambientLights.ShutDown();
        }
        m_appliedAmbientKnown = true;
        m_appliedAmbientOn = ambientShouldBeOn;
    }

    if (!m_appliedRailKnown || railShouldBeOn != m_appliedRailOn) {
        if (railShouldBeOn) {
            if (!firmware_internal::EnableLedRail(sys)) return;
        } else {
            if (!firmware_internal::QuiesceLedRail(sys)) return;
        }
        m_appliedRailKnown = true;
        m_appliedRailOn = railShouldBeOn;
    }

    StepMeasurement& measurement = CurrentMeasurement();
    measurement.prediction = VerifyPrediction{};
    measurement.prediction.comparisonBaselineMilliAmps = static_cast<uint16_t>(CurrentComparisonBaselineMilliAmps());
    measurement.prediction.predictedTotalMilliAmps = static_cast<uint16_t>(CurrentReferenceBaselineMilliAmps());
    measurement.prediction.activeLedLimitMilliAmps = sys.ledPowerLimitMa;

    if (!railShouldBeOn) {
        firmware_internal::ClearLedOutput(sys);
        sys.ledPowerStats = modules::BudgetAwareLedPipelineStats{};
        return;
    }

    if (CurrentStepUsesLedLoad()) {
        if (measurement.prediction.requestedLevel1To30 == 0U) {
            measurement.prediction.requestedLevel1To30 = SelectRequestedLevel(sys.ledPowerLimitMa);
            if (measurement.prediction.requestedLevel1To30 == 0U) {
                measurement.prediction.requestedLevel1To30 = 1U;
            }
        }
        measurement.prediction.requestedPwm = LedLevelToPwm(measurement.prediction.requestedLevel1To30);
    }

    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> physicalFrame{};
    const modules::Rgb color = CurrentStepUsesLedLoad() ? RequestedColorForLevel(measurement.prediction.requestedLevel1To30)
                                                        : modules::RGB_BLACK;
    for (auto& led : physicalFrame) {
        led = color;
    }

    measurement.prediction.requestedLedMilliAmps =
        firmware_internal::EstimatePhysicalFrameLedCurrentMa(sys, physicalFrame, 256U);

    RenderPhysicalFrameWithPowerBudget(sys, physicalFrame, 256U);

    measurement.prediction.appliedLedMilliAmps = sys.ledPowerStats.appliedLedCurrentMa;
    measurement.prediction.predictedTotalMilliAmps = static_cast<uint16_t>(
        measurement.prediction.comparisonBaselineMilliAmps + measurement.prediction.appliedLedMilliAmps);
    measurement.prediction.appliedBrightnessScale01 = sys.ledPowerStats.appliedBrightnessScale01;
    measurement.prediction.budgetLimited =
        measurement.prediction.requestedLedMilliAmps != measurement.prediction.appliedLedMilliAmps;
}

void VerifySession::RestoreRuntimeHardwareState() {
    auto& sys = firmware_internal::GetSystem();
    firmware_internal::ClearLedOutput(sys);

    if (!m_appliedAmbientKnown || !m_appliedAmbientOn) {
        sys.ambientLights.PowerOn();
    }
    if (!m_appliedRailKnown || !m_appliedRailOn) {
        if (!firmware_internal::EnableLedRail(sys)) return;
    }

    m_appliedAmbientKnown = false;
    m_appliedAmbientOn = false;
    m_appliedRailKnown = false;
    m_appliedRailOn = false;
}

void VerifySession::Render() {
    if (!m_active) {
        return;
    }

    ApplyCurrentHardwareState();
}

} // namespace firmware::amptest

#endif
