#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/rgb.hpp"
#include "modules/user_input.hpp"

namespace fxe {
class Console;
}

namespace firmware::amptest {

enum class LedMode : uint8_t {
    Red = 0,
    Green,
    Blue,
    White,
    Rgb,
    Rgbw,
    Count,
};

inline constexpr uint8_t LED_LEVEL_COUNT = 30U;
inline constexpr uint8_t SAFE_START_LEVEL = 1U;
inline constexpr size_t LED_MODE_COUNT = static_cast<size_t>(LedMode::Count);
inline constexpr size_t SAMPLE_LED_MODE_COUNT = 4U;

struct Measurement {
    bool recorded = false;
    uint32_t milliAmps = 0;
};

using SampleSeries = std::array<Measurement, LED_LEVEL_COUNT>;

struct InterpolatedEstimate {
    bool valid = false;
    uint8_t sampleCount = 0;
    uint32_t baselineMilliAmps = 0;
    int32_t estimatedFullScaleDeltaMilliAmps = 0;
    uint32_t estimatedFullScaleMilliAmps = 0;
    uint32_t milliAmpsPerLedTimes100 = 0;
    uint32_t milliAmpsPerChannelTimes100 = 0;
    uint32_t maxResidualMilliAmps = 0;
};

const char* LedModeToString(LedMode mode);
uint8_t LedModeActiveChannelCount(LedMode mode);
uint8_t LedLevelToPwm(uint8_t level1To30);
bool TryParseCurrentInput(std::string_view text, uint32_t& outMilliAmps);
InterpolatedEstimate InterpolateEstimate(const Measurement& baseline,
                                         const SampleSeries& samples,
                                         uint16_t ledCount,
                                         uint8_t activeChannelsPerLed);

#ifdef ARDUINO

struct VerifyPrediction {
    uint16_t requestedLedMilliAmps = 0;
    uint16_t appliedLedMilliAmps = 0;
    uint16_t predictedTotalMilliAmps = 0;
    uint16_t comparisonBaselineMilliAmps = 0;
    uint16_t activeLedLimitMilliAmps = 0;
    uint8_t requestedLevel1To30 = 0;
    uint8_t requestedPwm = 0;
    float appliedBrightnessScale01 = 1.0f;
    bool budgetLimited = false;
};

class Session final {
  public:
    static constexpr uint8_t MIN_LEVEL = 1U;
    static constexpr uint8_t MAX_LEVEL = LED_LEVEL_COUNT;

    void Start(fxe::Console& console);
    void Stop(fxe::Console& console);

    bool IsActive() const { return m_active; }
    bool HasResults() const { return m_hasResults; }

    bool HandleEnteredLine(fxe::Console& console, std::string_view line);
    bool HandleButtonEvent(const modules::ButtonEvent& event);

    void PrintStatus(fxe::Console& console) const;
    void Render() const;

  private:
    enum class Phase : uint8_t {
        Inactive = 0,
        BaselineCore,
        BaselineAmbient,
        BaselineRail,
        SampleRed,
        SampleGreen,
        SampleBlue,
        SampleWhite,
    };

    void Reset();
    void ApplyCurrentHardwareState();
    void RestoreRuntimeHardwareState();
    void PrintCurrentStep(fxe::Console& console) const;
    void PrintSummary(fxe::Console& console) const;
    bool Advance(fxe::Console& console);
    void RecordCurrent(fxe::Console& console, uint32_t milliAmps);
    void RecordSample(fxe::Console& console, uint32_t milliAmps);

    Measurement* CurrentMeasurement();
    const Measurement* CurrentMeasurement() const;
    SampleSeries* CurrentSamples();
    const SampleSeries* CurrentSamples() const;
    const Measurement* LedLoadBaseline() const;
    const char* LedLoadBaselineLabel() const;
    bool IsSamplePhase() const;
    size_t CurrentSampleModeIndex() const;
    LedMode CurrentLedMode() const;
    size_t CurrentStepIndex1() const;
    size_t TotalStepCount() const;
    size_t RecordedStepCount() const;
    size_t RecordedSampleCount(size_t modeIndex) const;

    bool m_active = false;
    bool m_hasResults = false;
    Phase m_phase = Phase::Inactive;
    uint8_t m_level1To30 = SAFE_START_LEVEL;
    Measurement m_coreBaseline = {};
    Measurement m_ambientBaseline = {};
    Measurement m_railBaseline = {};
    std::array<SampleSeries, SAMPLE_LED_MODE_COUNT> m_ledMeasurements = {};
    bool m_appliedAmbientKnown = false;
    bool m_appliedAmbientOn = false;
    bool m_appliedRailKnown = false;
    bool m_appliedRailOn = false;
    fxe::ElapsedTime m_lastLevelPrintElapsed;
};

class VerifySession final {
  public:
    enum class Step : uint8_t {
        CoreBaseline = 0,
        AmbientBaseline,
        RailBaseline,
        RedUnderLimit,
        GreenUnderLimit,
        BlueUnderLimit,
        NeutralUnderLimit,
        NeutralOverLimit,
        Count,
    };

    static constexpr size_t STEP_COUNT = static_cast<size_t>(Step::Count);

    void Start(fxe::Console& console);
    void Stop(fxe::Console& console);

    bool IsActive() const { return m_active; }
    bool HasResults() const { return m_hasResults; }

    bool HandleEnteredLine(fxe::Console& console, std::string_view line);
    void PrintStatus(fxe::Console& console) const;
    void Render();

  private:
    struct StepMeasurement {
        VerifyPrediction prediction;
        bool recorded = false;
        uint32_t actualTotalMilliAmps = 0;
    };

    void Reset();
    void ApplyCurrentHardwareState();
    void RestoreRuntimeHardwareState();
    void PrintCurrentStep(fxe::Console& console) const;
    void PrintSummary(fxe::Console& console) const;
    bool Advance(fxe::Console& console);
    void RecordActualMeasurement(fxe::Console& console, uint32_t milliAmps);

    StepMeasurement& CurrentMeasurement();
    const StepMeasurement& CurrentMeasurement() const;
    const char* CurrentStepName() const;
    const char* CurrentStepDescription() const;
    uint32_t CurrentReferenceBaselineMilliAmps() const;
    uint32_t CurrentComparisonBaselineMilliAmps() const;
    bool CurrentAmbientShouldBeOn() const;
    bool CurrentRailShouldBeOn() const;
    bool CurrentStepUsesLedLoad() const;
    bool CurrentStepExpectsBudgetLimit() const;
    modules::Rgb RequestedColorForLevel(uint8_t level1To30) const;
    uint8_t SelectRequestedLevel(uint16_t ledLimitMilliAmps);
    size_t CurrentStepIndex1() const;
    size_t RecordedStepCount() const;

    bool m_active = false;
    bool m_hasResults = false;
    Step m_step = Step::CoreBaseline;
    std::array<StepMeasurement, STEP_COUNT> m_measurements = {};
    bool m_appliedAmbientKnown = false;
    bool m_appliedAmbientOn = false;
    bool m_appliedRailKnown = false;
    bool m_appliedRailOn = false;
};

#endif

} // namespace firmware::amptest
