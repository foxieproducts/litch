#include <cstdio>

#include "modules/runtime_health.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

modules::BoardScanAttemptResult Attempt(
    const modules::BoardScanAttemptStatus status,
    const uint64_t occupancy = 0ULL) {
    return modules::BoardScanAttemptResult{status, occupancy};
}

} // namespace

int main() {
    std::puts("[BOARD_INPUT_SCAN_RESULT] start");

    runtime::RuntimeHealthReducer reducer;
    modules::BoardInputRuntimeHealth health(reducer);
    const auto notDue = health.ObserveScanAttempt(
        5000U, Attempt(modules::BoardScanAttemptStatus::NotDue));
    Check(notDue.snapshot.state == runtime::RuntimeHealthState::Healthy &&
              notDue.snapshot.revision == 0U &&
              !notDue.dispatchObservation && !notDue.authoritativeResync,
          "NotDue is neither failure, success, nor dispatch evidence");

    const auto firstFailure = health.ObserveScanAttempt(
        0U, Attempt(modules::BoardScanAttemptStatus::Failed, UINT64_MAX));
    Check(firstFailure.snapshot.state == runtime::RuntimeHealthState::Healthy &&
              !firstFailure.dispatchObservation,
          "transient failed attempt stays quiet and never dispatches payload");
    (void)health.ObserveScanAttempt(
        99U, Attempt(modules::BoardScanAttemptStatus::Failed));
    const auto paused = health.ObserveScanAttempt(
        100U, Attempt(modules::BoardScanAttemptStatus::Failed));
    Check(paused.snapshot.state == runtime::RuntimeHealthState::InputPaused &&
              !paused.dispatchObservation,
          "continuous failure enters InputPaused at the bounded interval");

    const auto firstSuccess = health.ObserveScanAttempt(
        101U, Attempt(modules::BoardScanAttemptStatus::Succeeded, 0x11ULL));
    const auto almostRecovered = health.ObserveScanAttempt(
        200U, Attempt(modules::BoardScanAttemptStatus::Succeeded, 0x22ULL));
    Check(!firstSuccess.dispatchObservation &&
              !almostRecovered.dispatchObservation &&
              almostRecovered.snapshot.state ==
                  runtime::RuntimeHealthState::InputPaused,
          "fresh successes remain blocked until the full recovery interval");

    const auto recovered = health.ObserveScanAttempt(
        201U, Attempt(modules::BoardScanAttemptStatus::Succeeded, 0x33ULL));
    Check(recovered.snapshot.state == runtime::RuntimeHealthState::Healthy &&
              recovered.dispatchObservation &&
              recovered.authoritativeResync,
          "recovery authorizes exactly one fresh authoritative resync");

    const auto retry = health.ObserveScanAttempt(
        202U, Attempt(modules::BoardScanAttemptStatus::Succeeded, 0x33ULL));
    Check(retry.authoritativeResync,
          "unacknowledged recovery resync retries on fresh evidence");
    health.AcknowledgeAuthoritativeResync(true);

    const auto ordinary = health.ObserveScanAttempt(
        203U, Attempt(modules::BoardScanAttemptStatus::Succeeded, 0x33ULL));
    Check(ordinary.dispatchObservation && !ordinary.authoritativeResync,
          "post-recovery success returns to ordinary scan dispatch");

    if (g_failures == 0) {
        std::puts("[BOARD_INPUT_SCAN_RESULT] ok");
        return 0;
    }
    std::printf("[BOARD_INPUT_SCAN_RESULT] failures=%d\n", g_failures);
    return 1;
}
