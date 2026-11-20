#pragma once

#include <cstdint>

#include "modules/ota_post_boot_verifier.hpp"

namespace modules {

struct OtaRuntimeAcceptanceSample {
    bool loopBoundaryReached = false;
    bool recoveryTransportReady = false;
};

constexpr bool IsOtaRuntimeCandidateAcceptable(
    const OtaRuntimeAcceptanceSample sample) {
    // Artwork and session owners admit their own bounded working sets. OTA
    // health only proves the live loop and configured recovery transport;
    // duplicating a global heap policy here rejected valid candidates after
    // those owners initialized.
    return sample.loopBoundaryReached && sample.recoveryTransportReady;
}

constexpr bool RequiresOtaRuntimeAcceptance(
    const bool runningStateReadable,
    const OtaPostBootRunningState runningState) {
    return !runningStateReadable ||
           runningState == OtaPostBootRunningState::PendingVerify;
}

} // namespace modules
