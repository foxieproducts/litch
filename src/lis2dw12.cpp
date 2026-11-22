#include "hardware/lis2dw12.hpp"

#include <algorithm>

namespace hardware {
namespace {

constexpr uint8_t CTRL2_SOFT_RESET = 0x40U;
constexpr uint8_t CTRL2_BDU_AUTO_INCREMENT = 0x0CU;
constexpr uint8_t CTRL3_ACTIVE_LOW_LATCHED_PUSH_PULL = 0x18U;
constexpr uint8_t CTRL4_INT1_WAKE = 0x20U;
constexpr uint8_t CTRL4_INT1_SINGLE_TAP = 0x40U;
constexpr uint8_t CTRL6_FS2_LOW_NOISE = 0x04U;
constexpr uint8_t CTRL7_INTERRUPTS_ENABLE = 0x20U;
constexpr uint8_t CTRL1_MOTION_LOW_POWER_25HZ = 0x30U;
constexpr uint8_t CTRL1_INTERACTIVE_LOW_POWER_25HZ = 0x30U;
constexpr uint8_t CTRL1_TAP_HIGH_PERFORMANCE_400HZ = 0x74U;
constexpr uint16_t MOTION_ODR_HZ = 25U;
constexpr uint16_t MOTION_ODR_TENTHS_HZ = 250U;
constexpr uint16_t TAP_ODR_HZ = 400U;

bool UsesMotion(const Lis2dw12WakeMode mode) {
    return mode == Lis2dw12WakeMode::Motion ||
           mode == Lis2dw12WakeMode::MotionAndTap;
}

bool UsesTap(const Lis2dw12WakeMode mode) {
    return mode == Lis2dw12WakeMode::Tap ||
           mode == Lis2dw12WakeMode::MotionAndTap;
}

uint16_t ThresholdToMilliG(const uint8_t raw, const uint8_t steps) {
    return static_cast<uint16_t>((static_cast<uint32_t>(raw) * 2000U) /
                                 steps);
}

uint16_t DurationToMilliseconds(const uint8_t raw,
                                const bool tapMode) {
    if (tapMode) {
        return static_cast<uint16_t>((static_cast<uint32_t>(raw) * 1000U) /
                                     TAP_ODR_HZ);
    }
    return static_cast<uint16_t>((static_cast<uint32_t>(raw) * 10000U) /
                                 MOTION_ODR_TENTHS_HZ);
}

} // namespace

bool Lis2dw12::Read(const uint8_t reg,
                    uint8_t* const data,
                    const size_t length) {
    return m_transport.read != nullptr &&
           m_transport.read(
               m_transport.context, I2C_ADDRESS, reg, data, length);
}

bool Lis2dw12::Write(const uint8_t reg, const uint8_t value) {
    return m_transport.write != nullptr &&
           m_transport.write(
               m_transport.context, I2C_ADDRESS, reg, &value, 1U);
}

bool Lis2dw12::Fail(const Lis2dw12Error error) {
    m_available = false;
    m_wakeArmed = false;
    m_lastError = error;
    return false;
}

uint8_t Lis2dw12::QuantizeThreshold(const uint16_t milliG,
                                    const uint8_t steps) {
    const uint32_t rounded =
        (static_cast<uint32_t>(milliG) * steps + 1000U) / 2000U;
    return static_cast<uint8_t>(std::clamp<uint32_t>(rounded, 1U, steps - 1U));
}

uint8_t Lis2dw12::QuantizeDuration(const uint16_t milliseconds,
                                   const uint16_t odrHz,
                                   const uint8_t maxValue) {
    const uint32_t numerator = static_cast<uint32_t>(milliseconds) * odrHz;
    const uint32_t roundedUp =
        (numerator == 0U) ? 0U : ((numerator + 999U) / 1000U);
    return static_cast<uint8_t>(
        std::min<uint32_t>(roundedUp, maxValue));
}

int32_t Lis2dw12::RawToMilliG(const int16_t raw,
                              const bool lowPowerMode1) {
    if (lowPowerMode1) {
        return ((static_cast<int32_t>(raw) / 16) * 976) / 1000;
    }
    return ((static_cast<int32_t>(raw) / 4) * 244) / 1000;
}

bool Lis2dw12::Begin(const Lis2dw12Transport& transport,
                     const Lis2dw12Config& config) {
    m_transport = transport;
    m_applied = {};
    m_deviceId = 0U;
    m_available = false;
    m_wakeArmed = false;
    m_lastError = Lis2dw12Error::None;

    if (m_transport.read == nullptr || m_transport.write == nullptr ||
        m_transport.delay == nullptr) {
        return Fail(Lis2dw12Error::InvalidTransport);
    }
    if (!Read(lis2dw12_register::WHO_AM_I, &m_deviceId, 1U)) {
        return Fail(Lis2dw12Error::Transport);
    }
    if (m_deviceId != EXPECTED_DEVICE_ID) {
        return Fail(Lis2dw12Error::WrongDeviceId);
    }
    if (!Write(lis2dw12_register::CTRL2, CTRL2_SOFT_RESET)) {
        return Fail(Lis2dw12Error::Transport);
    }
    m_transport.delay(m_transport.context, 2U);
    m_available = true;
    return ApplyConfig(config);
}

bool Lis2dw12::ApplyConfig(const Lis2dw12Config& config) {
    if (!m_available) {
        return false;
    }

    m_wakeArmed = false;
    if (!Write(lis2dw12_register::CTRL7, 0U) ||
        !Write(lis2dw12_register::CTRL1, 0U) ||
        !Write(lis2dw12_register::CTRL2, CTRL2_BDU_AUTO_INCREMENT)) {
        return Fail(Lis2dw12Error::Transport);
    }
    if (!config.enabled) {
        m_applied = {};
        m_lastError = Lis2dw12Error::None;
        return true;
    }

    const bool tap = UsesTap(config.mode);
    const bool motion = UsesMotion(config.mode);
    const uint16_t odrHz = tap ? TAP_ODR_HZ : MOTION_ODR_HZ;
    const uint8_t motionThreshold =
        QuantizeThreshold(config.motionThresholdMg, 64U);
    const uint8_t tapThreshold =
        QuantizeThreshold(config.tapThresholdMg, 32U);
    const uint8_t motionDuration = tap
                                       ? QuantizeDuration(
                                             config.motionDurationMs,
                                             TAP_ODR_HZ,
                                             3U)
                                       : static_cast<uint8_t>(
                                             std::min<uint32_t>(
                                                 (static_cast<uint32_t>(
                                                      config.motionDurationMs) *
                                                      MOTION_ODR_TENTHS_HZ +
                                                  9999U) /
                                                     10000U,
                                                 3U));
    const uint8_t tapShock =
        QuantizeDuration(config.tapShockMs, TAP_ODR_HZ / 8U, 3U);
    const uint8_t intRoute =
        static_cast<uint8_t>((motion ? CTRL4_INT1_WAKE : 0U) |
                             (tap ? CTRL4_INT1_SINGLE_TAP : 0U));

    const bool configured =
        Write(lis2dw12_register::CTRL3,
              CTRL3_ACTIVE_LOW_LATCHED_PUSH_PULL) &&
        Write(lis2dw12_register::CTRL4_INT1, intRoute) &&
        Write(lis2dw12_register::CTRL6,
              tap ? CTRL6_FS2_LOW_NOISE : 0U) &&
        Write(lis2dw12_register::TAP_THS_X,
              tap ? tapThreshold : 0U) &&
        Write(lis2dw12_register::TAP_THS_Y,
              tap ? tapThreshold : 0U) &&
        Write(lis2dw12_register::TAP_THS_Z,
              tap ? static_cast<uint8_t>(0xE0U | tapThreshold) : 0U) &&
        Write(lis2dw12_register::INT_DUR,
              tap ? tapShock : 0U) &&
        Write(lis2dw12_register::WAKE_UP_THS,
              motion ? motionThreshold : 0U) &&
        Write(lis2dw12_register::WAKE_UP_DUR,
              motion ? static_cast<uint8_t>(motionDuration << 5U) : 0U) &&
        Write(lis2dw12_register::CTRL1,
              tap ? CTRL1_TAP_HIGH_PERFORMANCE_400HZ
                  : CTRL1_MOTION_LOW_POWER_25HZ);
    if (!configured) {
        (void)Write(lis2dw12_register::CTRL7, 0U);
        (void)Write(lis2dw12_register::CTRL1, 0U);
        return Fail(Lis2dw12Error::Transport);
    }

    m_transport.delay(m_transport.context, tap ? 3U : 80U);
    if (!Write(lis2dw12_register::CTRL7, CTRL7_INTERRUPTS_ENABLE)) {
        (void)Write(lis2dw12_register::CTRL1, 0U);
        return Fail(Lis2dw12Error::Transport);
    }

    m_applied.enabled = true;
    m_applied.mode = config.mode;
    m_applied.odrTenthsHz =
        tap ? static_cast<uint16_t>(TAP_ODR_HZ * 10U)
            : MOTION_ODR_TENTHS_HZ;
    m_applied.motionThresholdMg =
        motion ? ThresholdToMilliG(motionThreshold, 64U) : 0U;
    m_applied.motionDurationMs =
        motion ? DurationToMilliseconds(motionDuration, tap) : 0U;
    m_applied.tapThresholdMg =
        tap ? ThresholdToMilliG(tapThreshold, 32U) : 0U;
    m_applied.tapShockMs =
        tap ? static_cast<uint16_t>(tapShock * 8U * 1000U / TAP_ODR_HZ)
            : 0U;
    m_applied.tapExperimental = tap;
    m_motionDurationMs = config.motionDurationMs;
    m_lastError = Lis2dw12Error::None;
    m_wakeArmed = true;
    return true;
}

bool Lis2dw12::SetInteractiveSampling(const bool enabled) {
    if (!m_available) { return false; }
    if (!m_applied.enabled || m_applied.tapExperimental) { return true; }
    // Handling and sleep share 25 Hz, so entry/exit never rewrite registers.
    const uint16_t odrTenthsHz = enabled ? 250U : MOTION_ODR_TENTHS_HZ;
    if (m_applied.odrTenthsHz == odrTenthsHz) { return true; }
    const uint8_t duration = static_cast<uint8_t>(std::min<uint32_t>(
        (uint32_t(m_motionDurationMs) * odrTenthsHz + 9999U) / 10000U, 3U));
    if (!Write(lis2dw12_register::WAKE_UP_DUR, duration << 5U) ||
        !Write(lis2dw12_register::CTRL1,
               enabled ? CTRL1_INTERACTIVE_LOW_POWER_25HZ
                       : CTRL1_MOTION_LOW_POWER_25HZ)) {
        return Fail(Lis2dw12Error::Transport);
    }
    m_applied.odrTenthsHz = odrTenthsHz;
    m_applied.motionDurationMs = uint32_t(duration) * 10000U / odrTenthsHz;
    m_wakeArmed = false;
    return true;
}

bool Lis2dw12::ArmForSleep(const Lis2dw12Config& config) {
    if (!ApplyConfig(config) || !config.enabled) {
        return false;
    }
    Lis2dw12Event discarded;
    if (!ReadAndClearEvents(discarded)) {
        m_wakeArmed = false;
        return false;
    }
    m_wakeArmed = true;
    return true;
}

bool Lis2dw12::PowerDown() {
    if (!m_available) {
        return false;
    }
    m_wakeArmed = false;
    const bool ok = Write(lis2dw12_register::CTRL7, 0U) &&
                    Write(lis2dw12_register::CTRL1, 0U);
    if (!ok) {
        return Fail(Lis2dw12Error::Transport);
    }
    m_applied = {};
    return true;
}

bool Lis2dw12::ProbeIdentity() {
    if (!m_available) return false;
    uint8_t deviceId = 0U;
    if (!Read(lis2dw12_register::WHO_AM_I, &deviceId, 1U)) {
        m_lastError = Lis2dw12Error::Transport;
        return false;
    }
    if (deviceId != EXPECTED_DEVICE_ID) {
        m_lastError = Lis2dw12Error::WrongDeviceId;
        return false;
    }
    m_lastError = Lis2dw12Error::None;
    return true;
}

bool Lis2dw12::ReadAndClearEvents(Lis2dw12Event& event) {
    if (!m_available) {
        return false;
    }
    uint8_t wake = 0U;
    uint8_t tap = 0U;
    uint8_t all = 0U;
    if (!Read(lis2dw12_register::WAKE_UP_SRC, &wake, 1U) ||
        !Read(lis2dw12_register::TAP_SRC, &tap, 1U) ||
        !Read(lis2dw12_register::ALL_INT_SRC, &all, 1U)) {
        m_lastError = Lis2dw12Error::Transport;
        return false;
    }

    Lis2dw12Event decoded;
    decoded.rawWake = wake;
    decoded.rawTap = tap;
    decoded.rawAll = all;
    decoded.wakeUp = (wake & 0x08U) != 0U;
    decoded.wakeX = (wake & 0x04U) != 0U;
    decoded.wakeY = (wake & 0x02U) != 0U;
    decoded.wakeZ = (wake & 0x01U) != 0U;
    decoded.singleTap = (tap & 0x20U) != 0U;
    decoded.doubleTap = (tap & 0x10U) != 0U;
    decoded.tapX = (tap & 0x04U) != 0U;
    decoded.tapY = (tap & 0x02U) != 0U;
    decoded.tapZ = (tap & 0x01U) != 0U;
    event = decoded;
    m_lastError = Lis2dw12Error::None;
    return true;
}

bool Lis2dw12::ReadSample(Lis2dw12Sample& sample) {
    if (!m_available) {
        return false;
    }
    uint8_t bytes[6] = {};
    if (!Read(lis2dw12_register::OUT_X_L, bytes, sizeof(bytes))) {
        m_lastError = Lis2dw12Error::Transport;
        return false;
    }

    Lis2dw12Sample decoded;
    decoded.rawX = static_cast<int16_t>(
        static_cast<uint16_t>(bytes[0]) |
        (static_cast<uint16_t>(bytes[1]) << 8U));
    decoded.rawY = static_cast<int16_t>(
        static_cast<uint16_t>(bytes[2]) |
        (static_cast<uint16_t>(bytes[3]) << 8U));
    decoded.rawZ = static_cast<int16_t>(
        static_cast<uint16_t>(bytes[4]) |
        (static_cast<uint16_t>(bytes[5]) << 8U));
    const bool lowPowerMode1 = !m_applied.tapExperimental;
    decoded.milliGx = RawToMilliG(decoded.rawX, lowPowerMode1);
    decoded.milliGy = RawToMilliG(decoded.rawY, lowPowerMode1);
    decoded.milliGz = RawToMilliG(decoded.rawZ, lowPowerMode1);
    sample = decoded;
    m_lastError = Lis2dw12Error::None;
    return true;
}

} // namespace hardware
