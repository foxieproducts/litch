#include <cstring>

#include "foxie_embedded/byte_order.hpp"
#include "modules/connection_session_codec.hpp"

namespace modules::connection_session_codec {
namespace {

bool ValidLimit(const size_t limit) {
    return limit >= MIN_PACKET_BYTES && limit <= MAX_PACKET_BYTES;
}

bool ValidGrant(const uint16_t bodyBytes, const uint8_t capacity,
                const uint8_t lane) {
    return capacity != 0U && lane <= 1U &&
           capacity <= (lane == 0U ? MAX_ORDERED_CAPACITY : MAX_REVERSE_CAPACITY) &&
           bodyBytes != 0U && bodyBytes <= MAX_PACKET_BYTES - HEADER_BYTES &&
           (lane != 0U || bodyBytes <= MIN_PACKET_BYTES - HEADER_BYTES);
}

uint16_t LowBits(const uint32_t count) {
    return count >= 16U ? UINT16_MAX : static_cast<uint16_t>((1U << count) - 1U);
}

Error ValidateControl(const PacketView& packet) {
    const auto& header = packet.header;
    const auto body = packet.body;
    const uint8_t* const bytes = body.Data();
    const auto code = static_cast<Control>(header.code);
    const bool hello = code == Control::Hello;
    if ((header.token == 0U) != hello) return Error::Identity;
    switch (code) {
    case Control::Hello:
    case Control::Welcome:
        if (header.sequence != 0U || body.Size() != 10U ||
            fxe::ReadU64LE(bytes) == 0U || !ValidLimit(fxe::ReadU16LE(bytes + 8U))) {
            return Error::Control;
        }
        break;
    case Control::Identity:
        if (header.sequence != 0U || body.Size() != 10U ||
            fxe::ReadU64LE(bytes) == 0U ||
            (fxe::ReadU16LE(bytes + 8U) & ~KNOWN_FEATURES_MASK) != 0U) {
            return Error::Control;
        }
        break;
    case Control::Ready:
    case Control::Activated:
        if (header.sequence != 0U || body.Size() != 4U) return Error::Control;
        break;
    case Control::Open:
        if (header.sequence == 0U || body.Size() != 8U ||
            fxe::ReadU16LE(bytes) == 0U ||
            !ValidGrant(fxe::ReadU16LE(bytes + 4U), bytes[3U], bytes[2U]) ||
            fxe::ReadU16LE(bytes + 6U) > MAX_EXPECTED_FEEDBACK_MS) {
            return Error::Control;
        }
        break;
    case Control::Opened:
        if (header.sequence == 0U || body.Size() != 10U || bytes[7U] > 1U) {
            return Error::Control;
        }
        if (fxe::ReadU16LE(bytes + 8U) == 0U) {
            if (fxe::ReadU32LE(bytes) == 0U || fxe::ReadU32LE(bytes) == header.token ||
                !ValidGrant(fxe::ReadU16LE(bytes + 4U), bytes[6U], bytes[7U])) {
                return Error::Control;
            }
        } else if (fxe::ReadU32LE(bytes) != 0U ||
                   fxe::ReadU16LE(bytes + 4U) != 0U || bytes[6U] != 0U) {
            return Error::Control;
        }
        break;
    case Control::Progress: {
        if (body.Size() != 10U) return Error::Control;
        const uint32_t confirmed = fxe::ReadU32LE(bytes);
        const uint32_t granted = fxe::ReadU32LE(bytes + 4U);
        if (confirmed > header.sequence || granted < header.sequence ||
            granted > LAST_SEQUENCE || granted - confirmed > MAX_WINDOW ||
            (fxe::ReadU16LE(bytes + 8U) & ~LowBits(granted - header.sequence)) != 0U) {
            return Error::Control;
        }
        break;
    }
    case Control::Checkpoint:
        if (body.Size() != 10U ||
            (fxe::ReadU16LE(bytes + 8U) & ~LowBits(header.sequence)) != 0U) {
            return Error::Control;
        }
        break;
    case Control::Outcome:
        if (header.sequence == 0U || body.Size() != 3U || bytes[0U] > 2U ||
            ((bytes[0U] == 0U) != (fxe::ReadU16LE(bytes + 1U) == 0U))) {
            return Error::Control;
        }
        break;
    case Control::Confirm:
    case Control::Query:
        if (!body.IsEmpty()) return Error::Control;
        break;
    case Control::Close:
    case Control::Closed:
    case Control::Heartbeat:
        if (header.sequence != 0U || !body.IsEmpty()) return Error::Control;
        break;
    default:
        return Error::Control;
    }
    return Error::None;
}

Error Validate(const PacketView& packet) {
    if (static_cast<uint8_t>(packet.header.kind) > static_cast<uint8_t>(Kind::Object)) {
        return Error::Kind;
    }
    if (packet.header.sequence > LAST_SEQUENCE) return Error::Identity;
    if (packet.header.kind == Kind::Control) return ValidateControl(packet);
    return packet.header.token != 0U && packet.header.sequence != 0U
        ? Error::None : Error::Identity;
}

} // namespace

Error Codec::Decode(const fxe::ConstByteSpan bytes, PacketView& output,
                    const size_t packetLimit) {
    if (!ValidLimit(packetLimit) || bytes.Data() == nullptr ||
        bytes.Size() < HEADER_BYTES || bytes.Size() > packetLimit) return Error::Length;
    if ((bytes[0U] >> 4U) != VERSION) return Error::Version;
    PacketView packet{{static_cast<Kind>(bytes[0U] & 0x0fU), bytes[1U],
                       fxe::ReadU32LE(bytes.Data() + 2U),
                       fxe::ReadU32LE(bytes.Data() + 6U)},
                      {bytes.Data() + HEADER_BYTES, bytes.Size() - HEADER_BYTES}};
    const Error error = Validate(packet);
    if (error == Error::None) output = packet;
    return error;
}

Error Codec::Encode(const PacketView& packet, fxe::ByteSpan output,
                    size_t& size) {
    size = 0U;
    if (packet.body.Size() > MAX_PACKET_BYTES - HEADER_BYTES ||
        (packet.body.Size() != 0U && packet.body.Data() == nullptr) ||
        output.Data() == nullptr || output.Size() < HEADER_BYTES + packet.body.Size()) {
        return Error::Length;
    }
    const Error error = Validate(packet);
    if (error != Error::None) return error;
    auto* const bytes = output.Data();
    bytes[0U] = static_cast<uint8_t>((VERSION << 4U) | static_cast<uint8_t>(packet.header.kind));
    bytes[1U] = packet.header.code;
    fxe::WriteU32LE(packet.header.token, bytes + 2U);
    fxe::WriteU32LE(packet.header.sequence, bytes + 6U);
    if (!packet.body.IsEmpty()) std::memcpy(bytes + HEADER_BYTES, packet.body.Data(), packet.body.Size());
    size = HEADER_BYTES + packet.body.Size();
    return Error::None;
}

} // namespace modules::connection_session_codec
