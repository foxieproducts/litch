#pragma once

#include <cstdint>

#include <ArduinoJson.h>

#include "modules/game_history_protocol.hpp"

namespace modules {

inline constexpr const char* KEY_GAME_HISTORY_V1 = "GAME_HISTORY_V1";
inline constexpr uint8_t GAME_HISTORY_PERSISTENCE_VERSION = 1U;

// Current-game archive identity carried inside the atomic settings snapshot.
// The journal is a mirror of this authority, never a replacement for it.
struct GameHistoryPersistenceState {
    // Nonserialized quarantine: malformed optional archive authority must not
    // prevent restoring/saving the current game or be migrated as legacy.
    bool valid = true;
    bool present = false;
    game_history::GameId gameId{};
    uint64_t lineRevision = 0U;
    uint16_t currentPly = 0U;
    bool captureIncomplete = false;
    bool shouldSeal = false;
    game_history::ArchiveStatus status =
        game_history::ArchiveStatus::Unfinished;
    game_history::Outcome outcome = game_history::Outcome::Ongoing;
    game_history::Winner winner = game_history::Winner::None;

    // The existing incomplete flag and a saturated bound form a durable stop
    // marker. Once exact revision/ply tracking is lost, Undo cannot resume it.
    bool IsCaptureExhausted() const {
        return captureIncomplete &&
            (lineRevision == game_history::MAX_LINE_REVISION || currentPly == 4096U);
    }
};

bool WriteGameHistoryPersistenceState(
    JsonDocument& doc,
    const GameHistoryPersistenceState& state);
bool ReadGameHistoryPersistenceState(
    const JsonDocument& doc,
    GameHistoryPersistenceState& out);

} // namespace modules
