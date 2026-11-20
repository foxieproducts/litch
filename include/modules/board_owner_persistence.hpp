#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <array>
#include <string_view>

#include "modules/board_owner_peer.hpp"

namespace modules {

inline constexpr const char* KEY_BOARD_OWNER_V1 = "BOARD_OWNER_V1";

enum class BoardOwnerRecord : uint8_t {
    Absent,
    Active,
    Clearing,
    Adding,
    Invalid,
};

inline constexpr size_t OWNER_JOURNAL_SLOTS = 60U;

BoardOwnerRecord ReadBoardOwner(const JsonDocument& document);
// Reads legacy missing/V1 records; writes V2 [2, 0/1/2]. Preparing an absent
// record reserves its two scalar slots. Later valid transitions reuse those
// slots in place, including Absent, without growing ArduinoJson's arena.
bool WriteBoardOwner(JsonDocument& document, BoardOwnerRecord record);
// Reserve the bounded V3 scalar journal once on the settings owner at boot.
// It contains only baseline addresses, never native keys or a phone registry.
bool PrepareBoardOwnerJournal(JsonDocument& document);
BoardOwnerAddBaseline ReadBoardOwnerAddBaseline(const JsonDocument& document);
bool WriteBoardOwnerAdding(JsonDocument& document, const BoardOwnerAddBaseline& baseline);
bool IsBoardOwnerSettingKey(std::string_view key);

// Adapter seam for the existing SettingsPersistenceWorker tracked-receipt API.
// SaveOwnerRecord returning false means no document mutation and no receipt.
// Success returns a nonzero receipt for the exact admitted document generation.
// At most one receipt may be outstanding; Release must succeed before reuse.
// Unknown is a terminal recovery/error outcome, never another pending state.
class BoardOwnerPersistence {
  public:
    using Receipt = uint32_t;
    enum class Outcome : uint8_t {
        Unknown,
        Pending,
        Saved,
        Failed,
    };

    virtual ~BoardOwnerPersistence() = default;
    virtual bool SaveOwnerRecord(BoardOwnerRecord, Receipt&) = 0;
    virtual bool SaveAddingJournal(const BoardOwnerAddBaseline&, Receipt&) { return false; }
    // Only an authenticated existing owner's current epoch/session may request
    // repair. This admits persistence, never identity or product authority.
    virtual bool RequestIdentityRepair(uint64_t) { return false; }
    virtual BoardOwnerAddBaseline AddingBaseline() const { return {}; }
    virtual Outcome GetOutcome(Receipt) const = 0;
    virtual bool Release(Receipt) = 0;
};

} // namespace modules
