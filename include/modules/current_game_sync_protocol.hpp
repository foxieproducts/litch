#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace modules::current_game_sync {

inline constexpr uint8_t CURRENT_LINE_PRESENT = 1U << 0U;
inline constexpr uint8_t ARCHIVE_REQUIRED = 1U << 1U;
inline constexpr uint8_t LINE_SEALED = 1U << 2U;
inline constexpr uint8_t KNOWN_FLAGS = 0x07U;

enum class Opcode : uint8_t {
    Begin = 0x20U,
    State = 0x21U,
    Bounds = 0x22U,
    SegmentBegin = 0x23U,
    Data = 0x24U,
    SegmentCommit = 0x25U,
    SegmentCommitted = 0x26U,
    ArchiveAck = 0x27U,
    Archived = 0x28U,
};

struct ResumeCursor {
    uint32_t nextRecordSequence = 0U;
    std::array<uint8_t, 8U> prefixFingerprint{};
};

struct Digest {
    std::array<uint8_t, 12U> fingerprint{};
};

struct Bounds {
    uint64_t lineRevision = 0U;
    uint16_t recordCount = 0U;
    uint8_t flags = 0U;
};

bool IsKnownOpcode(uint8_t opcode);
bool IsValidResumeCursor(const ResumeCursor& cursor);
bool IsValidDigest(const Digest& digest, bool allowAbsent);
bool IsValidBounds(const Bounds& bounds);
bool EncodeResumeCursor(const ResumeCursor& cursor, uint8_t out[12]);
bool DecodeResumeCursor(const uint8_t* bytes, size_t length,
                        ResumeCursor& out);
bool EncodeDigest(const Digest& digest, bool allowAbsent, uint8_t out[12]);
bool DecodeDigest(const uint8_t* bytes, size_t length, bool allowAbsent,
                  Digest& out);
bool EncodeBounds(const Bounds& bounds, uint8_t out[12]);
bool DecodeBounds(const uint8_t* bytes, size_t length, Bounds& out);

} // namespace modules::current_game_sync
