#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "hardware/pca9555_scan_schedule.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[PCA9555_SCAN_SCHEDULE] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);

    hardware::Pca9555ScanSchedule schedule;
    Check(schedule.ShouldScan(), "boot requests a complete scan");
    auto snapshot = schedule.CaptureInterrupts();
    schedule.OnScanSucceeded(snapshot);
    Check(!schedule.ShouldScan(), "successful boot scan clears request");

    schedule.NotifyExpanderInterrupt(2U);
    Check(schedule.ShouldScan(), "one expander interrupt requests a scan");
    snapshot = schedule.CaptureInterrupts();
    schedule.NotifyExpanderInterrupt(1U);
    schedule.OnScanSucceeded(snapshot);
    Check(schedule.ShouldScan(), "interrupt during scan is not erased");

    snapshot = schedule.CaptureInterrupts();
    schedule.OnScanSucceeded(snapshot);
    Check(!schedule.ShouldScan(), "follow-up scan acknowledges later interrupt");

    fxe::ManualMillis::AdvanceMs(49U);
    Check(!schedule.ShouldScan(), "periodic reconciliation waits 50 ms");
    fxe::ManualMillis::AdvanceMs(1U);
    Check(schedule.ShouldScan(), "periodic reconciliation runs after 50 ms");

    snapshot = schedule.CaptureInterrupts();
    schedule.OnScanFailed();
    Check(schedule.ShouldScan(), "failed scan remains pending");
    schedule.OnScanSucceeded(snapshot);

    schedule.NotifyExpanderInterrupt(0U);
    schedule.NotifyExpanderInterrupt(1U);
    schedule.NotifyExpanderInterrupt(2U);
    schedule.NotifyExpanderInterrupt(3U);
    snapshot = schedule.CaptureInterrupts();
    schedule.OnScanSucceeded(snapshot);
    Check(!schedule.ShouldScan(), "four interrupt lines coalesce into one scan");

    if (g_failures == 0) {
        std::puts("[PCA9555_SCAN_SCHEDULE] ok");
        return 0;
    }
    std::printf("[PCA9555_SCAN_SCHEDULE] failures=%d\n", g_failures);
    return 1;
}
