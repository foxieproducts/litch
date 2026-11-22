#pragma once

#include <cstddef>
#include <cstdint>

#include "foxie_embedded/span.hpp"

namespace modules::connection_session_codec {

constexpr uint8_t VERSION = 4U;
constexpr size_t HEADER_BYTES = 10U;
constexpr size_t MIN_PACKET_BYTES = 20U;
constexpr size_t MAX_PACKET_BYTES = 512U;
constexpr uint32_t LAST_SEQUENCE = UINT32_MAX - 1U;
// Wire bounds are independent of each receiver's smaller selected capacity.
constexpr uint8_t MAX_WINDOW = 64U;
constexpr uint8_t MAX_ORDERED_CAPACITY = 16U;
constexpr uint8_t MAX_REVERSE_CAPACITY = 64U;
constexpr uint16_t KNOWN_FEATURES_MASK = 0x007fU;
constexpr uint16_t MAX_EXPECTED_FEEDBACK_MS = 60000U;

enum class Kind : uint8_t {
    Control = 0U,
    State = 1U,
    Patch = 2U,
    Ordered = 3U,
    Object = 4U,
};

enum class Control : uint8_t {
    Hello = 1U,
    Welcome = 2U,
    Identity = 3U,
    Ready = 4U,
    Activated = 5U,
    Open = 6U,
    Opened = 7U,
    Progress = 8U,
    Checkpoint = 9U,
    Outcome = 10U,
    Confirm = 11U,
    Query = 12U,
    Close = 13U,
    Closed = 14U,
    Heartbeat = 15U,
};

struct Header {
    Kind kind = Kind::Control;
    uint8_t code = 0U;
    uint32_t token = 0U;
    uint32_t sequence = 0U;
};

// The body borrows the packet's storage; it is invalid once that storage changes.
// Domain adapters validate their opcode/body and negotiated stream binding before
// admission. A receive-window adapter must retain every semantic identity field
// (kind, opcode and body); retaining the complete packet also satisfies this.
struct PacketView {
    Header header{};
    fxe::ConstByteSpan body{};
};

enum class Error : uint8_t {
    None,
    Length,
    Version,
    Kind,
    Identity,
    Control,
};

class Codec final {
  public:
    // Failure leaves output unchanged. packetLimit is the negotiated ATT payload,
    // not a requested MTU; controls remain valid at the 20-byte minimum.
    static Error Decode(fxe::ConstByteSpan bytes, PacketView& output,
                        size_t packetLimit = MAX_PACKET_BYTES);
    // Failure returns size zero without changing output bytes. Body storage must
    // not overlap output. Encoding alone does not authorize a stream or command.
    static Error Encode(const PacketView& packet, fxe::ByteSpan output,
                        size_t& size);
};

} // namespace modules::connection_session_codec
