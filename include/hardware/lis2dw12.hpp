#pragma once

#include <cstddef>
#include <cstdint>

namespace hardware {

namespace lis2dw12_register {

inline constexpr uint8_t WHO_AM_I = 0x0FU;
inline constexpr uint8_t CTRL1 = 0x20U;
inline constexpr uint8_t CTRL2 = 0x21U;
inline constexpr uint8_t CTRL3 = 0x22U;
inline constexpr uint8_t CTRL4_INT1 = 0x23U;
inline constexpr uint8_t CTRL6 = 0x25U;
inline constexpr uint8_t OUT_X_L = 0x28U;
inline constexpr uint8_t TAP_THS_X = 0x30U;
inline constexpr uint8_t TAP_THS_Y = 0x31U;
inline constexpr uint8_t TAP_THS_Z = 0x32U;
inline constexpr uint8_t INT_DUR = 0x33U;
inline constexpr uint8_t WAKE_UP_THS = 0x34U;
inline constexpr uint8_t WAKE_UP_DUR = 0x35U;
inline constexpr uint8_t WAKE_UP_SRC = 0x38U;
inline constexpr uint8_t TAP_SRC = 0x39U;
inline constexpr uint8_t ALL_INT_SRC = 0x3BU;
inline constexpr uint8_t CTRL7 = 0x3FU;

} // namespace lis2dw12_register

struct Lis2dw12Transport {
    using ReadFn = bool (*)(void* context,
                            uint8_t address,
                            uint8_t reg,
                            uint8_t* data,
                            size_t length);
    using WriteFn = bool (*)(void* context,
                             uint8_t address,
                             uint8_t reg,
                             const uint8_t* data,
                             size_t length);
    using DelayFn = void (*)(void* context, uint32_t milliseconds);

    void* context = nullptr;
    ReadFn read = nullptr;
    WriteFn write = nullptr;
    DelayFn delay = nullptr;
};

enum class Lis2dw12WakeMode : uint8_t {
    Motion = 0,
    Tap = 1,
    MotionAndTap = 2,
};

struct Lis2dw12Config {
    bool enabled = true;
    Lis2dw12WakeMode mode = Lis2dw12WakeMode::Motion;
    uint16_t motionThresholdMg = 125U;
    uint16_t motionDurationMs = 80U;
    uint16_t tapThresholdMg = 438U;
    uint16_t tapShockMs = 40U;
};

struct Lis2dw12AppliedConfig {
    bool enabled = false;
    Lis2dw12WakeMode mode = Lis2dw12WakeMode::Motion;
    uint16_t odrTenthsHz = 0U;
    uint16_t motionThresholdMg = 0U;
    uint16_t motionDurationMs = 0U;
    uint16_t tapThresholdMg = 0U;
    uint16_t tapShockMs = 0U;
    bool tapExperimental = false;
};

struct Lis2dw12Event {
    bool wakeUp = false;
    bool wakeX = false;
    bool wakeY = false;
    bool wakeZ = false;
    bool singleTap = false;
    bool doubleTap = false;
    bool tapX = false;
    bool tapY = false;
    bool tapZ = false;
    uint8_t rawWake = 0U;
    uint8_t rawTap = 0U;
    uint8_t rawAll = 0U;
};

struct Lis2dw12Sample {
    int16_t rawX = 0;
    int16_t rawY = 0;
    int16_t rawZ = 0;
    int32_t milliGx = 0;
    int32_t milliGy = 0;
    int32_t milliGz = 0;
};

enum class Lis2dw12Error : uint8_t {
    None = 0,
    InvalidTransport,
    Transport,
    WrongDeviceId,
};

/**
 * \brief Narrow LIS2DW12 driver for deterministic always-on wake behavior.
 *
 * The interrupt contract is deliberately fixed to push-pull, active-low and
 * latched so it can share ESP32-S3 EXT1 ANY_LOW with the board buttons. Reading
 * ALL_INT_SRC clears the latch before sleep entry.
 */
class Lis2dw12 final {
  public:
    static constexpr uint8_t I2C_ADDRESS = 0x19U;
    static constexpr uint8_t EXPECTED_DEVICE_ID = 0x44U;

    bool Begin(const Lis2dw12Transport& transport,
               const Lis2dw12Config& config);
    bool ApplyConfig(const Lis2dw12Config& config);
    bool ArmForSleep(const Lis2dw12Config& config);
    bool PowerDown();
    // Temporary awake sampling; preserves persisted wake/tap configuration.
    // Does not delay: consumers wait for the next complete output period.
    bool SetInteractiveSampling(bool enabled);

    bool ReadAndClearEvents(Lis2dw12Event& event);
    bool ReadSample(Lis2dw12Sample& sample);
    // Current read-only identity; transient failures preserve configured state
    // so a later probe can recover without changing wake/sampling registers.
    bool ProbeIdentity();

    bool IsAvailable() const { return m_available; }
    bool IsWakeArmed() const { return m_wakeArmed; }
    uint8_t GetDeviceId() const { return m_deviceId; }
    Lis2dw12Error GetLastError() const { return m_lastError; }
    const Lis2dw12AppliedConfig& GetAppliedConfig() const {
        return m_applied;
    }

  private:
    bool Read(uint8_t reg, uint8_t* data, size_t length);
    bool Write(uint8_t reg, uint8_t value);
    bool Fail(Lis2dw12Error error);
    static uint8_t QuantizeThreshold(uint16_t milliG, uint8_t steps);
    static uint8_t QuantizeDuration(uint16_t milliseconds,
                                    uint16_t odrHz,
                                    uint8_t maxValue);
    static int32_t RawToMilliG(int16_t raw, bool lowPowerMode1);

    Lis2dw12Transport m_transport{};
    Lis2dw12AppliedConfig m_applied{};
    Lis2dw12Error m_lastError = Lis2dw12Error::None;
    uint8_t m_deviceId = 0U;
    uint16_t m_motionDurationMs = 80U;
    bool m_available = false;
    bool m_wakeArmed = false;
};

} // namespace hardware
