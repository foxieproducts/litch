#pragma once

#include <cstdint>

namespace hardware {

enum class BoardRevision : uint8_t {
    Unknown = 0U,
    V1C3 = 1U,
    V2S3 = 2U,
    V3S3 = 3U,
    V3_3S3 = 4U,
};

constexpr bool HasFourLedSquares(const BoardRevision revision) {
    return revision == BoardRevision::V3S3 ||
           revision == BoardRevision::V3_3S3;
}

struct S3RevisionStrapSample {
    bool gpio35IsLow = false;
    bool gpio36IsLow = false;
    bool gpio37IsLow = false;
};

// Active-low revision straps have binary weights 1, 2 and 4, not a
// resistor count. V3.3 fits R3/GPIO35 and R4/GPIO36: board version 3.
// This raw board version is separate from the stable BoardRevision/BLE ID.
constexpr BoardRevision DetectS3BoardRevision(
    const S3RevisionStrapSample sample) {
    const uint8_t boardVersion = static_cast<uint8_t>(
        (sample.gpio35IsLow ? 1U : 0U) |
        (sample.gpio36IsLow ? 2U : 0U) |
        (sample.gpio37IsLow ? 4U : 0U));
    switch (boardVersion) {
        case 0U:
            return BoardRevision::V2S3;
        case 1U:
            return BoardRevision::V3S3;
        case 3U:
            return BoardRevision::V3_3S3;
        default:
            return BoardRevision::Unknown;
    }
}

// Preserve deployed V2 behavior until a supported strap pattern is certain.
constexpr BoardRevision ResolveBoardRevision(const BoardRevision revision) {
    return (revision == BoardRevision::Unknown) ? BoardRevision::V2S3
                                                 : revision;
}

// Add future revisions only after they have a concrete identity and complete
// production-test profile. Unknown must not inherit support through fallback.
constexpr bool SupportsProductionHardwareTest(const BoardRevision revision) {
    return revision == BoardRevision::V2S3 || HasFourLedSquares(revision);
}

constexpr BoardRevision BoardRevisionFromId(const uint8_t revisionId) {
    switch (revisionId) {
        case static_cast<uint8_t>(BoardRevision::V1C3):
            return BoardRevision::V1C3;
        case static_cast<uint8_t>(BoardRevision::V2S3):
            return BoardRevision::V2S3;
        case static_cast<uint8_t>(BoardRevision::V3S3):
            return BoardRevision::V3S3;
        case static_cast<uint8_t>(BoardRevision::V3_3S3):
            return BoardRevision::V3_3S3;
        default:
            return BoardRevision::Unknown;
    }
}

} // namespace hardware
