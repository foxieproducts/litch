#include <array>
#include <cstdio>
#include <cstring>

#include "modules/current_game_log.hpp"
#include "modules/current_game_sync_protocol.hpp"
#include "modules/game_history_protocol.hpp"

namespace {
using namespace modules::current_game_sync;
int failures = 0;
void Check(bool value, const char* message) {
    if (!value) {
        ++failures;
        std::printf("[FAIL] %s\n", message);
    }
}
bool MatchesFixture(const char* key, const uint8_t* bytes, size_t length) {
    FILE* file = std::fopen("test/fixtures/current_game_sync_v1.txt", "r");
    if (file == nullptr) return false;
    char line[160U] = {};
    const size_t keyLength = std::strlen(key);
    bool match = false;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, key, keyLength) != 0 ||
            line[keyLength] != '=') continue;
        const char* cursor = line + keyLength + 1U;
        size_t index = 0U;
        match = true;
        while (cursor[0U] != '\0' && cursor[0U] != '\n' &&
               cursor[0U] != '\r') {
            const auto nibble = [](const char value) {
                if (value >= '0' && value <= '9') return value - '0';
                if (value >= 'a' && value <= 'f') return value - 'a' + 10;
                return -1;
            };
            const int high = nibble(cursor[0U]);
            const int low = nibble(cursor[1U]);
            if (high < 0 || low < 0 || index >= length ||
                bytes[index] != uint8_t((high << 4U) | low)) {
                match = false;
                break;
            }
            ++index;
            cursor += 2U;
        }
        match = match && index == length;
        break;
    }
    std::fclose(file);
    return match;
}
}

int main() {
    std::array<uint8_t, 12U> bytes{};
    ResumeCursor cursor{2U, {0x66U, 0x03U, 0x73U, 0xE8U,
                            0x1AU, 0x8CU, 0x0EU, 0xCAU}};
    ResumeCursor decodedCursor;
    Check(EncodeResumeCursor(cursor, bytes.data()) &&
              DecodeResumeCursor(bytes.data(), bytes.size(), decodedCursor) &&
              decodedCursor.nextRecordSequence == cursor.nextRecordSequence &&
              decodedCursor.prefixFingerprint == cursor.prefixFingerprint,
          "resume cursor round-trips");
    Check(MatchesFixture("resume_seq_2_base_sha256_64",
                         bytes.data(), bytes.size()),
          "resume cursor matches the shared little-endian fixture");
    Check(!IsValidResumeCursor({1U, {}}) &&
              !IsValidResumeCursor({4100U, {1U}}),
          "resume cursor rejects contradictory absence and overflow");
    Check(!DecodeResumeCursor(bytes.data(), 11U, decodedCursor),
          "resume cursor rejects a short control payload");
    const ResumeCursor emptyPrefix{1U, {0xE3U, 0xB0U, 0xC4U, 0x42U,
                                        0x98U, 0xFCU, 0x1CU, 0x14U}};
    Check(EncodeResumeCursor(emptyPrefix, bytes.data()) &&
              MatchesFixture("resume_seq_1_empty_sha256_64",
                             bytes.data(), bytes.size()),
          "record 1 uses the canonical SHA-256 empty-prefix fingerprint");

    Digest digest{{0xC6U, 0xEDU, 0xE8U, 0xE1U, 0x14U, 0x70U,
                   0x33U, 0x54U, 0x8AU, 0xB6U, 0x39U, 0x58U}};
    Digest decodedDigest;
    Check(EncodeDigest(digest, false, bytes.data()) &&
              DecodeDigest(bytes.data(), bytes.size(), false,
                           decodedDigest) &&
              decodedDigest.fingerprint == digest.fingerprint &&
              MatchesFixture("digest_base_move_sha256_96",
                             bytes.data(), bytes.size()),
          "exact digest matches the shared fixture");
    Check(!IsValidDigest({}, false) && IsValidDigest({}, true),
          "only State may encode one all-zero absent digest");
    Check(!DecodeDigest(bytes.data(), 13U, false, decodedDigest),
          "digest rejects an oversized control payload");

    Bounds bounds{0x0102030405060708ULL,
                  modules::current_game_log::MAX_LINE_RECORDS,
                  uint8_t(CURRENT_LINE_PRESENT | ARCHIVE_REQUIRED |
                          LINE_SEALED)};
    Bounds decodedBounds;
    Check(EncodeBounds(bounds, bytes.data()) &&
              DecodeBounds(bytes.data(), bytes.size(), decodedBounds) &&
              decodedBounds.lineRevision == bounds.lineRevision &&
              decodedBounds.recordCount ==
                  modules::current_game_log::MAX_LINE_RECORDS &&
              MatchesFixture(
                  "bounds_revision_72623859790382856_records_4098",
                  bytes.data(), bytes.size()),
          "current-line bounds match the shared fixture");
    Check(IsValidBounds({}) &&
              !IsValidBounds({1U, 0U, ARCHIVE_REQUIRED}) &&
              !IsValidBounds({1U, 1U,
                              uint8_t(CURRENT_LINE_PRESENT | LINE_SEALED)}) &&
              IsValidBounds({1U,
                             modules::current_game_log::MAX_LINE_RECORDS - 1U,
                             CURRENT_LINE_PRESENT}) &&
              !IsValidBounds({1U,
                              modules::current_game_log::MAX_LINE_RECORDS,
                              CURRENT_LINE_PRESENT}),
          "bounds reject flags without coherent presence or record count");
    bytes.fill(0U);
    bytes[11U] = 1U;
    Check(!DecodeBounds(bytes.data(), bytes.size(), decodedBounds),
          "bounds reject a nonzero reserved byte");
    Check(IsKnownOpcode(0x20U) && IsKnownOpcode(0x28U) &&
              !IsKnownOpcode(0x29U),
          "current-line opcode range is exact");

    modules::game_history::Frame frame;
    frame.opcode = static_cast<uint8_t>(Opcode::Begin);
    frame.transactionId = 0x1234U;
    frame.payloadLength = bytes.size();
    Check(EncodeResumeCursor(cursor, frame.payload.data()),
          "resume cursor populates frame payload");
    std::array<uint8_t, modules::game_history::MAX_FRAME_BYTES> frameBytes{};
    size_t frameLength = 0U;
    Check(modules::game_history::EncodeFrame(
              frame, frameBytes.data(), frameBytes.size(), frameLength) &&
              frameLength == modules::game_history::MIN_ATT_PAYLOAD_BYTES &&
              MatchesFixture("frame_begin_tx_4660_seq_2",
                             frameBytes.data(), frameLength),
          "CurrentBegin matches the shared minimum-ATT frame fixture");

    frame.opcode = static_cast<uint8_t>(Opcode::State);
    Check(EncodeDigest(digest, false, frame.payload.data()) &&
              modules::game_history::EncodeFrame(
                  frame, frameBytes.data(), frameBytes.size(), frameLength) &&
              frameLength == modules::game_history::MIN_ATT_PAYLOAD_BYTES &&
              MatchesFixture("frame_state_tx_4660_base_move",
                             frameBytes.data(), frameLength),
          "CurrentState matches the shared minimum-ATT frame fixture");
    if (failures == 0) std::puts("[CURRENT_GAME_SYNC_PROTOCOL] ok");
    return failures == 0 ? 0 : 1;
}
