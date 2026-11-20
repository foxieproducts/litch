#include <array>
#include <cstdio>
#include <cstring>

#include "foxie_embedded/checksum.hpp"
#include "modules/game_history_protocol.hpp"

namespace {

using namespace modules::game_history;

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

int HexNibble(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool LoadFixtureHex(const char* const key,
                    uint8_t* const out,
                    const size_t capacity,
                    size_t& outLength) {
    outLength = 0U;
    FILE* file = std::fopen("test/fixtures/game_history_v1.txt", "r");
    if (file == nullptr) {
        return false;
    }
    char line[1024] = {};
    const size_t keyLength = std::strlen(key);
    bool found = false;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, key, keyLength) != 0 ||
            line[keyLength] != '=') {
            continue;
        }
        const char* cursor = line + keyLength + 1U;
        while (cursor[0] != '\0' && cursor[0] != '\n' &&
               cursor[0] != '\r') {
            const int high = HexNibble(cursor[0]);
            const int low = HexNibble(cursor[1]);
            if (high < 0 || low < 0 || outLength >= capacity) {
                std::fclose(file);
                return false;
            }
            out[outLength++] = static_cast<uint8_t>((high << 4) | low);
            cursor += 2;
        }
        found = true;
        break;
    }
    std::fclose(file);
    return found;
}

bool MatchesFixture(const char* const key,
                    const uint8_t* const bytes,
                    const size_t length) {
    std::array<uint8_t, MAX_SEGMENT_BYTES> expected{};
    size_t expectedLength = 0U;
    return LoadFixtureHex(key, expected.data(), expected.size(),
                          expectedLength) &&
           expectedLength == length &&
           std::memcmp(expected.data(), bytes, length) == 0;
}

GameId ExampleId() {
    GameId id;
    for (uint8_t index = 0U; index < id.bytes.size(); ++index) {
        id.bytes[index] = index;
    }
    return id;
}

Position EncodeExamplePosition(const char* const fen,
                               const Variant variant,
                               const uint8_t duckSquare = INVALID_SQUARE) {
    Position position;
    Check(EncodePosition(fen, variant, duckSquare, position),
          "example position encodes");
    return position;
}

void TestPositionRoundTrip() {
    constexpr char INITIAL[] =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/"
        "RNBQKBNR w KQkq - 0 1";
    const Position classic = EncodeExamplePosition(INITIAL, Variant::Classic);
    Check(MatchesFixture("position_initial_classic", classic.bytes.data(),
                         classic.bytes.size()),
          "Classic initial position matches the shared fixture");

    fxe::FixedString<litch_max::FEN_BUFFER_CAPACITY> decoded;
    uint8_t duck = 0U;
    Check(DecodePosition(classic, Variant::Classic, decoded, duck),
          "Classic position decodes");
    Check(decoded.Equals(INITIAL) && duck == INVALID_SQUARE,
          "Classic FEN round-trips exactly");

    constexpr char LARGE_COUNTER_FEN[] =
        "8/8/8/8/8/8/8/8 w - - 4294967295 4294967295";
    const Position largeCounters = EncodeExamplePosition(
        LARGE_COUNTER_FEN, Variant::Classic);
    Check(DecodePosition(largeCounters, Variant::Classic, decoded, duck) &&
              decoded.Equals(LARGE_COUNTER_FEN),
          "32-bit FEN counters round-trip without narrowing");

    constexpr char DUCK_FEN[] =
        "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/"
        "RNBQKBNR b KQkq e3 0 1";
    const Position duckPosition = EncodeExamplePosition(
        DUCK_FEN, Variant::Duck, 27U);
    Check(DecodePosition(duckPosition, Variant::Duck, decoded, duck),
          "Duck position decodes");
    Check(decoded.Equals(DUCK_FEN) && duck == 27U,
          "Duck FEN and square round-trip exactly");

    Position invalid = classic;
    invalid.bytes[0] = 0xF1U;
    Check(!DecodePosition(invalid, Variant::Classic, decoded, duck),
          "reserved piece code is rejected");
    Check(!EncodePosition(INITIAL, Variant::Duck, 0U, invalid),
          "Duck cannot share a piece square");
    Check(!EncodePosition(
              "8/8/8/8/8/8/8/8/ w - - 0 1",
              Variant::Classic, INVALID_SQUARE, invalid),
          "extra rank separator is rejected");

    invalid = classic;
    invalid.bytes[38] = 0U;
    invalid.bytes[39] = 0U;
    invalid.bytes[40] = 0U;
    invalid.bytes[41] = 0U;
    Check(!DecodePosition(invalid, Variant::Classic, decoded, duck),
          "zero fullmove number is rejected");
}

void RefreshRecordCrc(uint8_t* bytes, const size_t length) {
    const uint32_t crc = fxe::Crc32(bytes, length - RECORD_CRC_BYTES);
    for (size_t index = 0; index < RECORD_CRC_BYTES; ++index) {
        bytes[length - RECORD_CRC_BYTES + index] =
            static_cast<uint8_t>(crc >> (8U * index));
    }
}

void TestMalformedCanonicalInputs() {
    Position position = EncodeExamplePosition(
        "8/8/8/8/8/8/8/K6k w - - 0 1", Variant::Classic);
    position.bytes[33] = 36U;
    fxe::FixedString<litch_max::FEN_BUFFER_CAPACITY> fen;
    uint8_t duck = INVALID_SQUARE;
    Check(!DecodePosition(position, Variant::Classic, fen, duck),
          "packed en-passant must be on rank 3 or 6");
    Check(!EncodePosition("8/8/8/8/8/8/8/K6k w  - 0 1",
                          Variant::Classic, INVALID_SQUARE, position),
          "empty castling token is rejected");
    Check(!EncodePosition("8/8/8/8/8/8/8/K6k w - ",
                          Variant::Classic, INVALID_SQUARE, position),
          "truncated en-passant token is safely rejected");
    std::array<char, litch_max::FEN_BUFFER_CAPACITY + 64U> oversized{};
    constexpr char PREFIX[] = "8/8/8/8/8/8/8/K6k w - - ";
    std::memcpy(oversized.data(), PREFIX, sizeof(PREFIX) - 1U);
    std::memset(oversized.data() + sizeof(PREFIX) - 1U, '0',
                oversized.size() - sizeof(PREFIX) - 3U);
    std::memcpy(oversized.data() + oversized.size() - 4U, "0 1", 4U);
    Check(!EncodePosition(oversized.data(), Variant::Classic,
                          INVALID_SQUARE, position),
          "FEN parsing is bounded by the canonical capacity");

    std::array<uint8_t, MAX_RECORD_BYTES> bytes{};
    size_t length = 0;
    Record decoded;
    Check(LoadFixtureHex("record_line_base_seq_1", bytes.data(),
                         bytes.size(), length), "load malformed base seed");
    bytes[7U + 27U + 33U] = 36U;
    RefreshRecordCrc(bytes.data(), length);
    Check(!DecodeRecord(bytes.data(), length, decoded),
          "CRC-valid record rejects invalid en-passant rank");

    Record seal;
    seal.type = RecordType::Seal;
    seal.outboxSequence = 3U;
    seal.gameId = ExampleId();
    seal.lineRevision = 2U;
    seal.status = ArchiveStatus::Completed;
    seal.outcome = Outcome::Checkmate;
    seal.winner = Winner::White;
    Check(EncodeRecord(seal, bytes.data(), bytes.size(), length),
          "encode seal mutation seed");
    bytes[7U + 25U] = 255U;
    RefreshRecordCrc(bytes.data(), length);
    Check(!DecodeRecord(bytes.data(), length, decoded),
          "CRC-valid seal rejects unknown outcome");
    Check(!IsValidSeal(ArchiveStatus::Completed,
                       static_cast<Outcome>(255U), Winner::White),
          "seal encoder admission rejects unknown outcome");

    for (const uint8_t flag : {HasCapture, HasPromotion,
                               HasSecondaryRelocation, HasDuckRelocation}) {
        std::array<uint8_t, 5U> transition = {flag, 52U, 36U, 255U, 255U};
        const size_t transitionLength =
            flag == HasCapture || flag == HasPromotion ? 4U : 5U;
        if (flag == HasPromotion) transition[3] = 0U;
        Transition parsed;
        Check(!DecodeTransition(transition.data(), transitionLength, parsed),
              "present transition flags reject absent sentinel values");
        Check(LoadFixtureHex("record_committed_ply_seq_2", bytes.data(),
                             bytes.size(), length), "load malformed ply seed");
        std::memcpy(bytes.data() + 76U, transition.data(), transitionLength);
        length = 76U + transitionLength + RECORD_CRC_BYTES;
        const size_t payloadLength = length - RECORD_HEADER_BYTES - RECORD_CRC_BYTES;
        bytes[2] = static_cast<uint8_t>(payloadLength);
        RefreshRecordCrc(bytes.data(), length);
        Check(!DecodeRecord(bytes.data(), length, decoded),
              "CRC-valid ply rejects noncanonical optional fields");
    }
}

void TestGoldenRecords() {
    constexpr char INITIAL[] =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/"
        "RNBQKBNR w KQkq - 0 1";
    constexpr char AFTER_E4[] =
        "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/"
        "RNBQKBNR b KQkq e3 0 1";

    Record lineBase;
    lineBase.type = RecordType::LineBase;
    lineBase.outboxSequence = 1U;
    lineBase.gameId = ExampleId();
    lineBase.lineRevision = 1U;
    lineBase.variant = Variant::Classic;
    lineBase.position = EncodeExamplePosition(
        INITIAL, Variant::Classic);

    std::array<uint8_t, MAX_RECORD_BYTES> baseBytes{};
    size_t baseLength = 0U;
    Check(EncodeRecord(lineBase, baseBytes.data(), baseBytes.size(),
                       baseLength) &&
              baseLength == 81U,
          "line-base record is exactly 81 bytes");
    Check(MatchesFixture("record_line_base_seq_1", baseBytes.data(),
                         baseLength),
          "line-base record matches the shared fixture");
    Record decoded;
    Check(DecodeRecord(baseBytes.data(), baseLength, decoded) &&
              decoded.type == RecordType::LineBase &&
              decoded.outboxSequence == 1U &&
              decoded.gameId == lineBase.gameId &&
              decoded.lineRevision == 1U,
          "line-base record round-trips");

    Record ply;
    ply.type = RecordType::CommittedPly;
    ply.outboxSequence = 2U;
    ply.gameId = ExampleId();
    ply.lineRevision = 2U;
    ply.ply = 1U;
    ply.position = EncodeExamplePosition(
        AFTER_E4, Variant::Classic);
    ply.transition.side = Side::White;
    ply.transition.from = 52U;
    ply.transition.to = 36U;

    std::array<uint8_t, MAX_RECORD_BYTES> plyBytes{};
    size_t plyLength = 0U;
    Check(EncodeRecord(ply, plyBytes.data(), plyBytes.size(), plyLength) &&
              plyLength == 83U,
          "minimal committed-ply record is exactly 83 bytes");
    Check(MatchesFixture("record_committed_ply_seq_2", plyBytes.data(),
                         plyLength),
          "committed-ply record matches the shared fixture");
    Check(DecodeRecord(plyBytes.data(), plyLength, decoded) &&
              decoded.lineRevision == 2U &&
              decoded.transition.from == 52U &&
              decoded.transition.to == 36U,
          "committed-ply record round-trips");

    plyBytes[12U] ^= 0x40U;
    Check(!DecodeRecord(plyBytes.data(), plyLength, decoded),
          "record CRC rejects mutation");
    Check(!DecodeRecord(baseBytes.data(), baseLength - 1U, decoded),
          "truncated record is rejected");

    Record maximum = ply;
    maximum.lineRevision = 0x0102030405060708ULL;
    maximum.transition.side = Side::Black;
    maximum.transition.captureSquare = 44U;
    maximum.transition.promotion = Promotion::Queen;
    maximum.transition.secondaryFrom = 63U;
    maximum.transition.secondaryTo = 61U;
    maximum.transition.duckFrom = 27U;
    maximum.transition.duckTo = 28U;
    Check(EncodeRecord(maximum, plyBytes.data(), plyBytes.size(), plyLength) &&
              plyLength == MAX_RECORD_BYTES &&
              plyBytes[76U] == 0x1FU &&
              DecodeRecord(plyBytes.data(), plyLength, decoded) &&
              decoded.lineRevision ==
                  0x0102030405060708ULL,
          "maximum record and 64-bit revision round-trip in 89 bytes");
    maximum.lineRevision = MAX_LINE_REVISION + 1U;
    Check(!EncodeRecord(maximum, plyBytes.data(), plyBytes.size(), plyLength),
          "firmware rejects revisions outside the app-safe range");
    maximum.lineRevision = 2U;
    maximum.position.bytes[0U] = 0xF1U;
    Check(!EncodeRecord(maximum, plyBytes.data(), plyBytes.size(), plyLength),
          "record encoder rejects an invalid packed position");

    Record seal;
    seal.type = RecordType::Seal;
    seal.outboxSequence = 3U;
    seal.gameId = ExampleId();
    seal.lineRevision = 2U;
    seal.status = ArchiveStatus::Completed;
    seal.outcome = Outcome::Checkmate;
    seal.winner = Winner::White;
    Check(EncodeRecord(seal, plyBytes.data(), plyBytes.size(), plyLength) &&
              plyLength == 38U &&
              DecodeRecord(plyBytes.data(), plyLength, decoded),
          "seal record is exactly 38 bytes and round-trips");

    seal.status = ArchiveStatus::Unfinished;
    Check(!EncodeRecord(seal, plyBytes.data(), plyBytes.size(), plyLength),
          "unfinished seal cannot claim a terminal result");
}

void TestMinimumMtuFrames() {
    BeginPayload beginPayload;
    beginPayload.journalId = 0x0102030405060708ULL;
    uint8_t payload[12] = {};
    Check(EncodeBeginPayload(beginPayload, payload),
          "begin payload encodes in twelve bytes");

    Frame begin;
    begin.opcode = static_cast<uint8_t>(Opcode::Begin);
    begin.transactionId = 0x1234U;
    begin.payloadLength = sizeof(payload);
    std::memcpy(begin.payload.data(), payload, sizeof(payload));
    std::array<uint8_t, MAX_FRAME_BYTES> bytes{};
    size_t length = 0U;
    Check(EncodeFrame(begin, bytes.data(), bytes.size(), length) &&
              length == MIN_ATT_PAYLOAD_BYTES &&
              MatchesFixture("frame_begin_tx_4660", bytes.data(), length),
          "begin frame matches the shared minimum-MTU fixture");

    std::array<uint8_t, MAX_RECORD_BYTES> baseBytes{};
    std::array<uint8_t, MAX_RECORD_BYTES> plyBytes{};
    size_t baseLength = 0U;
    size_t plyLength = 0U;
    Check(LoadFixtureHex("record_line_base_seq_1", baseBytes.data(),
                         baseBytes.size(), baseLength) &&
              LoadFixtureHex("record_committed_ply_seq_2", plyBytes.data(),
                             plyBytes.size(), plyLength),
          "shared record fixtures load");
    Crc32Accumulator crc;
    Check(crc.Update(baseBytes.data(), 17U) &&
              crc.Update(baseBytes.data() + 17U, baseLength - 17U) &&
              crc.Update(plyBytes.data(), plyLength) &&
              crc.Value() == 0xCA681BC0U,
          "incremental segment CRC is stable across frame boundaries");

    SegmentDescriptor descriptor;
    descriptor.encodedBytes = static_cast<uint16_t>(baseLength + plyLength);
    descriptor.recordCount = 2U;
    descriptor.firstOutboxSequence = 1U;
    descriptor.crc32 = crc.Value();
    Check(EncodeSegmentDescriptor(descriptor, payload),
          "segment descriptor fills one minimum-MTU payload");
    Frame segmentBegin;
    segmentBegin.opcode = static_cast<uint8_t>(Opcode::SegmentBegin);
    segmentBegin.transactionId = 0x1234U;
    segmentBegin.frameSequence = 3U;
    segmentBegin.payloadLength = sizeof(payload);
    std::memcpy(segmentBegin.payload.data(), payload, sizeof(payload));
    Check(EncodeFrame(segmentBegin, bytes.data(), bytes.size(), length) &&
              MatchesFixture("frame_segment_begin_tx_4660", bytes.data(),
                             length),
          "segment-begin frame matches the shared fixture");

    Frame decoded;
    Check(DecodeFrame(bytes.data(), length, decoded) &&
              decoded.transactionId == segmentBegin.transactionId &&
              decoded.payloadLength == MIN_FRAME_PAYLOAD_BYTES,
          "minimum-ATT frame round-trips");
    bytes[2] = 0x40U;
    Check(DecodeFrame(bytes.data(), length, decoded) &&
              !IsKnownOpcode(decoded.opcode),
          "unknown opcode remains parseable for typed rejection");
    bytes[4] = 0U;
    bytes[5] = 0U;
    Check(!DecodeFrame(bytes.data(), length, decoded),
          "zero transaction identity is rejected");

    StateBoundsPayload bounds;
    bounds.newestAllocated = 12U;
    bounds.boardDurableAck = 13U;
    Check(!EncodeStateBoundsPayload(bounds, payload),
          "board acknowledgement cannot exceed allocated sequence");

    CommitPayload commit;
    commit.firstOutboxSequence = 5U;
    commit.lastOutboxSequence = 4U;
    Check(!EncodeCommitPayload(commit, payload),
          "reversed commit range is rejected");

    RejectPayload rejection;
    rejection.reason = RejectReason::Crc;
    rejection.rejectedOpcode = static_cast<uint8_t>(Opcode::Commit);
    rejection.expectedFrameSequence = 9U;
    rejection.detail = 0x01020304U;
    uint8_t rejectionBytes[8] = {};
    RejectPayload decodedRejection;
    Check(EncodeRejectPayload(rejection, rejectionBytes) &&
              DecodeRejectPayload(rejectionBytes, sizeof(rejectionBytes),
                                  decodedRejection) &&
              decodedRejection.reason == RejectReason::Crc &&
              decodedRejection.expectedFrameSequence == 9U &&
              decodedRejection.detail == 0x01020304U,
          "typed rejection payload round-trips");

    constexpr size_t MINIMAL_TWO_EDGE_BYTES = 81U + 38U;
    constexpr size_t MINIMAL_COMMITTED_PLY_BYTES = 83U;
    constexpr size_t MAX_MINIMAL_PLIES_WITH_BOTH_EDGES =
        (MAX_SEGMENT_BYTES - MINIMAL_TWO_EDGE_BYTES) /
        MINIMAL_COMMITTED_PLY_BYTES;
    static_assert(MAX_MINIMAL_PLIES_WITH_BOTH_EDGES == 23U,
                  "segment byte cap, not transition cap, is decisive");
}

void TestRestoreControlPayloads() {
    uint8_t bytes[CONTROL_PAYLOAD_BYTES] = {};
    RestoreCursorPayload cursor{0x0102030405060708ULL, 0x0A0B0C0DU};
    RestoreCursorPayload decodedCursor;
    Check(EncodeRestoreCursorPayload(cursor, bytes) &&
              MatchesFixture("restore_cursor", bytes, sizeof(bytes)) &&
              DecodeRestoreCursorPayload(bytes, sizeof(bytes),
                                         decodedCursor) &&
              decodedCursor.journalId == cursor.journalId &&
              decodedCursor.durableAck == cursor.durableAck,
          "restore cursor matches the shared fixture");
    cursor.durableAck = 0U;
    Check(!EncodeRestoreCursorPayload(cursor, bytes),
          "restore cursor requires a durable acknowledgement");

    RestoreManifestPayload manifest{MAX_RESTORE_UPLOAD_BYTES,
                                    MAX_RESTORE_RECORDS,
                                    Variant::Duck,
                                    3968U,
                                    4096U,
                                    0x12345678U};
    RestoreManifestPayload decodedManifest;
    Check(EncodeRestoreManifestPayload(manifest, bytes) &&
              MatchesFixture("restore_manifest_max", bytes, sizeof(bytes)) &&
              DecodeRestoreManifestPayload(bytes, sizeof(bytes),
                                           decodedManifest) &&
              decodedManifest.currentPly == 4096U,
          "maximum restore manifest matches the shared fixture");
    manifest.recordCount = 128U;
    Check(!EncodeRestoreManifestPayload(manifest, bytes),
          "restore manifest rejects count and ply disagreement");

    RestoreAcceptedPayload accepted{4096U, MAX_RESTORE_TRANSITIONS,
                                    Variant::Duck,
                                    MAX_RESTORE_UPLOAD_BYTES,
                                    0x12345678U};
    RestoreAcceptedPayload decodedAccepted;
    Check(EncodeRestoreAcceptedPayload(accepted, bytes) &&
              MatchesFixture("restore_accepted_max", bytes, sizeof(bytes)) &&
              DecodeRestoreAcceptedPayload(bytes, sizeof(bytes),
                                           decodedAccepted) &&
              decodedAccepted.transitionCount == MAX_RESTORE_TRANSITIONS,
          "maximum restore acceptance matches the shared fixture");
    bytes[6U] = 1U;
    Check(!DecodeRestoreAcceptedPayload(bytes, sizeof(bytes),
                                        decodedAccepted),
          "restore acceptance rejects nonzero reserved bytes");

    Check(IsKnownOpcode(static_cast<uint8_t>(Opcode::RestoreBegin)) &&
              IsKnownOpcode(static_cast<uint8_t>(Opcode::RestoreAccepted)),
          "canonical opcode table includes restore transactions");
}

} // namespace

int main() {
    std::puts("[GAME_HISTORY_PROTOCOL] start");
    TestPositionRoundTrip();
    TestMalformedCanonicalInputs();
    TestGoldenRecords();
    TestMinimumMtuFrames();
    TestRestoreControlPayloads();

    if (g_failures == 0) {
        std::puts("[GAME_HISTORY_PROTOCOL] ok");
        return 0;
    }
    std::printf("[GAME_HISTORY_PROTOCOL] failures=%d\n", g_failures);
    return 1;
}
