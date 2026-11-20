#pragma once

#include <array>
#include <cstdint>

namespace modules {

inline constexpr uint8_t MAX_SAVED_PHONES = 3U;
struct BoardOwnerPeer {
    uint8_t type = 0U;
    std::array<uint8_t, 6U> address{};
    bool operator==(const BoardOwnerPeer& other) const {
        return type == other.type && address == other.address;
    }
};
struct BoardOwnerPeerSet {
    uint8_t count = 0U;
    std::array<BoardOwnerPeer, MAX_SAVED_PHONES> peers{};
    bool Contains(const BoardOwnerPeer& peer) const;
    bool Valid() const;
    bool ContainsAll(const BoardOwnerPeerSet& other) const;
    bool operator==(const BoardOwnerPeerSet& other) const {
        return count == other.count && peers == other.peers;
    }
};
struct BoardOwnerRpaRecord {
    BoardOwnerPeer rpa{}, identity{};
    bool operator==(const BoardOwnerRpaRecord& other) const {
        return rpa == other.rpa && identity == other.identity;
    }
};
struct BoardOwnerAddBaseline {
    BoardOwnerPeerSet saved{};
    uint8_t rpaCount = 0U;
    std::array<BoardOwnerRpaRecord, MAX_SAVED_PHONES> rpas{};
    bool Valid() const;
    bool operator==(const BoardOwnerAddBaseline& other) const {
        return saved == other.saved && rpaCount == other.rpaCount && rpas == other.rpas;
    }
};


} // namespace modules
