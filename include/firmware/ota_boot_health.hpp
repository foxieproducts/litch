#pragma once

#include <array>
#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/ota_post_boot_verifier.hpp"

namespace hardware {
class RmtFlashArbiter;
}

namespace firmware {

// One boot's progress belongs to FirmwareSystem. Timers start on its first
// runtime poll, after setup, rather than during static construction.
struct RuntimeBootHealthState {
    fxe::ElapsedTime sinceLoopStart;
    fxe::ElapsedTime sinceCommitAttempt;
    bool boundaryReached = false;
    uint32_t startupCompletedAtMs = UINT32_MAX;
    bool commitFinished = false;
    bool crashGuardHealthy = false;
    uint8_t commitErrors = 0U;
    uint32_t healthTicket = 0U;
    modules::OtaPostBootHealth requestedHealth = modules::OtaPostBootHealth::NotReported;
    std::array<uint8_t, modules::OTA_POST_BOOT_SHA256_BYTES> imageSha256{};
};

enum class OtaBootHealthCommitResult : uint8_t {
    Unsupported = 0U,
    NoPendingImage,
    AwaitingHealth,
    Confirmed,
    RollbackRequested,
    SnapshotUnavailable,
    ImageEvidenceUnavailable,
    EvidenceChanged,
    NoValidFallback,
    ConfirmFailed,
    ConfirmReadbackFailed,
    RollbackFailed,
    Pending,
};

/** Read-only SDK preflight; no filesystem, image hash or boot mutation. */
OtaBootHealthCommitResult CheckPendingOtaAtBoot(
    hardware::RmtFlashArbiter& flashArbiter);

/** Begin/poll full integrity without granting health or changing SDK state. */
OtaBootHealthCommitResult PreparePendingOtaBootHealth(RuntimeBootHealthState& state);

/** Nonblocking full-integrity/SDK proof; Pending retains the exact worker job. */
OtaBootHealthCommitResult CommitPendingOtaAtHealthBoundary(
    RuntimeBootHealthState& state);

/** Request rollback only with a distinct, fully verified SDK VALID fallback. */
OtaBootHealthCommitResult ReportPendingOtaBootFailure(
    RuntimeBootHealthState& state);

} // namespace firmware
