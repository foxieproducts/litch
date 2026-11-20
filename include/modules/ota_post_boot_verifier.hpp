#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace modules {

inline constexpr size_t OTA_POST_BOOT_BUILD_ID_BYTES = 48U;
inline constexpr size_t OTA_POST_BOOT_SHA256_BYTES = 32U;
inline constexpr size_t OTA_POST_BOOT_SELECTOR_BYTES = 32U;
inline constexpr uint8_t OTA_POST_BOOT_NO_ENTRY = 0xFFU;

enum class OtaPostBootRunningState : uint8_t {
    Unknown = 0U,
    PendingVerify,
    Valid,
};

enum class OtaPostBootHealth : uint8_t {
    NotReported = 0U,
    Passed,
    Failed,
};

/** Exact bounded identity of an OTA application slot. */
struct OtaPostBootSlotIdentity {
    uint32_t address = 0U;
    uint8_t subtype = 0U;
};

/**
 * Owned raw SDK evidence, never a second boot journal.
 *
 * slots[0/1] are the actual supported S3 OTA partitions. The adapter verifies
 * their sizes/nonoverlap and corroborates selected/entry/sequence with the SDK.
 * The pure policy independently checks raw CRC, selection and slot mapping.
 */
struct OtaPostBootSnapshot {
    std::array<std::array<uint8_t, OTA_POST_BOOT_SELECTOR_BYTES>, 2U> selectors{};
    std::array<OtaPostBootSlotIdentity, 2U> slots{};
    OtaPostBootSlotIdentity running;
    OtaPostBootSlotIdentity selected;
    uint8_t selectedEntry = OTA_POST_BOOT_NO_ENTRY;
    uint32_t selectedSequence = 0U;
};

struct OtaPostBootSelectorEvidence {
    uint32_t sequence = 0U;
    uint32_t state = 0U;
    bool crcValid = false;
};

/** Validate supported slot identities and SDK selection, retaining raw states. */
bool DecodeCoherentOtaPostBootSnapshot(
    const OtaPostBootSnapshot& snapshot,
    std::array<OtaPostBootSelectorEvidence, 2U>& outSelectors);

enum class OtaPostBootDecision : uint8_t {
    UnusableSnapshot = 0U,
    AwaitingHealth,
    HealthPassed,
    HealthFailed,
    AlreadyValid,
    NoValidFallback,
};

enum class OtaPostBootAction : uint8_t {
    None = 0U,
    AwaitHealth,
    Confirm,
    AcceptValid,
    Rollback,
};

struct OtaPostBootEvaluation {
    OtaPostBootDecision decision = OtaPostBootDecision::UnusableSnapshot;
    OtaPostBootAction action = OtaPostBootAction::None;
    OtaPostBootRunningState runningState = OtaPostBootRunningState::Unknown;
    uint8_t fallbackEntry = OTA_POST_BOOT_NO_ENTRY;
    OtaPostBootSlotIdentity fallback;
};

/** Reduce a coherent SDK-selected/running snapshot; this never mutates flash. */
OtaPostBootEvaluation EvaluateOtaPostBootVerification(
    const OtaPostBootSnapshot& snapshot, OtaPostBootHealth health);

/** Exact owned comparison for the guard immediately before SDK mutation. */
bool OtaPostBootSnapshotsMatch(const OtaPostBootSnapshot& left,
                               const OtaPostBootSnapshot& right);

/** Only the selected SDK state bytes may change from PENDING to VALID. */
bool IsOtaPostBootConfirmationReadback(const OtaPostBootSnapshot& pending,
                                      const OtaPostBootSnapshot& valid);

} // namespace modules
