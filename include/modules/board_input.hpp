#pragma once

#include <cstdint>

namespace modules {

enum class BoardScanAttemptStatus : uint8_t {
    NotDue = 0,
    Succeeded,
    Failed,
};

// Allocation-free result for one scheduled complete-board scan attempt.
// rawOccupiedMask is fresh evidence only when status is Succeeded.
struct BoardScanAttemptResult {
    BoardScanAttemptStatus status = BoardScanAttemptStatus::NotDue;
    uint64_t rawOccupiedMask = 0ULL;
};

struct SquareDelta {
    uint8_t square = 0; // 0..63
    bool nowOccupied = false;
};

struct BoardObservation {
    uint64_t changedMask = 0;
    uint64_t occupiedMask = 0;
};

class IBoardInput {
  public:
    virtual ~IBoardInput() = default;

    // \brief Read the current full occupancy snapshot if supported.
    // \note Returns squares in canonical orientation (0=a8 .. 63=h1), before flip-sides.
    // \note This call should be side-effect free with respect to delta generation.
    virtual bool TryReadSnapshot(uint64_t& outOcc64) = 0;

    // \brief Returns true if a new sensor delta is available.
    // \note This is intentionally pull-based for determinism and replay.
    virtual bool TryReadDelta(SquareDelta& outDelta) = 0;

    // \brief Returns one complete hardware scan as an atomic observation.
    // \note No chronology is implied between bits in changedMask.
    virtual bool TryReadObservation(BoardObservation&) { return false; }
    virtual bool SupportsAtomicObservations() const { return false; }

    // \brief Capture one scheduled complete-board scan attempt.
    // \note Only Succeeded carries a fresh raw occupancy observation.
    // \note The default adapts legacy changed-only atomic inputs.
    virtual BoardScanAttemptResult CaptureScanAttempt() {
        BoardObservation observation;
        if (!TryReadObservation(observation)) {
            return {};
        }
        return {
            BoardScanAttemptStatus::Succeeded,
            observation.occupiedMask,
        };
    }

    // \brief Force a full rescan/snapshot if supported.
    virtual void Resync() = 0;
};

} // namespace modules
