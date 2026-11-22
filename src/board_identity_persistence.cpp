#include "modules/board_identity_persistence.hpp"

#include <cstring>

#include "modules/game_history_persistence.hpp"

namespace modules {
namespace {
bool Nonzero(const std::array<uint8_t, 16U>& bytes) {
    for (const uint8_t value : bytes) if (value != 0U) return true;
    return false;
}
int Nibble(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}
} // namespace

BoardIdentityRead ReadBoardIdentity(const JsonDocument& document, BoardIdentity& out) {
    out = {};
    if (!document.containsKey(KEY_BOARD_IDENTITY_V1)) return BoardIdentityRead::Absent;
    const JsonVariantConst value = document[KEY_BOARD_IDENTITY_V1];
    if (!value.is<JsonArrayConst>()) return BoardIdentityRead::Invalid;
    const auto record = value.as<JsonArrayConst>();
    if (record.size() != 2U || !record[0U].is<uint32_t>() ||
        record[0U].as<uint32_t>() != 1U || !record[1U].is<const char*>()) {
        return BoardIdentityRead::Invalid;
    }
    const char* text = record[1U].as<const char*>();
    if (text == nullptr || std::strlen(text) != 32U) return BoardIdentityRead::Invalid;
    BoardIdentity candidate;
    for (size_t index = 0U; index < candidate.bytes.size(); ++index) {
        const int high = Nibble(text[index * 2U]);
        const int low = Nibble(text[index * 2U + 1U]);
        if (high < 0 || low < 0) return BoardIdentityRead::Invalid;
        candidate.bytes[index] = static_cast<uint8_t>((high << 4) | low);
    }
    if (!Nonzero(candidate.bytes)) return BoardIdentityRead::Invalid;
    out = candidate;
    return BoardIdentityRead::Current;
}

bool WriteBoardIdentity(JsonDocument& document, const BoardIdentity& identity) {
    if (!Nonzero(identity.bytes)) return false;
    constexpr char digits[] = "0123456789abcdef";
    char text[33U] = {};
    for (size_t index = 0U; index < identity.bytes.size(); ++index) {
        text[index * 2U] = digits[identity.bytes[index] >> 4U];
        text[index * 2U + 1U] = digits[identity.bytes[index] & 15U];
    }
    auto record = document[KEY_BOARD_IDENTITY_V1].to<JsonArray>();
    if (record.isNull()) return false;
    record.add(1U);
    record.add(text);
    return !document.overflowed();
}

bool IsBoardIdentitySettingKey(std::string_view key) {
    const std::string_view identityKey(KEY_BOARD_IDENTITY_V1);
    if (key.size() != identityKey.size()) return false;
    for (size_t i = 0; i < key.size(); ++i) {
        const char c = key[i] >= 'a' && key[i] <= 'z' ? static_cast<char>(key[i] - 'a' + 'A') : key[i];
        if (c != identityKey[i]) return false;
    }
    return true;
}

void BoardIdentityPersistence::ObserveVerifiedPrimaryLoad(const JsonDocument& document) {
    if (m_configured) return;
    m_verifiedPrimaryIdentity =
        ReadBoardIdentity(document, m_identity) == BoardIdentityRead::Current;
}

bool BoardIdentityPersistence::Prepare(JsonDocument& document, FillRandom random,
                                       void* context) {
    // Called once during boot, before worker startup; identity is then immutable.
    if (m_configured) return false;
    m_configured = true;
    m_durable.store(false, std::memory_order_release);
    m_prepared = false;
    m_preparedExisting = false;
    m_lastGameIdentity = {};
    m_random = random;
    m_context = context;
    const BoardIdentity primaryIdentity = m_identity;
    const auto read = ReadBoardIdentity(document, m_identity);
    m_preparedExisting = read == BoardIdentityRead::Current;
    const bool primaryMatches = m_verifiedPrimaryIdentity &&
        read == BoardIdentityRead::Current && m_identity == primaryIdentity;
    if (read == BoardIdentityRead::Invalid) return false;
    if (read == BoardIdentityRead::Absent) {
        bool generated = false;
        for (uint8_t attempt = 0U; attempt < 4U && !generated; ++attempt) {
            if (random == nullptr || !random(context, m_identity.bytes.data(), m_identity.bytes.size())) return false;
            generated = Nonzero(m_identity.bytes);
        }
        if (!generated) return false;
        if (!WriteBoardIdentity(document, m_identity)) {
            (void)document.remove(KEY_BOARD_IDENTITY_V1);
            return false;
        }
    }
    GameHistoryPersistenceState history;
    if (ReadGameHistoryPersistenceState(document, history) && history.present) {
        m_lastGameIdentity = history.gameId;
    }
    m_prepared = true;
    m_durable.store(primaryMatches, std::memory_order_release);
    return true;
}

bool BoardIdentityPersistence::MatchesExistingPreparedIdentity(
    const JsonDocument& document) const {
    BoardIdentity candidate;
    return m_prepared && m_preparedExisting &&
        ReadBoardIdentity(document, candidate) == BoardIdentityRead::Current &&
        candidate == m_identity;
}

bool BoardIdentityPersistence::GenerateGameIdentity(game_history::GameId& out) {
    if (!m_prepared || m_random == nullptr) return false;
    for (uint8_t attempt = 0U; attempt < 4U; ++attempt) {
        game_history::GameId candidate;
        if (!m_random(m_context, candidate.bytes.data(), candidate.bytes.size())) return false;
        if (!game_history::IsValidGameId(candidate) || candidate == m_lastGameIdentity ||
            candidate.bytes == m_identity.bytes) continue;
        m_lastGameIdentity = candidate;
        out = candidate;
        return true;
    }
    return false;
}

void BoardIdentityPersistence::OnDurableSettingsSnapshot(const JsonDocument& document) {
    BoardIdentity persisted;
    const bool matches = m_prepared &&
        ReadBoardIdentity(document, persisted) == BoardIdentityRead::Current &&
        persisted == m_identity;
    m_durable.store(matches, std::memory_order_release);
}

} // namespace modules
