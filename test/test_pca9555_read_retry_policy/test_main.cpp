#include <cstdio>

#include "hardware/pca9555_read_retry_policy.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[PCA9555_READ_RETRY_POLICY] start");

    {
        hardware::pca9555_read_retry_policy::ReadRetryState state;
        const auto action = hardware::pca9555_read_retry_policy::HandleReadFailure(state, 1);
        Check(action == hardware::pca9555_read_retry_policy::ReadFailureAction::Retry,
              "first failure retries without bus reset");
        Check(state.consecutiveFailures == 1, "first failure increments consecutive count");
        Check(!state.busResetAttempted, "first failure does not mark bus reset attempted");
    }

    {
        hardware::pca9555_read_retry_policy::ReadRetryState state;
        (void)hardware::pca9555_read_retry_policy::HandleReadFailure(state, 1);
        const auto action = hardware::pca9555_read_retry_policy::HandleReadFailure(state, 2);
        Check(action == hardware::pca9555_read_retry_policy::ReadFailureAction::ResetBusAndRetry,
              "second consecutive failure requests one bus reset");
        Check(state.consecutiveFailures == 2, "second failure updates consecutive count");
        Check(state.busResetAttempted, "second failure marks bus reset attempted");
    }

    {
        hardware::pca9555_read_retry_policy::ReadRetryState state;
        (void)hardware::pca9555_read_retry_policy::HandleReadFailure(state, 1);
        (void)hardware::pca9555_read_retry_policy::HandleReadFailure(state, 2);
        const auto third = hardware::pca9555_read_retry_policy::HandleReadFailure(state, 3);
        const auto fourth = hardware::pca9555_read_retry_policy::HandleReadFailure(state, 4);
        Check(third == hardware::pca9555_read_retry_policy::ReadFailureAction::Retry,
              "third failure retries after a single reset attempt");
        Check(fourth == hardware::pca9555_read_retry_policy::ReadFailureAction::GiveUp,
              "fourth failure gives up after max attempts");
        Check(state.busResetAttempted, "later failures do not clear bus reset state");
    }

    if (g_failures == 0) {
        std::puts("[PCA9555_READ_RETRY_POLICY] ok");
        return 0;
    }

    std::printf("[PCA9555_READ_RETRY_POLICY] failures=%d\n", g_failures);
    return 1;
}