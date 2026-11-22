#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "modules/ble_owner_pairing_protocol.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

} // namespace

int main() {
    using namespace modules::ble_owner_pairing;

    Check(std::strcmp(CHARACTERISTIC_UUID,
                      "6f2a20d1-89df-4ac6-a9f2-53f6f7db0107") == 0,
          "owner pairing reserves an endpoint distinct from Litchcraft");

    const std::array<uint8_t, 13> end0 = {
        1, 0x82, 0x34, 0x12, 0, 1, 4, 0, 0x20, 0, 0, 2, 0x80,
    };
    const std::array<uint8_t, 13> end1 = {
        1, 0x82, 0x34, 0x12, 1, 0x80, 2, 0, 0, 0x20, 0, 4, 1,
    };
    std::ifstream fixture("test/fixtures/owner_pairing_v1.txt");
    std::string end0Line;
    std::string end1Line;
    std::getline(fixture, end0Line);
    std::getline(fixture, end1Line);
    Check(end0Line == "pattern_end0=01823412000104002000000280" &&
              end1Line == "pattern_end1=01823412018002000020000401",
          "canonical golden fixture is exact");

    Frame frame;
    Check(Decode(end0.data(), end0.size(), frame) &&
              frame.mask == 0x8002000020000401ULL,
          "end-0 high-bit golden decodes");
    std::array<uint8_t, 20> encoded = {};
    Check(Encode(frame, encoded.data(), encoded.size()) == end0.size() &&
              std::memcmp(encoded.data(), end0.data(), end0.size()) == 0,
          "end-0 pattern round-trips before frame reuse");

    Check(Decode(end1.data(), end1.size(), frame) && frame.viewEnd == 1U &&
              frame.mask == 0x0104002000000280ULL,
          "end-1 golden decodes");
    Check(Encode(frame, encoded.data(), encoded.size()) == end1.size() &&
              std::memcmp(encoded.data(), end1.data(), end1.size()) == 0,
          "end-1 pattern round-trips");

    const uint8_t statusRequest[] = {1, 1};
    const uint8_t beginPattern[] = {1, 2};
    const uint8_t patternMatches[] = {1, 3, 1, 0};
    const uint8_t cancel[] = {1, 4, 1, 0};
    Check(Decode(statusRequest, sizeof(statusRequest), frame) &&
              Decode(beginPattern, sizeof(beginPattern), frame),
          "both requests decode");
    Check(Decode(patternMatches, sizeof(patternMatches), frame) &&
              Decode(cancel, sizeof(cancel), frame),
          "match and cancel decode");

    for (uint8_t state = 0U; state <= 10U; ++state) {
        Frame stateFrame;
        stateFrame.opcode = Opcode::State;
        stateFrame.session = 1U;
        stateFrame.state = static_cast<PairingState>(state);
        Check(Encode(stateFrame, encoded.data(), encoded.size()) == 6U,
              "every state ordinal encodes");
    }
    for (uint8_t reason = 1U; reason <= 11U; ++reason) {
        Frame rejectFrame;
        rejectFrame.opcode = Opcode::Reject;
        rejectFrame.requestOpcode = 2U;
        rejectFrame.reason = static_cast<Reason>(reason);
        Check(Encode(rejectFrame, encoded.data(), encoded.size()) == 4U,
              "every reject reason ordinal encodes");
    }

    std::array<uint8_t, 20> status = {1, 0x81, 3, 0x0D};
    status[19] = 1U;
    Check(Decode(status.data(), status.size(), frame),
          "physical-ready flag is independent of state");
    status[3] = 0x10U;
    Check(!Decode(status.data(), status.size(), frame),
          "reserved status flags are rejected");

    const uint8_t badReject[] = {1, 0xFF, 5, 1};
    Check(!Decode(badReject, sizeof(badReject), frame),
          "unknown request opcode is rejected");
    const uint8_t wrongVersion[] = {2, 1};
    const uint8_t wrongOpcode[] = {1, 5};
    const uint8_t zeroSession[] = {1, 3, 0, 0};
    const uint8_t invalidReason[] = {1, 0xFF, 1, 12};
    Check(!Decode(wrongVersion, sizeof(wrongVersion), frame) &&
              !Decode(wrongOpcode, sizeof(wrongOpcode), frame) &&
              !Decode(zeroSession, sizeof(zeroSession), frame) &&
              !Decode(invalidReason, sizeof(invalidReason), frame),
          "version opcode session and reason bounds are rejected");
    std::array<uint8_t, 20> missingDurable = {1, 0x81, 1, 8};
    std::array<uint8_t, 20> unexpectedIdentity = {1, 0x81, 1, 0};
    unexpectedIdentity[19] = 1U;
    Check(!Decode(missingDurable.data(), missingDurable.size(), frame) &&
              !Decode(unexpectedIdentity.data(), unexpectedIdentity.size(), frame),
          "durable identity dependencies are rejected");
    auto wrongBitCount = end0;
    wrongBitCount[5] = 0U;
    Check(!Decode(wrongBitCount.data(), wrongBitCount.size(), frame),
          "non-five-bit mask is rejected");
    Check(!Decode(end0.data(), end0.size() - 1U, frame),
          "wrong frame length is rejected");

    if (g_failures == 0) {
        std::puts("[BLE_OWNER_PAIRING_PROTOCOL] ok");
        return 0;
    }
    std::printf("[BLE_OWNER_PAIRING_PROTOCOL] failures=%d\n", g_failures);
    return 1;
}
