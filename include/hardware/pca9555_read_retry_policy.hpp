#pragma once

#include <cstdint>

namespace hardware::pca9555_read_retry_policy {

constexpr uint8_t kMaxReadAttempts = 4;
constexpr uint8_t kResetAfterConsecutiveFailures = 2;

struct ReadRetryState {
    uint8_t consecutiveFailures = 0;
    bool busResetAttempted = false;
};

enum class ReadFailureAction : uint8_t {
    Retry = 0,
    ResetBusAndRetry = 1,
    GiveUp = 2,
};

inline ReadFailureAction HandleReadFailure(ReadRetryState& state, const uint8_t attemptNumber) {
    ++state.consecutiveFailures;

    if (!state.busResetAttempted &&
        state.consecutiveFailures >= kResetAfterConsecutiveFailures) {
        state.busResetAttempted = true;
        return ReadFailureAction::ResetBusAndRetry;
    }

    if (attemptNumber >= kMaxReadAttempts) {
        return ReadFailureAction::GiveUp;
    }

    return ReadFailureAction::Retry;
}

} // namespace hardware::pca9555_read_retry_policy