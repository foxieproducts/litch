#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "modules/connection_session_codec.hpp"

namespace wire = modules::connection_session_codec;

namespace {

int failures = 0;

void Check(const bool value, const char* message) {
    if (value) return;
    ++failures;
    std::printf("FAIL: %s\n", message);
}

void SharedVectors() {
    std::ifstream input("test/fixtures/connection_session_v4.csv");
    Check(input.good(), "shared wire fixture is available");
    std::string line;
    size_t count = 0U;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::stringstream row(line);
        std::string name, valid, hex;
        std::getline(row, name, ',');
        std::getline(row, valid, ',');
        std::getline(row, hex);
        std::vector<uint8_t> bytes;
        for (size_t index = 0U; index < hex.size(); index += 2U) {
            bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(index, 2U), nullptr, 16)));
        }
        wire::PacketView packet{{wire::Kind::Ordered, 255U, 99U, 17U}, {}};
        const auto result = wire::Codec::Decode({bytes.data(), bytes.size()}, packet, 20U);
        Check((result == wire::Error::None) == (valid == "1"), name.c_str());
        if (result != wire::Error::None) {
            Check(packet.header.token == 99U && packet.header.sequence == 17U &&
                      packet.header.code == 255U && packet.body.IsEmpty(),
                  "invalid input leaves the previous decoded view untouched");
        } else {
            std::array<uint8_t, 20U> output{};
            size_t size = 0U;
            Check(wire::Codec::Encode(packet, {output.data(), output.size()}, size) == wire::Error::None,
                  "shared decoded vector re-encodes");
            Check(size == bytes.size() && std::equal(bytes.begin(), bytes.end(), output.begin()),
                  "both endpoints use byte-exact canonical encoding");
            if (packet.header.kind == wire::Kind::Control) {
                for (const size_t length : {wire::HEADER_BYTES - 1U, bytes.size() - 1U}) {
                    wire::PacketView truncated;
                    Check(wire::Codec::Decode({bytes.data(), length}, truncated) != wire::Error::None,
                          "control truncation cannot admit a partial state transition");
                }
            }
        }
        ++count;
    }
    Check(count == 53U, "shared fixture corpus has all expected boundaries");
}

void PacketBoundsAndFailureAtomicity() {
    std::array<uint8_t, 502U> body{};
    for (size_t index = 0U; index < body.size(); ++index) body[index] = static_cast<uint8_t>(index);
    const wire::PacketView packet{{wire::Kind::Object, 37U, 12U, 2U}, {body.data(), body.size()}};
    std::array<uint8_t, 513U> output{};
    output.fill(0x5aU);
    size_t size = 99U;
    Check(wire::Codec::Encode(packet, {output.data(), 511U}, size) == wire::Error::Length,
          "bulk encode respects supplied capacity");
    Check(size == 0U && output.front() == 0x5aU && output.back() == 0x5aU,
          "failed encode publishes no length or partial header");
    Check(wire::Codec::Encode(packet, {output.data(), output.size()}, size) == wire::Error::None && size == 512U,
          "maximum negotiated bulk packet fits without allocation");
    wire::PacketView decoded;
    Check(wire::Codec::Decode({output.data(), size}, decoded, 244U) == wire::Error::Length,
          "actual negotiated payload caps admission");
    Check(wire::Codec::Decode({output.data(), size}, decoded, 512U) == wire::Error::None,
          "same bulk packet fits a larger negotiated payload");
    Check(decoded.body.Data() == output.data() + wire::HEADER_BYTES && decoded.body.Size() == 502U,
          "decode returns bounded borrowed storage");
    Check(wire::Codec::Decode({output.data(), 513U}, decoded) == wire::Error::Length,
          "oversized packet rejected before decoding");
    Check(wire::Codec::Decode({output.data(), 10U}, decoded, 19U) == wire::Error::Length,
          "unsupported negotiated minimum rejected");
    Check(wire::Codec::Decode({nullptr, 20U}, decoded) == wire::Error::Length,
          "null input rejected before dereference");
    const wire::PacketView malformed{{wire::Kind::Control, uint8_t(wire::Control::Progress), 12U, 0U}, {nullptr, 10U}};
    Check(wire::Codec::Encode(malformed, {output.data(), output.size()}, size) == wire::Error::Length,
          "null nonempty body rejected before control validation");
}

} // namespace

int main() {
    SharedVectors();
    PacketBoundsAndFailureAtomicity();
    std::printf("[CONNECTION_SESSION_CODEC] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
