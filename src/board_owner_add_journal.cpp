#include "modules/board_owner_persistence.hpp"

namespace modules {

bool BoardOwnerPeerSet::Contains(const BoardOwnerPeer& peer) const {
    if (count > peers.size()) return false;
    for (uint8_t i = 0U; i < count; ++i) if (peers[i] == peer) return true;
    return false;
}

bool BoardOwnerPeerSet::Valid() const {
    if (count == 0U || count > peers.size()) return false;
    for (uint8_t i = 0U; i < count; ++i) {
        bool nonzero = false;
        for (const auto byte : peers[i].address) nonzero |= byte != 0U;
        if (peers[i].type > 1U || !nonzero) return false;
        for (uint8_t j = 0U; j < i; ++j) if (peers[i] == peers[j]) return false;
    }
    for (size_t i = count; i < peers.size(); ++i) if (!(peers[i] == BoardOwnerPeer{})) return false;
    return true;
}

bool BoardOwnerPeerSet::ContainsAll(const BoardOwnerPeerSet& other) const {
    if (!Valid() || !other.Valid()) return false;
    for (uint8_t i = 0U; i < other.count; ++i) if (!Contains(other.peers[i])) return false;
    return true;
}

bool BoardOwnerAddBaseline::Valid() const {
    if (!saved.Valid() || saved.count >= MAX_SAVED_PHONES || rpaCount > rpas.size()) return false;
    for (uint8_t i = 0U; i < rpaCount; ++i) {
        if (rpas[i].rpa.type > 3U || rpas[i].identity.type > 3U) return false;
        for (uint8_t j = 0U; j < i; ++j) if (rpas[i].rpa == rpas[j].rpa) return false;
    }
    for (size_t i = rpaCount; i < rpas.size(); ++i)
        if (!(rpas[i] == BoardOwnerRpaRecord{})) return false;
    return true;
}

BoardOwnerAddBaseline ReadBoardOwnerAddBaseline(const JsonDocument& document) {
    BoardOwnerAddBaseline baseline{};
    const JsonArrayConst record = document[KEY_BOARD_OWNER_V1].as<JsonArrayConst>();
    if (record.size() != OWNER_JOURNAL_SLOTS || !record[0U].is<uint32_t>() ||
        record[0U].as<uint32_t>() != 3U || !record[1U].is<uint32_t>() ||
        record[1U].as<uint32_t>() != 3U || !record[2U].is<uint32_t>() ||
        record[2U].as<uint32_t>() == 0U || record[2U].as<uint32_t>() >= MAX_SAVED_PHONES) return {};
    baseline.saved.count = static_cast<uint8_t>(record[2U].as<uint32_t>());
    for (size_t slot = 3U; slot < 17U; ++slot) {
        if (!record[slot].is<uint32_t>() || record[slot].as<uint32_t>() > 255U) return {};
        const size_t peer = (slot - 3U) / 7U;
        const size_t part = (slot - 3U) % 7U;
        const auto value = static_cast<uint8_t>(record[slot].as<uint32_t>());
        if (peer >= baseline.saved.count && value != 0U) return {};
        if (part == 0U) baseline.saved.peers[peer].type = value;
        else baseline.saved.peers[peer].address[part - 1U] = value;
    }
    if (!record[17U].is<uint32_t>() || record[17U].as<uint32_t>() > MAX_SAVED_PHONES) return {};
    baseline.rpaCount = static_cast<uint8_t>(record[17U].as<uint32_t>());
    for (size_t slot = 18U; slot < OWNER_JOURNAL_SLOTS; ++slot) {
        if (!record[slot].is<uint32_t>() || record[slot].as<uint32_t>() > 255U) return {};
        const size_t entry = (slot - 18U) / 14U;
        const size_t part = (slot - 18U) % 14U;
        const auto value = static_cast<uint8_t>(record[slot].as<uint32_t>());
        if (entry >= baseline.rpaCount && value != 0U) return {};
        auto& address = part < 7U ? baseline.rpas[entry].rpa : baseline.rpas[entry].identity;
        if (part % 7U == 0U) address.type = value;
        else address.address[part % 7U - 1U] = value;
    }
    return baseline.Valid() ? baseline : BoardOwnerAddBaseline{};
}

bool PrepareBoardOwnerJournal(JsonDocument& document) {
    const auto owner = ReadBoardOwner(document);
    if (document.overflowed() || owner == BoardOwnerRecord::Invalid ||
        (!document.isNull() && !document.is<JsonObject>())) return false;
    JsonArray record = document[KEY_BOARD_OWNER_V1].as<JsonArray>();
    if (record.size() == OWNER_JOURNAL_SLOTS) return true;
    const bool missing = record.isNull();
    const size_t required = JSON_ARRAY_SIZE(OWNER_JOURNAL_SLOTS) -
        (missing ? 0U : JSON_ARRAY_SIZE(2U)) + (missing ? JSON_OBJECT_SIZE(1U) : 0U);
    if (document.memoryUsage() > document.capacity() ||
        document.capacity() - document.memoryUsage() < required ||
        (!missing && record.size() != 2U)) return false;
    if (missing) record = document[KEY_BOARD_OWNER_V1].to<JsonArray>();
    while (record.size() < OWNER_JOURNAL_SLOTS) record.add(0U);
    record[0U].set(3U);
    record[1U].set(static_cast<uint8_t>(owner));
    return !document.overflowed();
}

bool WriteBoardOwnerAdding(JsonDocument& document, const BoardOwnerAddBaseline& baseline) {
    const auto owner = ReadBoardOwner(document);
    JsonArray record = document[KEY_BOARD_OWNER_V1].as<JsonArray>();
    if (document.overflowed() || !baseline.Valid() ||
        (owner != BoardOwnerRecord::Active && owner != BoardOwnerRecord::Adding) ||
        record.size() != OWNER_JOURNAL_SLOTS || record[0U].as<uint32_t>() != 3U) return false;
    record[1U].set(3U);
    record[2U].set(baseline.saved.count);
    for (size_t peer = 0U; peer < 2U; ++peer) {
        record[3U + peer * 7U].set(peer < baseline.saved.count ? baseline.saved.peers[peer].type : 0U);
        for (size_t byte = 0U; byte < 6U; ++byte)
            record[4U + peer * 7U + byte].set(peer < baseline.saved.count ? baseline.saved.peers[peer].address[byte] : 0U);
    }
    record[17U].set(baseline.rpaCount);
    for (size_t i = 0U; i < MAX_SAVED_PHONES; ++i) {
        for (size_t part = 0U; part < 14U; ++part) {
            const auto& address = part < 7U ? baseline.rpas[i].rpa : baseline.rpas[i].identity;
            const auto value = part % 7U == 0U ? address.type : address.address[part % 7U - 1U];
            record[18U + i * 14U + part].set(i < baseline.rpaCount ? value : 0U);
        }
    }
    return true;
}

} // namespace modules
