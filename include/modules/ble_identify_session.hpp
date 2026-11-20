#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace modules::ble_identify {

inline constexpr uint8_t ORDERED_OPCODE = 28U;
inline constexpr uint8_t VERSION = 1U;
inline constexpr size_t COMMAND_BYTES = 8U;
inline constexpr uint32_t COOLDOWN_MS = 300000U;
enum class Action : uint8_t { Probe = 0U, Start = 1U, Stop = 2U };
enum class Rejection : uint16_t { Cooldown = 0x1c01U, Capacity = 0x1c02U, PeerUnavailable = 0x1c03U };
struct Command {
    Action action = Action::Probe;
    uint8_t number = 0U;
    uint8_t red = 0U, green = 0U, blue = 0U;
    uint16_t durationMs = 0U;
};
inline bool Decode(const uint8_t* bytes, const size_t size, Command& out) {
    if (bytes == nullptr || size != COMMAND_BYTES || bytes[0U] != VERSION || bytes[1U] > 2U) return false;
    const Command value{static_cast<Action>(bytes[1U]), bytes[2U], bytes[3U], bytes[4U], bytes[5U],
        static_cast<uint16_t>(uint16_t(bytes[6U]) | (uint16_t(bytes[7U]) << 8U))};
    if (value.action == Action::Start) {
        if (value.number > 99U || value.durationMs < 250U || value.durationMs > 60000U) return false;
    } else {
        for (size_t index = 2U; index < size; ++index) if (bytes[index] != 0U) return false;
    }
    out = value;
    return true;
}

struct PeerIdentity {
    std::array<uint8_t, 6U> address{};
    uint8_t type = 0U;
    bool available = false;
    bool operator==(const PeerIdentity& other) const {
        return available && other.available && type == other.type && address == other.address;
    }
};

// Connection-independent, bounded RAM accounting. Receipt replay never enters
// this owner; only a new accepted Ordered Start asks for admission.
class RateLimit {
  public:
    enum class Result : uint8_t { Allowed, Cooldown, Capacity, PeerUnavailable };
    Result Start(const PeerIdentity& peer, const uint32_t now) {
        if (!peer.available) return Result::PeerUnavailable;
        Bucket* bucket = nullptr;
        Bucket* reusable = nullptr;
        for (auto& candidate : m_peers) {
            if (candidate.peer == peer) { bucket = &candidate; break; }
            if (reusable == nullptr && (!candidate.peer.available || Expired(candidate, now))) reusable = &candidate;
        }
        if (bucket == nullptr) {
            if (reusable == nullptr) return Result::Capacity;
            *reusable = {};
            reusable->peer = peer;
            bucket = reusable;
        }
        if (bucket->cooling) {
            if (now - bucket->lastActivity < COOLDOWN_MS) return Result::Cooldown;
            bucket->cooling = false;
            bucket->starts = 0U;
        } else if (now - bucket->lastActivity >= COOLDOWN_MS) bucket->starts = 0U;
        // Three starts are allowed. The fourth starts one fixed ignore window.
        // Ignored requests never move its boundary.
        bucket->lastActivity = now;
        if (bucket->starts == 3U) { bucket->cooling = true; return Result::Cooldown; }
        ++bucket->starts;
        return Result::Allowed;
    }

  private:
    struct Bucket {
        PeerIdentity peer{};
        uint32_t lastActivity = 0U;
        uint8_t starts = 0U;
        bool cooling = false;
    };
    static bool Expired(const Bucket& bucket, const uint32_t now) {
        return now - bucket.lastActivity >= COOLDOWN_MS;
    }
    std::array<Bucket, 8U> m_peers{};
};

} // namespace modules::ble_identify
