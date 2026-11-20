#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/ble_owner_pairing_protocol.hpp"

namespace modules::owner_pairing_session_wire {

inline constexpr uint8_t VERSION = 1U;
inline constexpr uint8_t ORDERED_OPCODE = 27U;
inline constexpr uint8_t STATUS_CODE = 40U;
inline constexpr uint8_t IDENTITY_LOW_CODE = 41U;
inline constexpr uint8_t IDENTITY_HIGH_CODE = 42U;
inline constexpr size_t COMMAND_BYTES = 5U;
inline constexpr size_t STATUS_BYTES = 8U;
inline constexpr size_t IDENTITY_HALF_BYTES = 8U;
inline constexpr uint8_t EITHER_END = 2U;

using Reason = ble_owner_pairing::Reason;
enum class Action : uint8_t { Begin = 1U, Cancel = 2U, BeginPhoneSecurity = 3U };
enum class State : uint8_t {
    Unavailable,
    Unpaired,
    AwaitPhysical,
    Securing,
    PersistingOwner,
    OwnerReady,
    OwnedOther,
    Clearing,
    Failed,
    ClearPrompt,
};
enum Flag : uint8_t {
    OwnerRetained = 1U,
    ConnectionOwnerReady = 2U,
    IdentityDurable = 4U,
    CanAddPhone = 8U,
    NativeBondAllowed = 16U,
};

struct Command {
    Action action = Action::Begin;
    uint16_t attempt = 0U;
    uint8_t physicalEnd = EITHER_END;
};
struct Status {
    State state = State::Unavailable;
    Reason reason = Reason::None;
    uint8_t flags = 0U;
    uint16_t attempt = 0U;
    uint8_t physicalEnd = 0U;
};

inline bool DecodeCommand(const uint8_t* bytes, size_t size, Command& out) {
    if (bytes == nullptr || size != COMMAND_BYTES || bytes[0] != VERSION ||
        bytes[1] < static_cast<uint8_t>(Action::Begin) ||
        bytes[1] > static_cast<uint8_t>(Action::BeginPhoneSecurity) || bytes[4] > EITHER_END) return false;
    const uint16_t attempt = static_cast<uint16_t>(bytes[2]) |
        static_cast<uint16_t>(static_cast<uint16_t>(bytes[3]) << 8U);
    if (attempt == 0U) return false;
    out = {static_cast<Action>(bytes[1]), attempt, bytes[4]};
    return true;
}

inline bool ValidStatus(const Status& status) {
    if (static_cast<uint8_t>(status.state) > static_cast<uint8_t>(State::ClearPrompt) ||
        static_cast<uint8_t>(status.reason) > static_cast<uint8_t>(Reason::BondDeleteFailed) ||
        (status.flags & ~uint8_t{31U}) != 0U || status.physicalEnd > EITHER_END ||
        (status.physicalEnd == EITHER_END && status.state != State::AwaitPhysical)) return false;
    const bool ready = (status.flags & ConnectionOwnerReady) != 0U;
    if ((status.flags & NativeBondAllowed) != 0U &&
        (status.state != State::Securing || (status.flags & IdentityDurable) == 0U)) return false;
    if ((status.flags & CanAddPhone) != 0U &&
        (status.state != State::OwnedOther ||
         (status.flags & (OwnerRetained | IdentityDurable)) != (OwnerRetained | IdentityDurable))) return false;
    if (ready && ((status.flags & (OwnerRetained | IdentityDurable)) !=
                      (OwnerRetained | IdentityDurable) ||
                  (status.state != State::OwnerReady && status.state != State::ClearPrompt))) return false;
    if (status.state == State::OwnerReady && !ready) return false;
    if ((status.state == State::AwaitPhysical || status.state == State::Securing ||
         status.state == State::PersistingOwner || status.state == State::ClearPrompt) &&
        status.attempt == 0U) return false;
    return true;
}

inline bool DecodeStatus(const uint8_t* bytes, size_t size, Status& out) {
    if (bytes == nullptr || size != STATUS_BYTES || bytes[0] != VERSION || bytes[7] != 0U) return false;
    const Status candidate{
        static_cast<State>(bytes[1]), static_cast<Reason>(bytes[2]), bytes[3],
        static_cast<uint16_t>(static_cast<uint16_t>(bytes[4]) |
                              static_cast<uint16_t>(static_cast<uint16_t>(bytes[5]) << 8U)),
        bytes[6]};
    if (!ValidStatus(candidate)) return false;
    out = candidate;
    return true;
}

inline bool EncodeStatus(const Status& status, uint8_t* bytes, size_t size) {
    if (bytes == nullptr || size < STATUS_BYTES || !ValidStatus(status)) return false;
    bytes[0] = VERSION;
    bytes[1] = static_cast<uint8_t>(status.state);
    bytes[2] = static_cast<uint8_t>(status.reason);
    bytes[3] = status.flags;
    bytes[4] = static_cast<uint8_t>(status.attempt);
    bytes[5] = static_cast<uint8_t>(status.attempt >> 8U);
    bytes[6] = status.physicalEnd;
    bytes[7] = 0U;
    return true;
}

} // namespace modules::owner_pairing_session_wire
