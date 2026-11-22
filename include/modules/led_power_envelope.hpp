#pragma once

#include <cstdint>

#include "hardware/usb_cc_sense.hpp"

namespace modules {

struct LedPowerEnvelope {
    uint16_t maxLedCurrentMa = 0U;
    uint16_t maxTotalCurrentMa = 0U;
    uint16_t fixedSystemCurrentMa = 0U;
};

constexpr uint16_t S3_MAX_LED_CURRENT_MA = 1000U;
constexpr uint16_t S3_FIXED_SYSTEM_CURRENT_MA = 234U;
// Nominal 500 mA protection budget retains 50 mA of source margin.
constexpr uint16_t S3_PROTECTION_SOURCE_BUDGET_MA = 500U;
constexpr uint16_t S3_PROTECTION_SOURCE_MARGIN_MA = 50U;
constexpr uint16_t S3_CONSERVATIVE_TOTAL_CURRENT_MA =
    S3_PROTECTION_SOURCE_BUDGET_MA - S3_PROTECTION_SOURCE_MARGIN_MA;
constexpr uint16_t S3_CONSERVATIVE_LED_HEADROOM_MA =
    S3_CONSERVATIVE_TOTAL_CURRENT_MA - S3_FIXED_SYSTEM_CURRENT_MA;
constexpr uint16_t S3_NORMAL_TOTAL_CURRENT_MA =
    S3_FIXED_SYSTEM_CURRENT_MA + S3_MAX_LED_CURRENT_MA;
constexpr uint16_t S3_CHANNEL_MAX_CURRENT_MA = 6U;
constexpr uint16_t S3_RED_CURRENT_MA_TIMES100 = 507U;
constexpr uint16_t S3_GREEN_CURRENT_MA_TIMES100 = 513U;
constexpr uint16_t S3_BLUE_CURRENT_MA_TIMES100 = 515U;
constexpr uint16_t S3_WHITE_CURRENT_MA_TIMES100 = 1222U;
// The source policy admits these pairs at least one second apart: t0/t1/t2.
constexpr uint8_t S3_STRONG_SOURCE_CONFIRMATION_SAMPLES = 3U;
constexpr uint32_t S3_CC_MONITOR_INTERVAL_MS = 10U;
constexpr uint32_t S3_CC_CHANGE_DEBOUNCE_MS = 20U;
enum class StrongSourceConfirmationTransition : uint8_t {
    None = 0,
    Gained,
    Lost,
};

struct StrongSourceConfirmationState {
    uint8_t samples = 0U;
};

struct StrongSourceConfirmationUpdate {
    StrongSourceConfirmationState state;
    StrongSourceConfirmationTransition transition =
        StrongSourceConfirmationTransition::None;
};

struct StrongSourceMonitorState {
    uint32_t downgradeCandidateSinceMs = 0U;
    bool downgradeCandidateActive = false;
};

struct StrongSourceMonitorUpdate {
    StrongSourceMonitorState state;
    bool downgradeStable = false;
};

inline bool IsStrongSourceConfirmed(const uint8_t samples) {
    return samples >= S3_STRONG_SOURCE_CONFIRMATION_SAMPLES;
}

inline bool IsStrongSourceConfirmed(
    const StrongSourceConfirmationState& state) {
    return IsStrongSourceConfirmed(state.samples);
}

inline bool ShouldContinueAutomaticStrongSourceSampling(
    const StrongSourceConfirmationState& state,
    const bool sampleValid) {
    if (!sampleValid) {
        return true;
    }
    return !IsStrongSourceConfirmed(state);
}

inline StrongSourceMonitorUpdate UpdateStrongSourceMonitorState(
    const StrongSourceMonitorState& current,
    const bool sourceStillAdvertisesAtLeast1500Ma,
    const uint32_t nowMs) {
    if (sourceStillAdvertisesAtLeast1500Ma) {
        return StrongSourceMonitorUpdate{};
    }

    if (!current.downgradeCandidateActive) {
        StrongSourceMonitorState started;
        started.downgradeCandidateSinceMs = nowMs;
        started.downgradeCandidateActive = true;
        return StrongSourceMonitorUpdate{started, false};
    }

    const bool stable =
        (nowMs - current.downgradeCandidateSinceMs) >=
        S3_CC_CHANGE_DEBOUNCE_MS;
    return StrongSourceMonitorUpdate{current, stable};
}

inline bool IsStrongSourceFullPairRecheckDue(
    const StrongSourceMonitorState& monitor,
    const bool fullPairRecheckPending,
    const uint32_t nowMs) {
    return fullPairRecheckPending &&
           monitor.downgradeCandidateActive &&
           (nowMs - monitor.downgradeCandidateSinceMs) >=
               S3_CC_CHANGE_DEBOUNCE_MS;
}

inline StrongSourceConfirmationState RevokeStrongSourceConfirmation(
    StrongSourceConfirmationState state) {
    state.samples = 0U;
    return state;
}

inline StrongSourceConfirmationUpdate UpdateStrongSourceConfirmationState(
    const StrongSourceConfirmationState& current,
    const bool sourceAdvertisesAtLeast1500Ma,
    const bool sourceMeetsConfirmationThreshold) {
    StrongSourceConfirmationState next = current;
    const bool wasConfirmed = IsStrongSourceConfirmed(current);
    if (!sourceAdvertisesAtLeast1500Ma) {
        next.samples = 0U;
        const StrongSourceConfirmationTransition transition =
            wasConfirmed ? StrongSourceConfirmationTransition::Lost
                         : StrongSourceConfirmationTransition::None;
        return StrongSourceConfirmationUpdate{next, transition};
    }
    if (wasConfirmed || !sourceMeetsConfirmationThreshold) {
        next.samples = wasConfirmed ? next.samples : 0U;
        return StrongSourceConfirmationUpdate{next};
    }

    ++next.samples;
    const StrongSourceConfirmationTransition transition =
        IsStrongSourceConfirmed(next)
            ? StrongSourceConfirmationTransition::Gained
            : StrongSourceConfirmationTransition::None;
    return StrongSourceConfirmationUpdate{next, transition};
}

inline LedPowerEnvelope SelectS3LedPowerEnvelope(const bool recoveryLatched) {
    LedPowerEnvelope envelope;
    envelope.maxLedCurrentMa = S3_MAX_LED_CURRENT_MA;
    envelope.fixedSystemCurrentMa = S3_FIXED_SYSTEM_CURRENT_MA;
    envelope.maxTotalCurrentMa =
        recoveryLatched
            ? S3_CONSERVATIVE_TOTAL_CURRENT_MA
            : S3_NORMAL_TOTAL_CURRENT_MA;
    return envelope;
}

} // namespace modules
