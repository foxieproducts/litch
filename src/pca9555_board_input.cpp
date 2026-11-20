#include "hardware/pca9555_board_input.hpp"

#include "hardware/pca9555_occupancy_decode.hpp"

#include <utility>

#ifdef ARDUINO

#include "logger.hpp"

#include <cstring>

#include "hardware/hardware.hpp"

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
    // Inputs + inverted polarity per docs/spec/hardware_platform.md.
    const bool dirOk = d.direction(PCA95x5::Direction::IN_ALL);
    const bool polOk = d.polarity(PCA95x5::Polarity::INVERTED_ALL);
    return dirOk && polOk;
}

} // namespace

namespace hardware {

Pca9555BoardInput::Pca9555BoardInput(TwoWire& wire) {
    m_wire = &wire;
}

Pca9555BoardInput::Pca9555BoardInput(Pca9555InputScan& scan) {
    m_scan = &scan;
}

void Pca9555BoardInput::Begin() {
    if (m_scan) {
        m_scan->Begin();
        Resync();
        return;
    }

    if (!m_wire) {
        return;
    }

    AttachAll(m_dev0, m_dev1, m_dev2, m_dev3, *m_wire);

    // Best-effort configuration.
    if (!ConfigureInputs(m_dev0) || !ConfigureInputs(m_dev1) || !ConfigureInputs(m_dev2) || !ConfigureInputs(m_dev3)) {
        ++m_i2cErrorCount;
        logger::Warn("pca9555: config failed");
    }

    Resync();
}

bool Pca9555BoardInput::TryReadSnapshot(uint64_t& outOcc64) {
    return ReadSnapshot(outOcc64);
}

bool Pca9555BoardInput::ReadSnapshot(uint64_t& outOcc64) {
    if (m_scan) {
        const modules::BoardScanAttemptResult result = m_scan->AttemptScan();
        if (result.status == modules::BoardScanAttemptStatus::Succeeded) {
            outOcc64 = result.rawOccupiedMask;
            return true;
        }
        if (result.status == modules::BoardScanAttemptStatus::NotDue &&
            m_scan->LastReadOk()) {
            outOcc64 = m_scan->GetOcc64();
            return true;
        }

        outOcc64 = 0ULL;
        return false;
    }

    if (!m_wire) {
        outOcc64 = 0;
        return false;
    }

    const uint16_t o0 = U16FromOcc16(static_cast<uint16_t>(m_dev0.read()));
    const uint16_t o1 = U16FromOcc16(static_cast<uint16_t>(m_dev1.read()));
    const uint16_t o2 = U16FromOcc16(static_cast<uint16_t>(m_dev2.read()));
    const uint16_t o3 = U16FromOcc16(static_cast<uint16_t>(m_dev3.read()));

    // PCA95x5 library exposes the last I2C status code.
    const bool ok = (m_dev0.i2c_error() == 0) && (m_dev1.i2c_error() == 0) && (m_dev2.i2c_error() == 0) && (m_dev3.i2c_error() == 0);
    if (!ok) {
        ++m_i2cErrorCount;
    }

    outOcc64 = hardware::DecodePca9555Occupancy64(o0, o1, o2, o3);

    return ok;
}

bool Pca9555BoardInput::TryReadDelta(modules::SquareDelta& outDelta) {
    if (m_pendingDiffMask64 == 0) {
        if (m_deferNextSnapshot) {
            m_deferNextSnapshot = false;
            return false;
        }

        // Future hardware path: if interrupt-driven is enabled and no interrupt has been
        // observed since the last drained snapshot, do not poll I2C.
        if (m_interruptDriven && !m_interruptLatched) {
            return false;
        }

        uint64_t nowOcc64 = 0;
        const bool ok = ReadSnapshot(nowOcc64);
        if (!ok) {
            // Avoid emitting spurious deltas from a bad read.
            // Do not update state; allow the next higher-level batch to retry.
            return false;
        }

        m_interruptLatched = false;

        m_pendingOcc64 = nowOcc64;
        m_pendingDiffMask64 = (m_pendingOcc64 ^ m_prevOcc64);

        if (m_pendingDiffMask64 == 0) {
            return false;
        }

        // Ensure we only ever drain one snapshot per higher-level batch.
        m_deferNextSnapshot = true;
    }

    // Pop the lowest-index changed square.
    for (uint8_t sq = 0; sq < 64; ++sq) {
        const uint64_t mask = (1ULL << sq);
        if ((m_pendingDiffMask64 & mask) == 0) {
            continue;
        }

        const bool nowOccupied = (m_pendingOcc64 & mask) != 0;
        outDelta.square = sq;
        outDelta.nowOccupied = nowOccupied;

        // Update previous snapshot for this square and clear the diff bit.
        if (nowOccupied) {
            m_prevOcc64 |= mask;
        } else {
            m_prevOcc64 &= ~mask;
        }

        m_pendingDiffMask64 &= ~mask;
        return true;
    }

    // Should be unreachable; reset pending state.
    m_pendingDiffMask64 = 0;
    return false;
}

void Pca9555BoardInput::Resync() {
    uint64_t nowOcc64 = 0;
    if (m_scan) {
        m_scan->RequestFullRead();
    }
    (void)ReadSnapshot(nowOcc64);

    if (!m_scan) {
        m_interruptLatched = false;
    }

    m_prevOcc64 = nowOcc64;
    m_pendingOcc64 = nowOcc64;
    m_pendingDiffMask64 = 0;
    m_deferNextSnapshot = false;
}

} // namespace hardware

#else

namespace hardware {

Pca9555BoardInput::Pca9555BoardInput() {}

Pca9555BoardInput::Pca9555BoardInput(SnapshotCallback cb) {
    m_snapshotCb = std::move(cb);
}

Pca9555BoardInput::Pca9555BoardInput(Pca9555InputScan& scan) {
    m_scan = &scan;
}

void Pca9555BoardInput::Begin() {}

bool Pca9555BoardInput::TryReadSnapshot(uint64_t& outOcc64) {
    return ReadSnapshot(outOcc64);
}

bool Pca9555BoardInput::ReadSnapshot(uint64_t& outOcc64) {
    if (m_scan) {
        const modules::BoardScanAttemptResult result = m_scan->AttemptScan();
        if (result.status == modules::BoardScanAttemptStatus::Succeeded) {
            outOcc64 = result.rawOccupiedMask;
            return true;
        }
        if (result.status == modules::BoardScanAttemptStatus::NotDue &&
            m_scan->LastReadOk()) {
            outOcc64 = m_scan->GetOcc64();
            return true;
        }

        outOcc64 = 0ULL;
        return false;
    }

    if (!m_snapshotCb) {
        outOcc64 = 0;
        return false;
    }

    return m_snapshotCb(outOcc64);
}

bool Pca9555BoardInput::TryReadDelta(modules::SquareDelta& outDelta) {
    if (m_pendingDiffMask64 == 0) {
        if (m_deferNextSnapshot) {
            m_deferNextSnapshot = false;
            return false;
        }

        uint64_t nowOcc64 = 0;
        const bool ok = ReadSnapshot(nowOcc64);
        if (!ok) {
            return false;
        }

        m_pendingOcc64 = nowOcc64;
        m_pendingDiffMask64 = (m_pendingOcc64 ^ m_prevOcc64);

        if (m_pendingDiffMask64 == 0) {
            return false;
        }

        m_deferNextSnapshot = true;
    }

    for (uint8_t sq = 0; sq < 64; ++sq) {
        const uint64_t mask = (1ULL << sq);
        if ((m_pendingDiffMask64 & mask) == 0) {
            continue;
        }

        const bool nowOccupied = (m_pendingOcc64 & mask) != 0;
        outDelta.square = sq;
        outDelta.nowOccupied = nowOccupied;

        if (nowOccupied) {
            m_prevOcc64 |= mask;
        } else {
            m_prevOcc64 &= ~mask;
        }

        m_pendingDiffMask64 &= ~mask;
        return true;
    }

    m_pendingDiffMask64 = 0;
    return false;
}

void Pca9555BoardInput::Resync() {
    uint64_t nowOcc64 = 0;
    (void)ReadSnapshot(nowOcc64);

    m_prevOcc64 = nowOcc64;
    m_pendingOcc64 = nowOcc64;
    m_pendingDiffMask64 = 0;
    m_deferNextSnapshot = false;
}

} // namespace hardware

#endif

namespace hardware {

modules::BoardScanAttemptResult Pca9555BoardInput::CaptureScanAttempt() {
    if (m_scan) {
        return m_scan->AttemptScan();
    }

    if (m_interruptDriven && !m_interruptLatched) {
        return {};
    }

    uint64_t rawOccupiedMask = 0ULL;
    if (!ReadSnapshot(rawOccupiedMask)) {
        return {modules::BoardScanAttemptStatus::Failed, 0ULL};
    }

    m_interruptLatched = false;
    return {modules::BoardScanAttemptStatus::Succeeded, rawOccupiedMask};
}

bool Pca9555BoardInput::TryReadObservation(modules::BoardObservation& outObservation) {
    // Atomic callers request exactly one hardware scan per call, unlike the
    // legacy delta drain loop. Do not carry its one-call defer across polls.
    m_deferNextSnapshot = false;

    if (m_pendingDiffMask64 == 0ULL) {
        if (!m_scan && m_interruptDriven && !m_interruptLatched) {
            return false;
        }

        uint64_t nowOcc64 = 0;
        if (!ReadSnapshot(nowOcc64)) {
            return false;
        }

        if (!m_scan) {
            m_interruptLatched = false;
        }
        m_pendingOcc64 = nowOcc64;
        m_pendingDiffMask64 = m_pendingOcc64 ^ m_prevOcc64;
    }

    if (m_pendingDiffMask64 == 0ULL) {
        return false;
    }

    outObservation.changedMask = m_pendingDiffMask64;
    outObservation.occupiedMask = m_pendingOcc64;
    m_prevOcc64 = m_pendingOcc64;
    m_pendingDiffMask64 = 0ULL;
    return true;
}

} // namespace hardware
