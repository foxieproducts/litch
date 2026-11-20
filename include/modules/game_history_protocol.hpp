#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "foxie_embedded/fixed_string.hpp"
#include <litch_max/fen_constants.hpp>

namespace modules::game_history {

inline constexpr uint8_t PROTOCOL_VERSION = 1U;
inline constexpr uint8_t FRAME_MAGIC = 0x4CU;
inline constexpr size_t FRAME_HEADER_BYTES = 8U;
inline constexpr size_t MIN_ATT_PAYLOAD_BYTES = 20U;
inline constexpr size_t MIN_FRAME_PAYLOAD_BYTES =
    MIN_ATT_PAYLOAD_BYTES - FRAME_HEADER_BYTES;
inline constexpr size_t MAX_FRAME_BYTES = 512U;
inline constexpr size_t MAX_FRAME_PAYLOAD_BYTES =
    MAX_FRAME_BYTES - FRAME_HEADER_BYTES;

inline constexpr size_t GAME_ID_BYTES = 16U;
inline constexpr size_t POSITION_BYTES = 43U;
inline constexpr size_t RECORD_HEADER_BYTES = 7U;
inline constexpr size_t RECORD_CRC_BYTES = 4U;
inline constexpr size_t MAX_TRANSITION_BYTES = 9U;
inline constexpr size_t MAX_RECORD_BYTES = 89U;
inline constexpr size_t MAX_SEGMENT_BYTES = 2048U;
inline constexpr uint8_t MAX_SEGMENT_TRANSITIONS = 32U;
inline constexpr size_t CONTROL_PAYLOAD_BYTES = 12U;
inline constexpr uint8_t MAX_RESTORE_TRANSITIONS = 128U;
inline constexpr uint8_t MAX_RESTORE_RECORDS =
    MAX_RESTORE_TRANSITIONS + 1U;
inline constexpr uint16_t LINE_BASE_RECORD_BYTES = 81U;
inline constexpr uint16_t MIN_COMMITTED_PLY_RECORD_BYTES = 83U;
inline constexpr uint16_t MAX_COMMITTED_PLY_RECORD_BYTES = 89U;
inline constexpr uint16_t SEAL_RECORD_BYTES = 38U;
inline constexpr uint16_t MAX_RESTORE_UPLOAD_BYTES =
    LINE_BASE_RECORD_BYTES +
    MAX_RESTORE_TRANSITIONS * MAX_COMMITTED_PLY_RECORD_BYTES;
inline constexpr uint16_t MAX_RESTORE_PLY = 4096U;
inline constexpr uint8_t INVALID_SQUARE = 0xFFU;
inline constexpr uint64_t MAX_LINE_REVISION = 0x7FFFFFFFFFFFFFFFULL;

enum class Variant : uint8_t {
    Classic = 1U,
    Duck = 2U,
};

enum class RecordType : uint8_t {
    LineBase = 1U,
    CommittedPly = 2U,
    Seal = 3U,
};

enum class ArchiveStatus : uint8_t {
    Completed = 1U,
    Unfinished = 2U,
};

enum class Outcome : uint8_t {
    Ongoing = 0U,
    Checkmate = 1U,
    Draw = 2U,
    KingCaptured = 3U,
    Fowled = 4U,
    Timeout = 5U,
    Resignation = 6U,
};

enum class Winner : uint8_t {
    None = 0U,
    White = 1U,
    Black = 2U,
    Draw = 3U,
};

enum class Side : uint8_t {
    White = 0U,
    Black = 1U,
};

enum class Promotion : uint8_t {
    None = 0U,
    Queen = 1U,
    Rook = 2U,
    Bishop = 3U,
    Knight = 4U,
};

enum TransitionFlag : uint8_t {
    MovingSideBlack = 1U << 0U,
    HasCapture = 1U << 1U,
    HasPromotion = 1U << 2U,
    HasSecondaryRelocation = 1U << 3U,
    HasDuckRelocation = 1U << 4U,
};

inline constexpr uint8_t SEGMENT_FLAG_ACKNOWLEDGED_OVERLAP = 1U << 0U;
inline constexpr uint8_t STATE_FLAG_JOURNAL_MATCHED = 1U << 0U;
inline constexpr uint8_t STATE_FLAG_CURSOR_RECONCILED = 1U << 1U;
inline constexpr uint8_t STATE_FLAG_SEGMENT_FOLLOWS = 1U << 2U;
inline constexpr uint8_t STATE_FLAG_NO_RECORDS = 1U << 3U;
inline constexpr uint8_t STATE_KNOWN_FLAGS = 0x0FU;

struct GameId {
    std::array<uint8_t, GAME_ID_BYTES> bytes{};

    [[nodiscard]] bool IsZero() const;
    [[nodiscard]] bool operator==(const GameId& other) const {
        return bytes == other.bytes;
    }
    [[nodiscard]] bool operator!=(const GameId& other) const {
        return !(*this == other);
    }
};

struct Position {
    std::array<uint8_t, POSITION_BYTES> bytes{};
};

struct Transition {
    Side side = Side::White;
    uint8_t from = INVALID_SQUARE;
    uint8_t to = INVALID_SQUARE;
    uint8_t captureSquare = INVALID_SQUARE;
    Promotion promotion = Promotion::None;
    uint8_t secondaryFrom = INVALID_SQUARE;
    uint8_t secondaryTo = INVALID_SQUARE;
    uint8_t duckFrom = INVALID_SQUARE;
    uint8_t duckTo = INVALID_SQUARE;
};

struct Record {
    RecordType type = RecordType::LineBase;
    uint32_t outboxSequence = 0U;
    GameId gameId{};
    uint64_t lineRevision = 0U;
    uint16_t ply = 0U;
    Variant variant = Variant::Classic;
    Position position{};
    Transition transition{};
    ArchiveStatus status = ArchiveStatus::Unfinished;
    Outcome outcome = Outcome::Ongoing;
    Winner winner = Winner::None;
};

static_assert(sizeof(Record) <= 96U,
              "history scanner retains at most one 96-byte record");

enum class Opcode : uint8_t {
    Begin = 1U,
    State = 2U,
    StateBounds = 3U,
    SegmentBegin = 4U,
    Data = 5U,
    Commit = 6U,
    Committed = 7U,
    RestoreBegin = 0x10U,
    RestoreReady = 0x11U,
    RestoreManifest = 0x12U,
    RestoreData = 0x13U,
    RestoreCommit = 0x14U,
    RestoreAccepted = 0x15U,
    CurrentBegin = 0x20U,
    CurrentState = 0x21U,
    CurrentBounds = 0x22U,
    CurrentSegmentBegin = 0x23U,
    CurrentData = 0x24U,
    CurrentSegmentCommit = 0x25U,
    CurrentSegmentCommitted = 0x26U,
    CurrentArchiveAck = 0x27U,
    CurrentArchived = 0x28U,
    Reject = 0x7FU,
};

enum class RejectReason : uint8_t {
    UnsupportedVersion = 1U,
    BadState = 2U,
    StaleTransaction = 3U,
    UnavailableWatermark = 4U,
    Sequence = 5U,
    Bounds = 6U,
    Crc = 7U,
    Storage = 8U,
    Busy = 9U,
    UnsupportedOpcode = 10U,
};

struct Frame {
    uint8_t opcode = static_cast<uint8_t>(Opcode::Begin);
    uint8_t flags = 0U;
    uint16_t transactionId = 0U;
    uint16_t frameSequence = 0U;
    uint16_t payloadLength = 0U;
    std::array<uint8_t, MAX_FRAME_PAYLOAD_BYTES> payload{};
};

struct SegmentDescriptor {
    uint16_t encodedBytes = 0U;
    uint8_t recordCount = 0U;
    uint8_t flags = 0U;
    uint32_t firstOutboxSequence = 0U;
    uint32_t crc32 = 0U;

    [[nodiscard]] uint32_t LastOutboxSequence() const;
};

struct BeginPayload {
    uint64_t journalId = 0U;
    uint32_t appDurableAck = 0U;
};

struct StatePayload {
    uint64_t journalId = 0U;
    uint32_t oldestRetained = 0U;
};

struct StateBoundsPayload {
    uint32_t newestAllocated = 0U;
    uint32_t boardDurableAck = 0U;
    uint32_t lostGameCount = 0U;
};

struct CommitPayload {
    uint32_t firstOutboxSequence = 0U;
    uint32_t lastOutboxSequence = 0U;
    uint32_t crc32 = 0U;
};

struct RejectPayload {
    RejectReason reason = RejectReason::BadState;
    uint8_t rejectedOpcode = 0U;
    uint16_t expectedFrameSequence = 0U;
    uint32_t detail = 0U;
};

struct RestoreCursorPayload {
    uint64_t journalId = 0U;
    uint32_t durableAck = 0U;
};

struct RestoreManifestPayload {
    uint16_t encodedBytes = 0U;
    uint8_t recordCount = 0U;
    Variant variant = Variant::Classic;
    uint16_t basePly = 0U;
    uint16_t currentPly = 0U;
    uint32_t crc32 = 0U;
};

struct RestoreAcceptedPayload {
    uint16_t currentPly = 0U;
    uint8_t transitionCount = 0U;
    Variant variant = Variant::Classic;
    uint16_t encodedBytes = 0U;
    uint32_t crc32 = 0U;
};

class Crc32Accumulator {
 public:
    void Reset();
    bool Update(const uint8_t* bytes, size_t length);
    [[nodiscard]] uint32_t Value() const;

 private:
    uint32_t m_state = 0xFFFFFFFFU;
};

bool IsKnownOpcode(uint8_t opcode);
bool IsValidGameId(const GameId& gameId);
bool IsValidVariant(Variant variant);
bool IsValidSeal(ArchiveStatus status, Outcome outcome, Winner winner);

bool EncodePosition(const char* fen,
                    Variant variant,
                    uint8_t duckSquare,
                    Position& out);
bool DecodePosition(const Position& position,
                    Variant variant,
                    fxe::FixedString<litch_max::FEN_BUFFER_CAPACITY>& outFen,
                    uint8_t& outDuckSquare);

// Compare placement, turn, rights, en-passant and Duck square. Engine-normalized
// counters are excluded only when an existing revision proves continuation.
bool SamePositionAuthority(const Position& left, const Position& right);
// Preserve archive counter continuity from a validated prior position/move.
// before and after may alias; call before replacing the mover's source square.
bool AdvancePositionCounters(const Position& before, const Transition& move,
                             Position& after);

size_t EncodedTransitionSize(const Transition& transition);
bool EncodeTransition(const Transition& transition,
                      uint8_t* out,
                      size_t outCapacity,
                      size_t& outLength);
bool DecodeTransition(const uint8_t* bytes,
                      size_t length,
                      Transition& out);

bool EncodeRecord(const Record& record,
                  uint8_t* out,
                  size_t outCapacity,
                  size_t& outLength);
bool DecodeRecord(const uint8_t* bytes,
                  size_t length,
                  Record& out);

bool EncodeFrame(const Frame& frame,
                 uint8_t* out,
                 size_t outCapacity,
                 size_t& outLength);
bool DecodeFrame(const uint8_t* bytes,
                 size_t length,
                 Frame& out);

bool EncodeBeginPayload(const BeginPayload& payload, uint8_t out[12]);
bool DecodeBeginPayload(const uint8_t* bytes, size_t length,
                        BeginPayload& out);
bool EncodeStatePayload(const StatePayload& payload, uint8_t out[12]);
bool DecodeStatePayload(const uint8_t* bytes, size_t length,
                        StatePayload& out);
bool EncodeStateBoundsPayload(const StateBoundsPayload& payload,
                              uint8_t out[12]);
bool DecodeStateBoundsPayload(const uint8_t* bytes, size_t length,
                              StateBoundsPayload& out);
bool EncodeSegmentDescriptor(const SegmentDescriptor& descriptor,
                             uint8_t out[12]);
bool DecodeSegmentDescriptor(const uint8_t* bytes,
                             size_t length,
                             SegmentDescriptor& out);
bool EncodeCommitPayload(const CommitPayload& payload, uint8_t out[12]);
bool DecodeCommitPayload(const uint8_t* bytes, size_t length,
                         CommitPayload& out);
bool EncodeRejectPayload(const RejectPayload& payload, uint8_t out[8]);
bool DecodeRejectPayload(const uint8_t* bytes, size_t length,
                         RejectPayload& out);
bool IsValidRestoreCursor(const RestoreCursorPayload& payload);
bool IsValidRestoreManifest(const RestoreManifestPayload& payload);
bool IsValidRestoreAccepted(const RestoreAcceptedPayload& payload);
bool EncodeRestoreCursorPayload(const RestoreCursorPayload& payload,
                                uint8_t out[CONTROL_PAYLOAD_BYTES]);
bool DecodeRestoreCursorPayload(const uint8_t* bytes, size_t length,
                                RestoreCursorPayload& out);
bool EncodeRestoreManifestPayload(const RestoreManifestPayload& payload,
                                  uint8_t out[CONTROL_PAYLOAD_BYTES]);
bool DecodeRestoreManifestPayload(const uint8_t* bytes, size_t length,
                                  RestoreManifestPayload& out);
bool EncodeRestoreAcceptedPayload(const RestoreAcceptedPayload& payload,
                                  uint8_t out[CONTROL_PAYLOAD_BYTES]);
bool DecodeRestoreAcceptedPayload(const uint8_t* bytes, size_t length,
                                  RestoreAcceptedPayload& out);

} // namespace modules::game_history
