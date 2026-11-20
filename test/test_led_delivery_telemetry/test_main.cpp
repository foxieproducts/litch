#include <cstdio>
#include "hardware/led_delivery_telemetry.hpp"

namespace {
int failures = 0;
void Check(const bool okay, const char* text) {
    if (!okay) { ++failures; std::printf("[FAIL] %s\n", text); }
}
void Submit(hardware::LedDeliveryTelemetry& telemetry, uint32_t atMs) {
    telemetry.OnAttempt(atMs);
    telemetry.OnSubmission(atMs + 2U);
    telemetry.OnOutcome(hardware::LedDeliveryOutcome::Confirmed, atMs + 10U);
}
void OutcomesAreNotTransfers() {
    hardware::LedDeliveryTelemetry telemetry;
    for (auto outcome : {hardware::LedDeliveryOutcome::FlashDenied,
                         hardware::LedDeliveryOutcome::BusWaitTimedOut,
                         hardware::LedDeliveryOutcome::Unchanged}) {
        telemetry.OnAttempt(100U);
        telemetry.OnOutcome(outcome, 101U);
    }
    telemetry.OnAttempt(110U);
    telemetry.OnSubmission(112U);
    telemetry.OnOutcome(hardware::LedDeliveryOutcome::Unconfirmed, 212U);
    auto snapshot = telemetry.GetSnapshot();
    Check(snapshot.attempts == 4U && snapshot.submissions == 1U &&
              snapshot.confirmations == 0U, "clean/denied/unconfirmed is not a confirmed TX");
    Check(snapshot.outcomes[0] == 1U && snapshot.outcomes[1] == 1U &&
              snapshot.outcomes[2] == 1U && snapshot.outcomes[3] == 1U,
          "all failure and unchanged branches retain distinct counts");
    Check(snapshot.maxShowMs == 102U && snapshot.maxPreSubmitMs == 2U,
          "blocking cost and pre-submit wait are measured separately");
    Submit(telemetry, 300U);
    snapshot = telemetry.GetSnapshot();
    Check(snapshot.submissions == 2U && snapshot.confirmations == 1U &&
              snapshot.submissionGaps[3] == 1U && snapshot.maxConfirmationGapMs == 0U,
          "submission gap includes failed TX; first confirmation invents no gap");
}
void WrapAndBoundedRecentGaps() {
    hardware::LedDeliveryTelemetry telemetry;
    Submit(telemetry, 0xfffffff0U);
    Submit(telemetry, 0x10U);
    auto snapshot = telemetry.GetSnapshot();
    Check(snapshot.maxSubmissionGapMs == 32U &&
              snapshot.maxConfirmationGapMs == 32U &&
              snapshot.submissionGaps[0] == 1U && snapshot.confirmationGaps[0] == 1U,
          "submission and confirmation intervals survive unsigned clock wrap");
    for (uint32_t i = 1U; i <= 10U; ++i) Submit(telemetry, 16U + i * 100U);
    snapshot = telemetry.GetSnapshot();
    Check(snapshot.recentLongGapCount == 8U && snapshot.recentLongGaps[0].atMs == 718U &&
              snapshot.recentLongGaps[7].atMs == 1026U,
          "long gap storage stays bounded and snapshots are chronological");
    Check(snapshot.submissionGaps[2] == 10U && snapshot.confirmationGaps[2] == 10U &&
              snapshot.maxShowMs == 10U, "histograms include only actual TX intervals");
}
}
int main() {
    OutcomesAreNotTransfers();
    WrapAndBoundedRecentGaps();
    std::printf("[led_delivery_telemetry] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
