#include "firmware/motion_wake_controller.hpp"

#include <algorithm>
#include <limits>

#include "hardware/lis2dw12_settings.hpp"

namespace firmware {
namespace {

bool SameSensorConfig(const hardware::Lis2dw12Config& first,
                      const hardware::Lis2dw12Config& second) {
    return first.enabled == second.enabled && first.mode == second.mode &&
           first.motionThresholdMg == second.motionThresholdMg &&
           first.motionDurationMs == second.motionDurationMs &&
           first.tapThresholdMg == second.tapThresholdMg &&
           first.tapShockMs == second.tapShockMs;
}

} // namespace

void MotionCalibrationAccumulator::Reset() {
    m_minX = 0;
    m_maxX = 0;
    m_minY = 0;
    m_maxY = 0;
    m_minZ = 0;
    m_maxZ = 0;
    m_sampleCount = 0U;
}

void MotionCalibrationAccumulator::Add(
    const hardware::Lis2dw12Sample& sample) {
    if (m_sampleCount == 0U) {
        m_minX = m_maxX = sample.milliGx;
        m_minY = m_maxY = sample.milliGy;
        m_minZ = m_maxZ = sample.milliGz;
    } else {
        m_minX = std::min(m_minX, sample.milliGx);
        m_maxX = std::max(m_maxX, sample.milliGx);
        m_minY = std::min(m_minY, sample.milliGy);
        m_maxY = std::max(m_maxY, sample.milliGy);
        m_minZ = std::min(m_minZ, sample.milliGz);
        m_maxZ = std::max(m_maxZ, sample.milliGz);
    }
    if (m_sampleCount != std::numeric_limits<uint16_t>::max()) {
        ++m_sampleCount;
    }
}

MotionCalibrationResult MotionCalibrationAccumulator::GetResult() const {
    MotionCalibrationResult result;
    result.sampleCount = m_sampleCount;
    if (m_sampleCount == 0U) {
        return result;
    }

    const int32_t rangeX = m_maxX - m_minX;
    const int32_t rangeY = m_maxY - m_minY;
    const int32_t rangeZ = m_maxZ - m_minZ;
    const uint32_t peakToPeak = static_cast<uint32_t>(
        std::max({rangeX, rangeY, rangeZ, static_cast<int32_t>(0)}));
    result.peakToPeakMg = static_cast<uint16_t>(
        std::min<uint32_t>(peakToPeak, std::numeric_limits<uint16_t>::max()));
    result.recommendedMotionThresholdMg = static_cast<uint16_t>(
        std::clamp<uint32_t>(peakToPeak * 2U, 125U, 1969U));
    return result;
}

void MotionWakeController::RecordError() {
    if (m_diagnostics.errorCount != std::numeric_limits<uint32_t>::max()) {
        ++m_diagnostics.errorCount;
    }
}

bool MotionWakeController::Begin(
    const hardware::BoardRevision revision,
    const hardware::Lis2dw12Transport& transport,
    const MotionWakePlatform& platform,
    const fxe::SettingsStore& settings) {
    m_platform = platform;
    m_diagnostics = {};
    m_settingsRevision = settings.GetRevision();
    m_wakeReady = false;
    m_stuckLowLatched = false;
    m_interactiveSampling = false;
    m_interactiveSamplingOwners = 0U;
    const auto profile = hardware::S3RevisionPinProfileFor(revision);
    m_interruptPin = profile.accelerometerInterrupt;
    m_supported = hardware::HasAccelerometer(revision);
    if (!m_supported) {
        return true;
    }
    if (m_interruptPin == hardware::NO_GPIO ||
        m_platform.configureInput == nullptr ||
        m_platform.readInterruptHigh == nullptr) {
        RecordError();
        return false;
    }

    m_platform.configureInput(m_platform.context, m_interruptPin);
    const auto config = hardware::lis2dw12_settings::Read(settings);
    if (!m_sensor.Begin(transport, config)) {
        RecordError();
        return false;
    }
    m_requestedConfig = config;
    ++m_diagnostics.configurationCount;

    hardware::Lis2dw12Event bootEvent;
    if (!m_sensor.ReadAndClearEvents(bootEvent)) {
        RecordError();
        return false;
    }
    m_diagnostics.lastEvent = bootEvent;
    return true;
}

bool MotionWakeController::ApplySettings(
    const fxe::SettingsStore& settings) {
    if (!m_supported || !m_sensor.IsAvailable()) {
        return !m_supported;
    }
    const auto config = hardware::lis2dw12_settings::Read(settings);
    if (!m_sensor.ApplyConfig(config) ||
        !m_sensor.SetInteractiveSampling(m_interactiveSampling)) {
        RecordError();
        return false;
    }
    m_requestedConfig = config;
    ++m_diagnostics.configurationCount;
    m_settingsRevision = settings.GetRevision();
    m_wakeReady = false;
    return true;
}

bool MotionWakeController::Update(const fxe::SettingsStore& settings) {
    if (!m_supported) {
        return true;
    }
    if (!m_sensor.IsAvailable()) {
        return false;
    }
    if (settings.GetRevision() != m_settingsRevision) {
        if (SameSensorConfig(hardware::lis2dw12_settings::Read(settings),
                             m_requestedConfig)) {
            m_settingsRevision = settings.GetRevision();
        } else if (!ApplySettings(settings)) {
            return false;
        }
    }
    return PollEvent();
}

bool MotionWakeController::CheckFactoryPresence(const bool runtimeUpdateOk) {
    return m_supported && runtimeUpdateOk && m_sensor.ProbeIdentity();
}

bool MotionWakeController::ApplyNow(const fxe::SettingsStore& settings) {
    return ApplySettings(settings);
}

bool MotionWakeController::SetInteractiveSampling(const bool enabled) {
    return SetInteractiveSampling(InteractiveSamplingOwner::Legacy, enabled);
}

bool MotionWakeController::SetInteractiveSampling(
    const InteractiveSamplingOwner owner, const bool enabled) {
    if (!m_supported) { return !enabled; }
    const uint8_t bit = static_cast<uint8_t>(owner);
    if (enabled) {
        m_interactiveSamplingOwners |= bit;
    } else {
        m_interactiveSamplingOwners &= static_cast<uint8_t>(~bit);
    }
    const bool requested = m_interactiveSamplingOwners != 0U;
    if (requested == m_interactiveSampling) {
        return m_sensor.IsAvailable();
    }
    if (!m_sensor.SetInteractiveSampling(requested)) {
        RecordError();
        return false;
    }
    m_interactiveSampling = requested;
    m_wakeReady = false;
    return true;
}

bool MotionWakeController::IsInterruptReleased() const {
    if (!m_supported) {
        return true;
    }
    return m_platform.readInterruptHigh != nullptr &&
           m_platform.readInterruptHigh(
               m_platform.context, m_interruptPin);
}

bool MotionWakeController::PollEvent() {
    if (!m_supported || !m_sensor.IsAvailable()) {
        return !m_supported;
    }
    if (IsInterruptReleased()) {
        m_stuckLowLatched = false;
        return true;
    }

    hardware::Lis2dw12Event event;
    if (!m_sensor.ReadAndClearEvents(event)) {
        RecordError();
        return false;
    }
    m_diagnostics.lastEvent = event;
    if (event.wakeUp || event.singleTap || event.doubleTap) {
        if (m_diagnostics.eventCount !=
            std::numeric_limits<uint32_t>::max()) {
            ++m_diagnostics.eventCount;
        }
    }
    if (!IsInterruptReleased() && !m_stuckLowLatched) {
        ++m_diagnostics.stuckLowCount;
        m_stuckLowLatched = true;
    }
    return true;
}

bool MotionWakeController::PrepareForSleep(
    const fxe::SettingsStore& settings) {
    m_wakeReady = false;
    m_interactiveSampling = false;
    m_interactiveSamplingOwners = 0U;
    if (!m_supported) {
        return true;
    }
    const hardware::Lis2dw12Config config =
        hardware::lis2dw12_settings::Read(settings);
    if (!m_sensor.IsAvailable()) {
        if (!config.enabled) {
            m_settingsRevision = settings.GetRevision();
            return true;
        }
        RecordError();
        return false;
    }

    if (!config.enabled) {
        if (!m_sensor.PowerDown()) {
            RecordError();
            return false;
        }
        m_requestedConfig = config;
        m_settingsRevision = settings.GetRevision();
        return true;
    }
    if (!m_sensor.ArmForSleep(config)) {
        RecordError();
        return false;
    }
    m_requestedConfig = config;
    ++m_diagnostics.configurationCount;
    m_settingsRevision = settings.GetRevision();

    if (!IsInterruptReleased()) {
        if (!m_stuckLowLatched) {
            ++m_diagnostics.stuckLowCount;
            m_stuckLowLatched = true;
        }
        return false;
    }
    m_stuckLowLatched = false;
    m_wakeReady = true;
    return true;
}

} // namespace firmware
