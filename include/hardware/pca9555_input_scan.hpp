#pragma once

#include <cstdint>

#include "hardware/board_hardware_profile.hpp"
#include "hardware/hardware.hpp"
#include "hardware/pca9555_occupancy_decode.hpp"
#include "hardware/pca9555_scan_schedule.hpp"
#include "modules/board_input.hpp"

#ifdef ARDUINO
#include <Wire.h>

#include <PCA95x5.h>
#else
#include "foxie_embedded/fixed_function.hpp"
#endif

namespace hardware {

class Pca9555InputScan final {
  public:
#ifdef ARDUINO
    explicit Pca9555InputScan(TwoWire& wire);
#else
    Pca9555InputScan();

    using ScanCallback = fxe::FixedFunction<bool(uint64_t& outOccupiedMask), 64>;
    explicit Pca9555InputScan(ScanCallback cb);
#endif

    void Begin();

    // Must be called before Begin() on multi-revision firmware.
    void ConfigureHardwareProfile(const BoardHardwareProfile& profile) {
        m_hasCustomI2cPins = profile.hasCustomI2cPins;
        m_i2cSda = profile.i2cSda;
        m_i2cScl = profile.i2cScl;
        m_mapping = GetExpanderPinToSquareMap(profile.revision);
    }

    void SetInterruptDriven(const bool enabled);
    void SetBusClockHz(uint32_t hz) { m_busClockHz = hz; }
    void NotifyAnyInterrupt();
    void NotifyExpanderInterrupt(const uint8_t expanderIndex);

    // Request a complete four-expander scan on the next attempt.
    void RequestFullRead();

    // Success requires verified input/polarity configuration and a fresh full scan.
    modules::BoardScanAttemptResult AttemptScan();
    // Compatibility wrapper: true means a scheduled scan succeeded.
    bool Update();

    bool LastReadOk() const { return m_lastReadOk; }
    uint8_t LastRespondingExpanderMask() const { return m_respondingExpanderMask; }
    uint32_t GetI2cErrorCount() const { return m_i2cErrorCount; }
    uint32_t GetUpdateCounter() const { return m_updateCounter; }

    uint64_t GetOcc64() const { return m_occ64; }

    void CopyRawValues(uint16_t outValues[PCA9555_EXPANDER_COUNT]) const;

    bool IsOccupied(const uint8_t square) const {
        if (square >= 64) {
            return false;
        }
        return (m_occ64 & (1ULL << square)) != 0;
    }

  private:
#ifdef ARDUINO
    TwoWire* m_wire = nullptr;
    PCA9555 m_dev0;
    PCA9555 m_dev1;
    PCA9555 m_dev2;
    PCA9555 m_dev3;
    bool m_configurationValid = false;
#else
    ScanCallback m_scanCb = nullptr;
#endif

    bool m_interruptDriven = false;
    uint32_t m_busClockHz = 100000U;
    Pca9555ScanSchedule m_schedule;

    bool m_lastReadOk = false;
    uint8_t m_respondingExpanderMask = 0U;
    uint32_t m_i2cErrorCount = 0;
    uint32_t m_updateCounter = 0;

    uint64_t m_occ64 = 0;
    uint16_t m_raw[PCA9555_EXPANDER_COUNT] = {};
    bool m_hasCustomI2cPins = HW_HAS_CUSTOM_I2C_PINS;
    uint8_t m_i2cSda = PIN_I2C_SDA;
    uint8_t m_i2cScl = PIN_I2C_SCL;
    const uint8_t (*m_mapping)[PCA9555_PINS_PER_EXPANDER] =
        GetExpanderPinToSquareMap();
};

} // namespace hardware
