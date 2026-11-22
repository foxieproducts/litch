#include "hardware/pca9555_input_scan.hpp"

#include <utility>

#ifdef ARDUINO

#include "logger.hpp"

#include "hardware/hardware.hpp"
#include "hardware/pca9555_read_retry_policy.hpp"

namespace {
constexpr uint16_t U16FromOcc16(const uint16_t v) {
    return v;
}

void AttachAll(PCA9555& d0, PCA9555& d1, PCA9555& d2, PCA9555& d3, TwoWire& wire) {
    d0.attach(wire, hardware::PCA9555_ADDR_0);
    d1.attach(wire, hardware::PCA9555_ADDR_1);
    d2.attach(wire, hardware::PCA9555_ADDR_2);
    d3.attach(wire, hardware::PCA9555_ADDR_3);
}

bool ConfigureInputs(PCA9555& d) {
    const bool dirOk = d.direction(PCA95x5::Direction::IN_ALL);
    const bool polOk = d.polarity(PCA95x5::Polarity::INVERTED_ALL);
    return dirOk && polOk;
}

bool ConfigureAllInputs(PCA9555& d0, PCA9555& d1,
                        PCA9555& d2, PCA9555& d3) {
    PCA9555* devices[4U] = {&d0, &d1, &d2, &d3};
    bool configured = true;
    for (auto* device : devices) {
        if (!ConfigureInputs(*device)) configured = false;
    }
    return configured;
}

bool ResetI2cBusAndReinitializeExpanders(TwoWire& wire,
                                         PCA9555& d0,
                                         PCA9555& d1,
                                         PCA9555& d2,
                                         PCA9555& d3,
                                         const uint32_t clockHz,
                                         const bool hasCustomI2cPins,
                                         const uint8_t i2cSda,
                                         const uint8_t i2cScl) {
    wire.end();
    if (hasCustomI2cPins) {
        wire.begin(i2cSda, i2cScl);
    } else {
        wire.begin();
    }
    wire.setClock(clockHz);

    AttachAll(d0, d1, d2, d3, wire);
    return ConfigureAllInputs(d0, d1, d2, d3);
}

} // namespace

namespace hardware {

Pca9555InputScan::Pca9555InputScan(TwoWire& wire) {
    m_wire = &wire;
}

void Pca9555InputScan::Begin() {
    if (!m_wire) {
        return;
    }

    AttachAll(m_dev0, m_dev1, m_dev2, m_dev3, *m_wire);

    m_configurationValid = false;
    m_schedule.RequestFullRead();
    (void)Update();
}

void Pca9555InputScan::SetInterruptDriven(const bool enabled) {
    m_interruptDriven = enabled;
    m_schedule.RequestFullRead();
}

void IRAM_ATTR Pca9555InputScan::NotifyAnyInterrupt() {
    m_schedule.NotifyAnyInterrupt();
}

void IRAM_ATTR Pca9555InputScan::NotifyExpanderInterrupt(const uint8_t expanderIndex) {
    if (expanderIndex >= PCA9555_EXPANDER_COUNT) {
        return;
    }
    m_schedule.NotifyExpanderInterrupt(expanderIndex);
}

void Pca9555InputScan::RequestFullRead() {
    m_schedule.RequestFullRead();
}

void Pca9555InputScan::CopyRawValues(uint16_t outValues[PCA9555_EXPANDER_COUNT]) const {
    if (!outValues) {
        return;
    }

    for (uint8_t i = 0; i < PCA9555_EXPANDER_COUNT; ++i) {
        outValues[i] = m_raw[i];
    }
}

modules::BoardScanAttemptResult Pca9555InputScan::AttemptScan() {
    if (!m_wire) {
        m_respondingExpanderMask = 0U;
        m_lastReadOk = false;
        m_schedule.OnScanFailed();
        return {modules::BoardScanAttemptStatus::Failed, 0ULL};
    }

    auto readWithRetry = [&](PCA9555& dev, const uint8_t expanderIndex, uint16_t& outRaw) -> bool {
        hardware::pca9555_read_retry_policy::ReadRetryState retryState;

        for (uint8_t attempt = 1;
             attempt <= hardware::pca9555_read_retry_policy::kMaxReadAttempts;
             ++attempt) {
            uint16_t candidate = 0U;
            if (dev.read(candidate)) {
                outRaw = U16FromOcc16(candidate);
                return true;
            }
            const uint8_t err = dev.i2c_error();

            ++m_i2cErrorCount;
            logger::Warn("pca9555: read failed exp=%u attempt=%u err=%u",
                         static_cast<unsigned>(expanderIndex),
                         static_cast<unsigned>(attempt),
                         static_cast<unsigned>(err));

            const auto action = hardware::pca9555_read_retry_policy::HandleReadFailure(retryState, attempt);
            if (action == hardware::pca9555_read_retry_policy::ReadFailureAction::ResetBusAndRetry && m_wire) {
                logger::Warn("pca9555: resetting i2c bus after %u consecutive failures",
                             static_cast<unsigned>(retryState.consecutiveFailures));

                m_configurationValid = ResetI2cBusAndReinitializeExpanders(
                        *m_wire, m_dev0, m_dev1, m_dev2, m_dev3,
                        m_busClockHz, m_hasCustomI2cPins,
                        m_i2cSda, m_i2cScl);
                if (!m_configurationValid) {
                    ++m_i2cErrorCount;
                    logger::Warn("pca9555: i2c bus reset/reinit failed");
                    return false;
                }
            }

            if (action == hardware::pca9555_read_retry_policy::ReadFailureAction::GiveUp) {
                break;
            }
        }

        return false;
    };

    const auto readAll = [&]() -> modules::BoardScanAttemptResult {
        // A readable input register does not prove polarity was configured.
        // Retry one complete configuration pass per pending scan; never publish
        // a mixture of configured and unconfigured expander values.
        m_respondingExpanderMask = 0U;
        if (!m_configurationValid) {
            m_configurationValid = ConfigureAllInputs(
                m_dev0, m_dev1, m_dev2, m_dev3);
            if (!m_configurationValid) {
                ++m_i2cErrorCount;
                logger::Warn("pca9555: input scan config failed");
                m_lastReadOk = false;
                m_schedule.OnScanFailed();
                return {modules::BoardScanAttemptStatus::Failed, 0ULL};
            }
        }
        const auto interruptSnapshot = m_schedule.CaptureInterrupts();
        uint16_t raw[PCA9555_EXPANDER_COUNT] = {};
        // Attempt each fitted device even when another fails; publish occupancy
        // only when the complete scan succeeds, but retain truthful diagnostics.
        PCA9555* devices[4] = {&m_dev0, &m_dev1, &m_dev2, &m_dev3};
        for (uint8_t index = 0U; index < 4U; ++index)
            if (readWithRetry(*devices[index], index, raw[index]))
                m_respondingExpanderMask |= static_cast<uint8_t>(1U << index);
        if (!m_configurationValid || m_respondingExpanderMask != 0x0FU) {
            m_lastReadOk = false;
            m_schedule.OnScanFailed();
            return {modules::BoardScanAttemptStatus::Failed, 0ULL};
        }

        for (uint8_t i = 0; i < PCA9555_EXPANDER_COUNT; ++i) {
            m_raw[i] = raw[i];
        }
        m_schedule.OnScanSucceeded(interruptSnapshot);
        m_lastReadOk = true;
        ++m_updateCounter;
        m_occ64 = hardware::DecodePca9555Occupancy64(
            m_raw[0], m_raw[1], m_raw[2], m_raw[3], m_mapping);
        return {modules::BoardScanAttemptStatus::Succeeded, m_occ64};
    };

    if (!m_interruptDriven || m_schedule.ShouldScan()) {
        return readAll();
    }
    return {};
}

bool Pca9555InputScan::Update() {
    return AttemptScan().status == modules::BoardScanAttemptStatus::Succeeded;
}

} // namespace hardware

#else

namespace hardware {

Pca9555InputScan::Pca9555InputScan() {}

Pca9555InputScan::Pca9555InputScan(ScanCallback cb)
    : m_scanCb(std::move(cb)) {}

void Pca9555InputScan::Begin() {}

void Pca9555InputScan::SetInterruptDriven(const bool enabled) {
    m_interruptDriven = enabled;
    m_schedule.RequestFullRead();
}

void Pca9555InputScan::NotifyAnyInterrupt() {
    m_schedule.NotifyAnyInterrupt();
}

void Pca9555InputScan::NotifyExpanderInterrupt(const uint8_t expanderIndex) {
    m_schedule.NotifyExpanderInterrupt(expanderIndex);
}

void Pca9555InputScan::RequestFullRead() {
    m_schedule.RequestFullRead();
}

void Pca9555InputScan::CopyRawValues(uint16_t outValues[PCA9555_EXPANDER_COUNT]) const {
    if (!outValues) {
        return;
    }

    for (uint8_t i = 0; i < PCA9555_EXPANDER_COUNT; ++i) {
        outValues[i] = 0;
    }
}

modules::BoardScanAttemptResult Pca9555InputScan::AttemptScan() {
    if (m_interruptDriven && !m_schedule.ShouldScan()) {
        return {};
    }

    const auto interruptSnapshot = m_schedule.CaptureInterrupts();
    uint64_t candidateOccupancy = 0ULL;
    if (!m_scanCb || !m_scanCb(candidateOccupancy)) {
        m_respondingExpanderMask = 0U;
        m_lastReadOk = false;
        m_schedule.OnScanFailed();
        return {modules::BoardScanAttemptStatus::Failed, 0ULL};
    }

    m_schedule.OnScanSucceeded(interruptSnapshot);
    m_lastReadOk = true;
    ++m_updateCounter;
    m_respondingExpanderMask = 0x0FU;
    m_occ64 = candidateOccupancy;
    return {modules::BoardScanAttemptStatus::Succeeded, candidateOccupancy};
}

bool Pca9555InputScan::Update() {
    return AttemptScan().status == modules::BoardScanAttemptStatus::Succeeded;
}

} // namespace hardware

#endif
