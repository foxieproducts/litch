#include <cstdio>

#include "hardware/pca9555_board_input.hpp"
#include "hardware/pca9555_input_scan.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

struct SnapshotSource {
    uint64_t occ64 = 0;
    uint32_t calls = 0;
    bool ok = true;
};

} // namespace

int main() {
    std::puts("[PCA9555_BOARD_INPUT_NATIVE] start");

    SnapshotSource src;
    src.occ64 = 0;

    hardware::Pca9555BoardInput input([&src](uint64_t& outOcc64) {
        ++src.calls;
        outOcc64 = src.occ64;
        return src.ok;
    });
    input.Resync();
    Check(src.calls == 1, "resync reads snapshot once");

    uint64_t snap = 123;
    src.calls = 0;
    Check(input.TryReadSnapshot(snap), "try read snapshot ok");
    Check(snap == 0, "snapshot matches source");
    Check(src.calls == 1, "try read snapshot calls source once");

    // Change two squares at once.
    src.occ64 = (1ULL << 2) | (1ULL << 10);

    modules::SquareDelta d;

    // Simulate one PollAndDispatch drain batch.
    src.calls = 0;
    int deltaCount = 0;
    while (input.TryReadDelta(d)) {
        if (deltaCount == 0) {
            Check(d.square == 2, "lowest changed square first");
            Check(d.nowOccupied, "square 2 now occupied");
        } else if (deltaCount == 1) {
            Check(d.square == 10, "second changed square next");
            Check(d.nowOccupied, "square 10 now occupied");
        } else {
            Check(false, "unexpected extra delta");
        }
        ++deltaCount;
    }

    Check(deltaCount == 2, "drains exactly two deltas");
    Check(src.calls == 1, "reads exactly one snapshot per drain batch");

    // The drain loop's terminating false return consumes the defer flag.
    // The next batch should read a new snapshot.
    src.calls = 0;
    Check(!input.TryReadDelta(d), "no changes => no delta");
    Check(src.calls == 1, "next batch reads snapshot again");

    // Remove one square.
    src.occ64 = (1ULL << 10);
    src.calls = 0;
    deltaCount = 0;
    while (input.TryReadDelta(d)) {
        Check(d.square == 2, "removed square emitted");
        Check(!d.nowOccupied, "removed square now unoccupied");
        ++deltaCount;
    }
    Check(deltaCount == 1, "single removal delta");
    Check(src.calls == 1, "removal batch reads one snapshot");

    // Failed snapshot reads should not emit deltas or corrupt state.
    src.ok = false;
    src.occ64 = (1ULL << 1) | (1ULL << 10);
    src.calls = 0;
    Check(!input.TryReadDelta(d), "failed snapshot returns no delta");
    Check(src.calls == 1, "failed snapshot read attempted once");

    // Recovery on next batch: successful snapshot should now produce the missing delta.
    src.ok = true;
    src.calls = 0;
    deltaCount = 0;
    while (input.TryReadDelta(d)) {
        Check(d.square == 1, "recovered delta square 1");
        Check(d.nowOccupied, "recovered delta occupied");
        ++deltaCount;
    }
    Check(deltaCount == 1, "recovery produces exactly one delta");
    Check(src.calls == 1, "recovery batch reads one snapshot");

    // Atomic consumers receive the complete scan change as one observation,
    // without an implied ordering between changed squares.
    SnapshotSource atomicSrc;
    atomicSrc.occ64 = (1ULL << 56) | (1ULL << 60);
    hardware::Pca9555BoardInput atomicInput([&atomicSrc](uint64_t& outOcc64) {
        ++atomicSrc.calls;
        outOcc64 = atomicSrc.occ64;
        return atomicSrc.ok;
    });
    atomicInput.Resync();

    atomicSrc.calls = 0;
    atomicSrc.occ64 = 0;
    modules::BoardObservation observation;
    Check(atomicInput.TryReadObservation(observation),
          "two-square scan yields one atomic observation");
    Check(observation.changedMask == ((1ULL << 56) | (1ULL << 60)),
          "atomic observation preserves complete changed mask");
    Check(observation.occupiedMask == 0,
          "atomic observation preserves complete new occupancy");
    Check(atomicSrc.calls == 1,
          "atomic observation reads one hardware snapshot");
    Check(!atomicInput.TryReadObservation(observation),
          "next atomic poll reports no change");
    Check(atomicSrc.calls == 2,
          "each atomic poll reads exactly one snapshot");

    SnapshotSource typedSrc;
    typedSrc.occ64 = 1ULL << 23U;
    hardware::Pca9555InputScan scan([&typedSrc](uint64_t& outOcc64) {
        ++typedSrc.calls;
        outOcc64 = typedSrc.occ64;
        return typedSrc.ok;
    });
    hardware::Pca9555BoardInput typedInput(scan);

    const auto fresh = typedInput.CaptureScanAttempt();
    Check(fresh.status == modules::BoardScanAttemptStatus::Succeeded,
          "board input exposes successful typed scan attempt");
    Check(fresh.rawOccupiedMask == typedSrc.occ64,
          "typed board result carries complete raw occupancy");

    const auto unchangedFresh = typedInput.CaptureScanAttempt();
    Check(unchangedFresh.status == modules::BoardScanAttemptStatus::Succeeded,
          "successful unchanged board scan remains fresh evidence");
    Check(unchangedFresh.rawOccupiedMask == typedSrc.occ64,
          "unchanged board scan keeps the complete raw occupancy");

    typedSrc.ok = false;
    typedSrc.occ64 = 1ULL << 31U;
    const auto failedAttempt = typedInput.CaptureScanAttempt();
    Check(failedAttempt.status == modules::BoardScanAttemptStatus::Failed,
          "board input exposes actual scan failure");
    Check(failedAttempt.rawOccupiedMask == 0ULL,
          "failed board scan carries no fresh evidence");
    Check(scan.GetOcc64() == (1ULL << 23U),
          "failed board scan preserves last validated driver occupancy");

    SnapshotSource compatibilitySrc;
    compatibilitySrc.occ64 = 1ULL << 41U;
    hardware::Pca9555InputScan compatibilityScan(
        [&compatibilitySrc](uint64_t& outOcc64) {
            ++compatibilitySrc.calls;
            outOcc64 = compatibilitySrc.occ64;
            return compatibilitySrc.ok;
        });
    compatibilityScan.SetInterruptDriven(true);
    hardware::Pca9555BoardInput compatibilityInput(compatibilityScan);
    const auto compatibilityFresh = compatibilityInput.CaptureScanAttempt();
    Check(compatibilityFresh.status ==
              modules::BoardScanAttemptStatus::Succeeded,
          "compatibility setup captures an initial fresh scan");

    uint64_t compatibilitySnapshot = 0ULL;
    Check(compatibilityInput.TryReadSnapshot(compatibilitySnapshot),
          "bool snapshot wrapper returns validated cache when no scan is due");
    Check(compatibilitySnapshot == compatibilitySrc.occ64,
          "bool snapshot wrapper preserves cached occupancy semantics");
    Check(compatibilitySrc.calls == 1U,
          "cached bool snapshot does not perform an unscheduled scan");

    compatibilitySrc.ok = false;
    compatibilitySrc.occ64 = 1ULL << 42U;
    compatibilityScan.NotifyAnyInterrupt();
    Check(!compatibilityInput.TryReadSnapshot(compatibilitySnapshot),
          "bool snapshot wrapper does not return stale cache after failure");
    Check(compatibilityScan.GetOcc64() == (1ULL << 41U),
          "failed bool wrapper attempt preserves driver last-known state");

    if (g_failures == 0) {
        std::puts("[PCA9555_BOARD_INPUT_NATIVE] ok");
        return 0;
    }

    std::printf("[PCA9555_BOARD_INPUT_NATIVE] failures=%d\n", g_failures);
    return 1;
}
