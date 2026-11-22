#pragma once

#include <cstddef>
#include <cstdint>

#include "hardware/pca9555_input_scan.hpp"
#include "modules/board_input.hpp"

#include "foxie_embedded/fixed_function.hpp"

#ifdef ARDUINO
#include <Wire.h>

#include <PCA95x5.h>
#endif

namespace hardware {

// Device-only board input backed by 4x PCA9555 expanders.
//
// Square numbering contract:
// - Exposes square indices 0..63 in the canonical orientation (0=a8, 63=h1)
//   BEFORE applying flip-sides. Higher layers apply flip-sides mapping.
class Pca9555BoardInput final : public modules::IBoardInput {
  public:
#ifdef ARDUINO
    explicit Pca9555BoardInput(TwoWire& wire);
#else
    Pca9555BoardInput();

    using SnapshotCallback = fxe::FixedFunction<bool(uint64_t& outOcc64), 64>;
    explicit Pca9555BoardInput(SnapshotCallback cb);
#endif
    explicit Pca9555BoardInput(Pca9555InputScan& scan);

    void Begin();

    // Future hardware: when enabled, the board input will only read I2C snapshots
    // after an expander interrupt is observed (avoids constant polling).
    // Default is false for the current ESP32-C3 prototype.
    void SetInterruptDriven(bool enabled) {
        if (m_scan) {
            m_scan->SetInterruptDriven(enabled);
            return;
        }

        m_interruptDriven = enabled;
        m_interruptLatched = true; // allow an initial snapshot read after enabling.
    }

    // Future hardware: call from an ISR/interrupt hook when any expander INT line fires.
    // This is intentionally lightweight; it only latches a flag.
    void NotifyAnyInterrupt() {
        if (m_scan) {
            m_scan->NotifyAnyInterrupt();
            return;
        }

        m_interruptLatched = true;
    }

    bool TryReadSnapshot(uint64_t& outOcc64) override;

    modules::BoardScanAttemptResult CaptureScanAttempt() override;

    bool TryReadDelta(modules::SquareDelta& outDelta) override;

    bool TryReadObservation(modules::BoardObservation& outObservation) override;

    bool SupportsAtomicObservations() const override { return true; }

    void Resync() override;

    uint32_t GetI2cErrorCount() const {
        if (m_scan) {
            return m_scan->GetI2cErrorCount();
        }
        return m_i2cErrorCount;
    }

  private:
#ifdef ARDUINO
    bool ReadSnapshot(uint64_t& outOcc64);
#else
    bool ReadSnapshot(uint64_t& outOcc64);
#endif

    uint64_t m_prevOcc64 = 0;

    uint64_t m_pendingOcc64 = 0;
    uint64_t m_pendingDiffMask64 = 0;

    // Prevent multiple I2C snapshot reads within a single higher-level drain
    // loop (ChessboardHardware::PollAndDispatch calls TryReadDelta() until false).
    // Once we have drained a snapshot's diffs, the next TryReadDelta() returns
    // false once before allowing another snapshot read.
    bool m_deferNextSnapshot = false;

    bool m_interruptDriven = false;
    bool m_interruptLatched = true;

    uint32_t m_i2cErrorCount = 0;

    Pca9555InputScan* m_scan = nullptr;

#ifdef ARDUINO
    TwoWire* m_wire = nullptr;
    PCA9555 m_dev0;
    PCA9555 m_dev1;
    PCA9555 m_dev2;
    PCA9555 m_dev3;
#else
    SnapshotCallback m_snapshotCb = nullptr;
#endif
};

} // namespace hardware
