#pragma once

#include <cstdint>

#include "foxie_embedded/settings_store.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "hardware/lis2dw12.hpp"

namespace firmware {

enum class InteractiveSamplingOwner : uint8_t {
    Legacy = 1U << 0U,
    WaterSlosh = 1U << 1U,
    Litchcraft = 1U << 2U,
};

struct MotionWakePlatform {
    using ConfigureInputFn = void (*)(void* context, uint8_t pin);
    using ReadInterruptHighFn = bool (*)(void* context, uint8_t pin);

    void* context = nullptr;
    ConfigureInputFn configureInput = nullptr;
    ReadInterruptHighFn readInterruptHigh = nullptr;
};

struct MotionWakeDiagnostics {
    uint32_t eventCount = 0U;
    uint32_t configurationCount = 0U;
    uint32_t errorCount = 0U;
    uint32_t stuckLowCount = 0U;
    hardware::Lis2dw12Event lastEvent{};
};

struct MotionCalibrationResult {
    uint16_t sampleCount = 0U;
    uint16_t peakToPeakMg = 0U;
    uint16_t recommendedMotionThresholdMg = 125U;
};

class MotionCalibrationAccumulator final {
  public:
    void Reset();
    void Add(const hardware::Lis2dw12Sample& sample);
    [[nodiscard]] MotionCalibrationResult GetResult() const;

  private:
    int32_t m_minX = 0;
    int32_t m_maxX = 0;
    int32_t m_minY = 0;
    int32_t m_maxY = 0;
    int32_t m_minZ = 0;
    int32_t m_maxZ = 0;
    uint16_t m_sampleCount = 0U;
};

/**
 * \brief Revision-gated owner for the V3 always-on accelerometer.
 *
 * Unsupported boards return success without touching I2C. A configured V3
 * only becomes sleep-ready after its latched active-low interrupt has been
 * cleared and observed high.
 */
class MotionWakeController final {
  public:
    bool Begin(hardware::BoardRevision revision,
               const hardware::Lis2dw12Transport& transport,
               const MotionWakePlatform& platform,
               const fxe::SettingsStore& settings);
    bool Update(const fxe::SettingsStore& settings);
    // Factory qualification requires a current checked identity independently
    // of IRQ level, together with the current runtime update result.
    bool CheckFactoryPresence(bool runtimeUpdateOk);
    bool ApplyNow(const fxe::SettingsStore& settings);
    bool PrepareForSleep(const fxe::SettingsStore& settings);
    bool PollEvent();
    bool SetInteractiveSampling(bool enabled);
    bool SetInteractiveSampling(InteractiveSamplingOwner owner, bool enabled);

    bool IsSupported() const { return m_supported; }
    bool IsAvailable() const { return m_sensor.IsAvailable(); }
    bool IsWakeReady() const { return m_wakeReady; }
    bool IsInterruptReleased() const;
    uint8_t GetInterruptPin() const { return m_interruptPin; }

    hardware::Lis2dw12& GetSensor() { return m_sensor; }
    const hardware::Lis2dw12& GetSensor() const { return m_sensor; }
    const MotionWakeDiagnostics& GetDiagnostics() const {
        return m_diagnostics;
    }

  private:
    bool ApplySettings(const fxe::SettingsStore& settings);
    void RecordError();

    hardware::Lis2dw12 m_sensor;
    MotionWakePlatform m_platform{};
    MotionWakeDiagnostics m_diagnostics{};
    // Compare canonical requests, not the sensor's quantized register values.
    hardware::Lis2dw12Config m_requestedConfig{};
    uint32_t m_settingsRevision = 0U;
    uint8_t m_interruptPin = hardware::NO_GPIO;
    bool m_supported = false;
    bool m_wakeReady = false;
    bool m_stuckLowLatched = false;
    bool m_interactiveSampling = false;
    uint8_t m_interactiveSamplingOwners = 0U;
};

} // namespace firmware
