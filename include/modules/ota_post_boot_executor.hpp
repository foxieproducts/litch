#pragma once

#include <array>
#include <cstdint>

#include "modules/ota_post_boot_verifier.hpp"

namespace modules {

/** Exact identity returned only after the adapter verifies the complete image. */
struct OtaPostBootImageEvidence {
    OtaPostBootSlotIdentity slot;
    std::array<uint8_t, OTA_POST_BOOT_BUILD_ID_BYTES> buildId{};
    uint8_t buildIdLength = 0U;
    std::array<uint8_t, OTA_POST_BOOT_SHA256_BYTES> imageSha256{};
};

/**
 * Call only inside the existing Core0 worker's one owned request.
 * readSnapshot/SDK mutation execute under the executor's single flash lease;
 * readImage owns bounded per-read leases and complete software verification.
 * A successful invalidation normally reboots without returning. Its bool is
 * only API call success, never a pre-reboot readback or completed rollback.
 */
struct OtaPostBootExecutionOps {
    void* context = nullptr;
    bool (*acquireFlash)(void*) = nullptr;
    void (*releaseFlash)(void*) = nullptr;
    bool (*readSnapshot)(void*, OtaPostBootSnapshot&) = nullptr;
    bool (*readImage)(void*, const OtaPostBootSlotIdentity&,
                      OtaPostBootImageEvidence&) = nullptr;
    bool (*markValid)(void*) = nullptr;
    bool (*markInvalidAndReboot)(void*) = nullptr;
};

enum class OtaPostBootExecutionResult : uint8_t {
    Unsupported = 0U,
    NoAction,
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
};

/**
 * One worker-owned running-image proof retained only within its health ticket.
 * Preparation does not grant health, SDK mutation or public image identity.
 */
struct OtaPostBootPreparedImage {
    OtaPostBootSnapshot snapshot{};
    OtaPostBootImageEvidence image{};
};

/** Fully verify a coherent running image without reporting successful health. */
OtaPostBootExecutionResult PrepareOtaPostBootRunningImage(
    const OtaPostBootExecutionOps& ops, OtaPostBootPreparedImage& outPrepared);

/**
 * Only Passed may finish this running-image proof. Recheck the exact snapshot,
 * then guard/mutate/read back in one lease; Failed must use its own fallback proof.
 */
OtaPostBootExecutionResult FinishOtaPostBootRunningImage(
    const OtaPostBootExecutionOps& ops, const OtaPostBootPreparedImage& prepared,
    OtaPostBootHealth health);

/** Hash outside the final lease, then guard/mutate/read back without a gap. */
OtaPostBootExecutionResult ExecuteOtaPostBootVerification(
    const OtaPostBootExecutionOps& ops, OtaPostBootHealth health);

} // namespace modules
