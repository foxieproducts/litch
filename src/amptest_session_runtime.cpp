#ifdef ARDUINO

#include "firmware/amptest_session.hpp"

#include <Arduino.h>

#include <array>

#include "console.hpp"
#include "firmware/firmware_internal.hpp"
#include "hardware/hardware.hpp"
#include "hardware/led_layout.hpp"
#include "modules/ui_types.hpp"

namespace firmware::amptest {

namespace {

constexpr std::array<LedMode, SAMPLE_LED_MODE_COUNT> SAMPLE_MODES = {
    LedMode::Red,
    LedMode::Green,
    LedMode::Blue,
    LedMode::White,
};

} // namespace

void Session::Reset() {
    m_active = false;
    m_hasResults = false;
    m_phase = Phase::Inactive;
    m_level1To30 = SAFE_START_LEVEL;
    m_coreBaseline = {};
    m_ambientBaseline = {};
    m_railBaseline = {};
    m_ledMeasurements = {};
    m_appliedAmbientKnown = false;
    m_appliedAmbientOn = false;
    m_appliedRailKnown = false;
    m_appliedRailOn = false;
    m_lastLevelPrintElapsed = fxe::ElapsedTime();
}

bool Session::IsSamplePhase() const {
    return m_phase == Phase::SampleRed || m_phase == Phase::SampleGreen || m_phase == Phase::SampleBlue ||
           m_phase == Phase::SampleWhite;
}

size_t Session::CurrentSampleModeIndex() const {
    switch (m_phase) {
        case Phase::SampleRed:
            return 0U;
        case Phase::SampleGreen:
            return 1U;
        case Phase::SampleBlue:
            return 2U;
        case Phase::SampleWhite:
            return 3U;
        case Phase::Inactive:
        case Phase::BaselineCore:
        case Phase::BaselineAmbient:
        case Phase::BaselineRail:
        default:
            return 0U;
    }
}

LedMode Session::CurrentLedMode() const {
    return SAMPLE_MODES[CurrentSampleModeIndex()];
}

Measurement* Session::CurrentMeasurement() {
    switch (m_phase) {
        case Phase::BaselineCore:
            return &m_coreBaseline;
        case Phase::BaselineAmbient:
            return &m_ambientBaseline;
        case Phase::BaselineRail:
            return &m_railBaseline;
        case Phase::SampleRed:
        case Phase::SampleGreen:
        case Phase::SampleBlue:
        case Phase::SampleWhite:
            return &m_ledMeasurements[CurrentSampleModeIndex()][m_level1To30 - 1U];
        case Phase::Inactive:
        default:
            return nullptr;
    }
}

const Measurement* Session::CurrentMeasurement() const {
    return const_cast<Session*>(this)->CurrentMeasurement();
}

SampleSeries* Session::CurrentSamples() {
    if (!IsSamplePhase()) {
        return nullptr;
    }
    return &m_ledMeasurements[CurrentSampleModeIndex()];
}

const SampleSeries* Session::CurrentSamples() const {
    return const_cast<Session*>(this)->CurrentSamples();
}

const Measurement* Session::LedLoadBaseline() const {
    if (m_railBaseline.recorded) {
        return &m_railBaseline;
    }
    if (m_ambientBaseline.recorded) {
        return &m_ambientBaseline;
    }
    if (m_coreBaseline.recorded) {
        return &m_coreBaseline;
    }
    return nullptr;
}

const char* Session::LedLoadBaselineLabel() const {
    if (m_railBaseline.recorded) {
        return "rail";
    }
    if (m_ambientBaseline.recorded) {
        return "ambient";
    }
    if (m_coreBaseline.recorded) {
        return "core";
    }
    return "baseline";
}

size_t Session::CurrentStepIndex1() const {
    switch (m_phase) {
        case Phase::BaselineCore:
            return 1U;
        case Phase::BaselineAmbient:
            return 2U;
        case Phase::BaselineRail:
            return 3U;
        case Phase::SampleRed:
            return 4U;
        case Phase::SampleGreen:
            return 5U;
        case Phase::SampleBlue:
            return 6U;
        case Phase::SampleWhite:
            return 7U;
        case Phase::Inactive:
        default:
            return 0U;
    }
}

size_t Session::TotalStepCount() const {
    return 7U;
}

size_t Session::RecordedSampleCount(const size_t modeIndex) const {
    size_t count = 0U;
    for (const Measurement& measurement : m_ledMeasurements[modeIndex]) {
        count += measurement.recorded ? 1U : 0U;
    }
    return count;
}

size_t Session::RecordedStepCount() const {
    size_t count = 0U;
    count += m_coreBaseline.recorded ? 1U : 0U;
    count += m_ambientBaseline.recorded ? 1U : 0U;
    count += m_railBaseline.recorded ? 1U : 0U;
    for (size_t modeIndex = 0; modeIndex < SAMPLE_LED_MODE_COUNT; ++modeIndex) {
        count += RecordedSampleCount(modeIndex);
    }
    return count;
}

void Session::ApplyCurrentHardwareState() {
    auto& sys = firmware_internal::GetSystem();
    const bool ambientShouldBeOn = (m_phase != Phase::BaselineCore);
    const bool railShouldBeOn = (m_phase == Phase::BaselineRail) || IsSamplePhase();

    if (!m_appliedAmbientKnown || ambientShouldBeOn != m_appliedAmbientOn) {
        if (ambientShouldBeOn) {
            sys.ambientLights.PowerOn();
        } else {
            sys.ambientLights.ShutDown();
        }
        m_appliedAmbientOn = ambientShouldBeOn;
        m_appliedAmbientKnown = true;
    }

    if (!m_appliedRailKnown || railShouldBeOn != m_appliedRailOn) {
        if (railShouldBeOn) {
            if (!firmware_internal::EnableLedRail(sys)) return;
        } else {
            if (!firmware_internal::QuiesceLedRail(sys)) return;
        }
        m_appliedRailOn = railShouldBeOn;
        m_appliedRailKnown = true;
    }

    if (IsSamplePhase()) {
        Render();
        return;
    }
    firmware_internal::ClearLedOutput(sys);
}

void Session::RestoreRuntimeHardwareState() {
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

void Session::Start(fxe::Console& console) {
    auto& sys = firmware_internal::GetSystem();

    if (sys.runtimeHardware.resolved.revision ==
        hardware::BoardRevision::V3_3S3) {
        console.PrintLn("amptest: V3.3 RGB profile unsupported; RGBW white-die calibration would be invalid");
        return;
    }
    if (!firmware_internal::QuiesceLedRail(sys)) {
        console.PrintLn("amptest: LED clear incomplete; runtime retained; retry start");
        return;
    }
    Reset();
    m_appliedRailKnown = true;
    m_appliedRailOn = false;

    m_active = true;
    m_phase = Phase::BaselineCore;

    sys.gamePersistence.RequestCancelAllThinking();
    sys.quickSettingsOverlayActive = false;
    sys.quickSettingsOverlayOwner = modules::UiOwner::None;
    sys.menuOverlay.Stop();
    sys.gameResetHoldActive = false;
    sys.gameResetHoldTriggered = false;

    console.PrintLn("amptest: step 0/%lu confirmed_black=1 5v_en=off",
                    static_cast<unsigned long>(TotalStepCount()));
    console.PrintLn("amptest: baseline steps record-and-advance; color steps record current level and stay on that color until Enter advances");
    console.PrintLn("amptest: WARNING raw characterization bypasses runtime current limiting; use a current-limited bench source and raise from level 1 deliberately");
    console.PrintLn("amptest: accepted current input examples -> 412, 412mA, 0.412A");
    ApplyCurrentHardwareState();
    PrintCurrentStep(console);
}

void Session::Stop(fxe::Console& console) {
    if (!m_active) {
        console.PrintLn("amptest: idle");
        return;
    }

    m_hasResults = (RecordedStepCount() != 0U);
    PrintSummary(console);
    m_active = false;
    m_phase = Phase::Inactive;
    RestoreRuntimeHardwareState();
    console.PrintLn("amptest: stopped");
}

} // namespace firmware::amptest

#endif
