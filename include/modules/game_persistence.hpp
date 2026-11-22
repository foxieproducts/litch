#pragma once

#include <cstddef>
#include <cstdint>

#include <ArduinoJson.h>

#include "foxie_embedded/fixed_string.hpp"
#include <litch_max/fen_constants.hpp>
#include "modules/game_history_persistence.hpp"
#include "runtime/board_constants.hpp"
#include "runtime/game_clock.hpp"

namespace modules {

// Persistence keys per docs/spec/saved_game_data.md.
static constexpr const char* KEY_GAME_FEN = "GAME_FEN";
static constexpr const char* KEY_GAME_ACCEPTED_PLAY = "GAME_ACCEPTED_PLAY";
static constexpr const char* KEY_GAME_MOVED_SQUARES = "GAME_MOVED_SQUARES";

static constexpr const char* KEY_GAME_UNDO_BASE_FEN = "GAME_UNDO_BASE_FEN";
static constexpr const char* KEY_GAME_UNDO_BASE_MOVED_SQUARES = "GAME_UNDO_BASE_MOVED_SQUARES";
static constexpr const char* KEY_GAME_UNDO_HISTORY = "GAME_UNDO_HISTORY";
static constexpr const char* KEY_GAME_CPU_MOVE = "GAME_CPU_MOVE";
static constexpr const char* KEY_GAME_CASTLING_ROOK_PROMPT = "GAME_CASTLING_ROOK_PROMPT";
static constexpr const char* KEY_GAME_INTERACTION_V1 = "GAME_INTERACTION_V1";
static constexpr const char* KEY_GAME_VARIANT_V1 = "GAME_VARIANT_V1";
static constexpr const char* KEY_GAME_DUCK_UNDO_V1 = "GAME_DUCK_UNDO_V1";
static constexpr const char* KEY_GAME_CLOCK_V1 = "GAME_CLOCK_V1";
static constexpr const char* KEY_GAME_CLOCK_TERMINAL_V1 = "GAME_CLOCK_TERMINAL_V1";

// Fixed array schema, in order: version, session ID, durable epoch, TIMER,
// base ms, increment ms, flags, active side, White ms, Black ms, timeout side.
// It is intentionally bounded and allocation-free at the coordinator boundary.
static constexpr uint32_t GAME_CLOCK_RECORD_VERSION = 1U;
static constexpr size_t GAME_CLOCK_RECORD_FIELD_COUNT = 11U;

struct GameClockPersistenceState {
    uint32_t sessionId = 0U;
    uint32_t durableEpoch = 0U;
    uint32_t timerSetting = 0U;
    uint32_t baseMs = 0U;
    bool started = false;
    runtime::GameClockState clock{};
    // Completed chess half retained only for timeout presentation. It is not
    // a resumable interaction or a committed compound-turn history entry.
    fxe::FixedString<6> terminalDuckChessMove;
};

// JSON schema details.
static constexpr size_t UNDO_HISTORY_FIELDS_PER_MOVE = 7;

// Undo history capacity is in plies (half-moves). Keep this aligned with the
// runtime state machine + coordinator capacities.
static constexpr size_t UNDO_HISTORY_MOVE_CAPACITY = 128;

using FenString = fxe::FixedString<litch_max::FEN_BUFFER_CAPACITY>;

struct GamePersistenceState {
    FenString fen;
    // Session provenance survives Undo-to-zero history identity replacement.
    bool hasAcceptedMove = false;

    // Array of 0..63 squares that have moved (for efficient LED overlays).
    // Stored as JSON int array.
    uint8_t movedSquares[runtime::BOARD_SQUARE_COUNT] = {};
    uint8_t movedSquaresCount = 0;

    // Undo base snapshot and history.
    FenString undoBaseFen;
    uint8_t undoBaseMovedSquares[runtime::BOARD_SQUARE_COUNT] = {};
    uint8_t undoBaseMovedSquaresCount = 0;

    // Flat int array of (side, movedPiece, capturedPiece, capturedSquare,
    // promotedPiece, from, to) repeated per move.
    uint8_t undoHistory[UNDO_HISTORY_FIELDS_PER_MOVE * UNDO_HISTORY_MOVE_CAPACITY] = {};
    uint16_t undoHistoryCount = 0; // element count (not move count)

    // Queued CPU physical prompt move (if active at snapshot time).
    bool cpuMoveValid = false;
    uint8_t cpuMoveFrom = 0;
    uint8_t cpuMoveTo = 0;
    bool cpuMoveCaptureSquareValid = false;
    uint8_t cpuMoveCaptureSquare = 0;
    char cpuMovePromotionSuffixLower = '\0';
    bool cpuMoveDuckDestinationValid = false;
    uint8_t cpuMoveDuckDestination = 0;

    fxe::FixedString<128> interactionState;
    fxe::FixedString<32> variantState;
    fxe::FixedString<(UNDO_HISTORY_MOVE_CAPACITY * 4U) + 1U>
        duckUndoState;

    bool clockStatePresent = false;
    GameClockPersistenceState clockState{};
    GameHistoryPersistenceState gameHistory{};
};

enum class GameHistoryClearPolicy : uint8_t { Remove, Preserve };
// Bounded scratch for callers compacting retained settings/artwork/history.
// The runtime coordinator prepares a complete candidate document instead.
inline constexpr size_t GAME_PERSISTENCE_COMPACT_CAPACITY = 3072U;
void ClearGameState(JsonDocument& doc,
                    GameHistoryClearPolicy historyPolicy = GameHistoryClearPolicy::Remove);
void ClearUndoState(JsonDocument& doc);

uint64_t SquareArrayToMask64(const uint8_t* squares, uint8_t count);
uint8_t Mask64ToSquareArray(uint64_t mask,
                            uint8_t* outSquares,
                            uint8_t outCapacity);

bool WriteBitmask64Hex(JsonDocument& doc, const char* key, uint64_t mask);
bool ReadBitmask64HexOrLegacyArray(const JsonDocument& doc,
                                   const char* key,
                                   uint64_t& outMask);
bool CompactDocRemovingGameState(
    JsonDocument& doc,
    GameHistoryClearPolicy historyPolicy = GameHistoryClearPolicy::Remove);
bool CompactDocRemovingGameState(
    JsonDocument& doc, char* scratch, size_t scratchCapacity,
    GameHistoryClearPolicy historyPolicy = GameHistoryClearPolicy::Remove);

bool WriteSquareIndexArray(JsonDocument& doc,
                           const char* key,
                           const uint8_t* squares,
                           uint8_t count);
bool ReadSquareIndexArray(const JsonDocument& doc,
                          const char* key,
                          uint8_t* outSquares,
                          uint8_t outCapacity,
                          uint8_t& outCount);

bool WriteUndoHistory(JsonDocument& doc,
                      const uint8_t* fields,
                      uint16_t fieldCount);
bool ReadUndoHistory(const JsonDocument& doc,
                     uint8_t* outFields,
                     uint16_t outCapacity,
                     uint16_t& outFieldCount);

bool WriteGameClockPersistenceState(
    JsonDocument& doc,
    const GameClockPersistenceState& state);
bool ReadGameClockPersistenceState(
    const JsonDocument& doc,
    GameClockPersistenceState& out);

// Prepare a complete replacement in reusable RAM; current is never mutated.
// candidate must match the destination SettingsStore capacity.
bool PrepareGamePersistenceDocument(JsonDocument& candidate,
                                    const JsonDocument& current,
                                    const GamePersistenceState& state);
bool WriteGamePersistenceState(JsonDocument& doc,
                               const GamePersistenceState& state);
bool ReadGamePersistenceState(const JsonDocument& doc,
                              GamePersistenceState& out);

} // namespace modules
