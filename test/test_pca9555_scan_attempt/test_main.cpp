#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/pca9555_input_scan.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

struct ScanSource {
    uint64_t occupiedMask = 0ULL;
    uint32_t calls = 0U;
    bool succeeds = true;
};

} // namespace

int main() {
    std::puts("[PCA9555_SCAN_ATTEMPT] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);

    ScanSource source;
    source.occupiedMask = 1ULL << 17U;
    hardware::Pca9555InputScan scan([&source](uint64_t& outOccupiedMask) {
        ++source.calls;
        outOccupiedMask = source.occupiedMask;
        return source.succeeds;
    });

    const modules::BoardScanAttemptResult first = scan.AttemptScan();
    Check(first.status == modules::BoardScanAttemptStatus::Succeeded,
          "scheduled scan reports success");
    Check(first.rawOccupiedMask == source.occupiedMask,
          "successful result carries the complete raw occupancy");
    Check(source.calls == 1U, "successful attempt reads the source once");

    const modules::BoardScanAttemptResult unchanged = scan.AttemptScan();
    Check(unchanged.status == modules::BoardScanAttemptStatus::Succeeded,
          "unchanged completed scan remains fresh successful evidence");
    Check(unchanged.rawOccupiedMask == source.occupiedMask,
          "unchanged successful result carries the complete raw occupancy");
    Check(source.calls == 2U, "unchanged attempt performs a fresh read");

    ScanSource scheduledSource;
    scheduledSource.occupiedMask = 1ULL << 4U;
    hardware::Pca9555InputScan scheduledScan(
        [&scheduledSource](uint64_t& outOccupiedMask) {
            ++scheduledSource.calls;
            outOccupiedMask = scheduledSource.occupiedMask;
            return scheduledSource.succeeds;
        });
    scheduledScan.SetInterruptDriven(true);

    const auto scheduledInitial = scheduledScan.AttemptScan();
    Check(scheduledInitial.status == modules::BoardScanAttemptStatus::Succeeded,
          "interrupt-driven startup performs its requested scan");
    Check(scheduledScan.GetOcc64() == scheduledSource.occupiedMask,
          "startup establishes last validated occupancy");

    const auto notDue = scheduledScan.AttemptScan();
    Check(notDue.status == modules::BoardScanAttemptStatus::NotDue,
          "interrupt-driven poll distinguishes no scan due");
    Check(notDue.rawOccupiedMask == 0ULL,
          "no-scan-due result carries no fresh evidence");
    Check(scheduledSource.calls == 1U,
          "no-scan-due result does not read the source");

    scheduledSource.succeeds = false;
    scheduledSource.occupiedMask = 1ULL << 9U;
    scheduledScan.NotifyExpanderInterrupt(2U);
    const auto failed = scheduledScan.AttemptScan();
    Check(failed.status == modules::BoardScanAttemptStatus::Failed,
          "attempted scan reports failure separately from no scan due");
    Check(failed.rawOccupiedMask == 0ULL,
          "failed result carries no fresh evidence");
    Check(scheduledScan.GetOcc64() == (1ULL << 4U),
          "failed attempt preserves last validated occupancy");

    scheduledSource.succeeds = true;
    const auto retried = scheduledScan.AttemptScan();
    Check(retried.status == modules::BoardScanAttemptStatus::Succeeded,
          "failed interrupt scan remains pending for retry");
    Check(retried.rawOccupiedMask == scheduledSource.occupiedMask,
          "successful retry carries the new complete occupancy");

    fxe::ManualMillis::AdvanceMs(49U);
    const auto beforeReconciliation = scheduledScan.AttemptScan();
    Check(beforeReconciliation.status == modules::BoardScanAttemptStatus::NotDue,
          "periodic reconciliation waits for the complete interval");
    fxe::ManualMillis::AdvanceMs(1U);
    const auto reconciliation = scheduledScan.AttemptScan();
    Check(reconciliation.status == modules::BoardScanAttemptStatus::Succeeded,
          "periodic reconciliation produces a fresh complete scan");

    if (g_failures == 0) {
        std::puts("[PCA9555_SCAN_ATTEMPT] ok");
        return 0;
    }
    std::printf("[PCA9555_SCAN_ATTEMPT] failures=%d\n", g_failures);
    return 1;
}
