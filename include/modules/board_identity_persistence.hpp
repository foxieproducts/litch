#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <ArduinoJson.h>

#include "modules/game_history_protocol.hpp"

namespace modules {

inline constexpr const char* KEY_BOARD_IDENTITY_V1 = "BOARD_IDENTITY_V1";
bool IsBoardIdentitySettingKey(std::string_view key);
struct BoardIdentity {
    std::array<uint8_t, 16U> bytes{};
    bool operator==(const BoardIdentity& other) const { return bytes == other.bytes; }
};
enum class BoardIdentityRead { Absent, Current, Invalid };
BoardIdentityRead ReadBoardIdentity(const JsonDocument& document, BoardIdentity& out);
bool WriteBoardIdentity(JsonDocument& document, const BoardIdentity& identity);

// Auxiliary identity metadata, not a game/history/persistence owner. The existing
// settings loader proves an existing primary identity; the worker proves new
// identities only after verified promotion.
class BoardIdentityPersistence final {
  public:
    using FillRandom = bool (*)(void*, uint8_t*, size_t);
    void ObserveVerifiedPrimaryLoad(const JsonDocument& document);
    bool Prepare(JsonDocument& document, FillRandom random, void* context);
    bool GenerateGameIdentity(game_history::GameId& out);
    void OnDurableSettingsSnapshot(const JsonDocument& document);
    bool IsPrepared() const { return m_prepared; }
    bool MatchesExistingPreparedIdentity(const JsonDocument& document) const;
    bool IsDurable() const { return m_durable.load(std::memory_order_acquire); }
    bool GetDurableIdentity(BoardIdentity& out) const {
        if (!IsDurable()) return false;
        out = m_identity;
        return true;
    }

  private:
    BoardIdentity m_identity{};
    game_history::GameId m_lastGameIdentity{};
    FillRandom m_random = nullptr;
    void* m_context = nullptr;
    bool m_configured = false;
    bool m_prepared = false;
    bool m_preparedExisting = false;
    bool m_verifiedPrimaryIdentity = false;
    std::atomic<bool> m_durable{false};
};

} // namespace modules
