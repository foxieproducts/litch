#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace modules::ble_owner_pairing {

inline constexpr const char* CHARACTERISTIC_UUID =
    "6f2a20d1-89df-4ac6-a9f2-53f6f7db0107";
inline constexpr uint8_t VERSION = 1U;

enum class Opcode : uint8_t {
    StatusReq = 1,
    BeginPattern = 2,
    PatternMatches = 3,
    Cancel = 4,
    Status = 0x81,
    Pattern = 0x82,
    State = 0x83,
    Reject = 0xFF,
};

enum class PairingState : uint8_t {
    Unavailable = 0,
    Unpaired,
    PhysicalReady,
    Pattern,
    AwaitHold,
    Securing,
    PersistingOwner,
    OwnerReady,
    OwnedOther,
    Clearing,
    Failed,
};

enum class Reason : uint8_t {
    None = 0,
    Malformed,
    UnsupportedVersion,
    StaleSession,
    PhysicalActionRequired,
    Busy,
    Mismatch,
    Timeout,
    WrongConnection,
    SecurityFailed,
    StorageFailed,
    BondDeleteFailed,
};

enum StatusFlag : uint8_t {
    OwnerActive = 1,
    ConnectionOwnerReady = 2,
    PhysicalReady = 4,
    IdentityDurable = 8,
};

struct Frame {
    Opcode opcode{};
    uint16_t session = 0;
    PairingState state = PairingState::Unavailable;
    Reason reason = Reason::None;
    uint8_t flags = 0;
    std::array<uint8_t, 16> identity{};
    uint8_t viewEnd = 0;
    uint64_t mask = 0;
    uint8_t requestOpcode = 0;
};

constexpr uint8_t Bits(uint64_t value) {
    uint8_t count = 0;
    while (value != 0U) {
        count += uint8_t(value & 1U);
        value >>= 1U;
    }
    return count;
}

constexpr uint64_t Rotate180(uint64_t value) {
    uint64_t rotated = 0;
    for (uint8_t i = 0; i < 64; ++i) {
        rotated |= ((value >> i) & 1U) << (63U - i);
    }
    return rotated;
}

constexpr uint16_t Read16(const uint8_t* data) {
    return uint16_t(data[0]) | (uint16_t(data[1]) << 8U);
}

constexpr uint64_t Read64(const uint8_t* data) {
    uint64_t value = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        value |= uint64_t(data[i]) << (8U * i);
    }
    return value;
}

inline bool ValidStatus(const Frame& frame) {
    if ((frame.flags & 0xF0U) != 0U) {
        return false;
    }
    bool identityNonzero = false;
    for (const uint8_t value : frame.identity) {
        identityNonzero |= value != 0U;
    }
    if (identityNonzero != ((frame.flags & IdentityDurable) != 0U)) {
        return false;
    }
    return (frame.flags & ConnectionOwnerReady) == 0U ||
           ((frame.flags & OwnerActive) != 0U &&
            frame.state == PairingState::OwnerReady);
}

inline bool Decode(const uint8_t* data, size_t len, Frame& out) {
    if (data == nullptr || len < 2U || data[0] != VERSION) {
        return false;
    }
    out = Frame{};
    out.opcode = static_cast<Opcode>(data[1]);
    switch (out.opcode) {
        case Opcode::StatusReq:
        case Opcode::BeginPattern:
            return len == 2U;
        case Opcode::PatternMatches:
        case Opcode::Cancel:
            if (len != 4U) {
                return false;
            }
            out.session = Read16(data + 2U);
            return out.session != 0U;
        case Opcode::Status:
            if (len != 20U || data[2] > 10U) {
                return false;
            }
            out.state = static_cast<PairingState>(data[2]);
            out.flags = data[3];
            for (size_t i = 0; i < 16; ++i) {
                out.identity[i] = data[4U + i];
            }
            return ValidStatus(out);
        case Opcode::Pattern:
            if (len != 13U) {
                return false;
            }
            out.session = Read16(data + 2U);
            out.viewEnd = data[4];
            out.mask = Read64(data + 5U);
            return out.session != 0U && out.viewEnd <= 1U &&
                   Bits(out.mask) == 5U && Rotate180(out.mask) != out.mask;
        case Opcode::State:
            if (len != 6U || data[4] > 10U || data[5] > 11U) {
                return false;
            }
            out.session = Read16(data + 2U);
            out.state = static_cast<PairingState>(data[4]);
            out.reason = static_cast<Reason>(data[5]);
            return out.session != 0U;
        case Opcode::Reject:
            if (len != 4U || data[2] < 1U || data[2] > 4U ||
                data[3] < 1U || data[3] > 11U) {
                return false;
            }
            out.requestOpcode = data[2];
            out.reason = static_cast<Reason>(data[3]);
            return true;
        default:
            return false;
    }
}

inline size_t EncodedLength(const Opcode opcode) {
    switch (opcode) {
        case Opcode::StatusReq:
        case Opcode::BeginPattern:
            return 2U;
        case Opcode::PatternMatches:
        case Opcode::Cancel:
        case Opcode::Reject:
            return 4U;
        case Opcode::State:
            return 6U;
        case Opcode::Pattern:
            return 13U;
        case Opcode::Status:
            return 20U;
        default:
            return 0U;
    }
}

inline size_t Encode(const Frame& frame, uint8_t* out, size_t capacity) {
    const size_t len = EncodedLength(frame.opcode);
    if (out == nullptr || len == 0U || capacity < len) {
        return 0U;
    }
    out[0] = VERSION;
    out[1] = static_cast<uint8_t>(frame.opcode);
    switch (frame.opcode) {
        case Opcode::StatusReq:
        case Opcode::BeginPattern:
            break;
        case Opcode::PatternMatches:
        case Opcode::Cancel:
            out[2] = uint8_t(frame.session);
            out[3] = uint8_t(frame.session >> 8U);
            break;
        case Opcode::Status:
            out[2] = uint8_t(frame.state);
            out[3] = frame.flags;
            for (size_t i = 0; i < 16; ++i) {
                out[4U + i] = frame.identity[i];
            }
            break;
        case Opcode::Pattern:
            out[2] = uint8_t(frame.session);
            out[3] = uint8_t(frame.session >> 8U);
            out[4] = frame.viewEnd;
            for (uint8_t i = 0; i < 8; ++i) {
                out[5U + i] = uint8_t(frame.mask >> (8U * i));
            }
            break;
        case Opcode::State:
            out[2] = uint8_t(frame.session);
            out[3] = uint8_t(frame.session >> 8U);
            out[4] = uint8_t(frame.state);
            out[5] = uint8_t(frame.reason);
            break;
        case Opcode::Reject:
            out[2] = frame.requestOpcode;
            out[3] = uint8_t(frame.reason);
            break;
        default:
            return 0U;
    }
    Frame decoded;
    return Decode(out, len, decoded) ? len : 0U;
}

} // namespace modules::ble_owner_pairing
