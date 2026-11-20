#include "modules/game_persistence.hpp"

#include <cstring>

#include "modules/settings_store.hpp"

namespace modules {

void ClearGameState(JsonDocument& doc, const GameHistoryClearPolicy historyPolicy) {
    (void)doc.remove(KEY_GAME_FEN);
    (void)doc.remove(KEY_GAME_ACCEPTED_PLAY);
    (void)doc.remove(KEY_GAME_MOVED_SQUARES);
    (void)doc.remove(KEY_GAME_CPU_MOVE);
    (void)doc.remove(KEY_GAME_CASTLING_ROOK_PROMPT);
    (void)doc.remove(KEY_GAME_INTERACTION_V1);
    (void)doc.remove(KEY_GAME_VARIANT_V1);
    (void)doc.remove(KEY_GAME_CLOCK_V1);
    (void)doc.remove(KEY_GAME_CLOCK_TERMINAL_V1);
    if (historyPolicy == GameHistoryClearPolicy::Remove) {
        (void)doc.remove(KEY_GAME_HISTORY_V1);
    }
}

void ClearUndoState(JsonDocument& doc) {
    (void)doc.remove(KEY_GAME_UNDO_BASE_FEN);
    (void)doc.remove(KEY_GAME_UNDO_BASE_MOVED_SQUARES);
    (void)doc.remove(KEY_GAME_UNDO_HISTORY);
    (void)doc.remove(KEY_GAME_DUCK_UNDO_V1);
}

// Compact the JSON document by removing game state keys, serializing
// remaining settings to a temporary buffer, clearing the doc, and
// re-parsing. This reclaims dead pool memory accumulated by ArduinoJson's
// monotonic allocator during repeated key overwrites.
//
// An oversized remainder returns false without clearing retained settings or
// preserved archive metadata. Reparse failure can leave the document empty or
// partial; callers retain the existing fallback for that failure.
bool CompactDocRemovingGameState(
    JsonDocument& doc, const GameHistoryClearPolicy historyPolicy) {
    char compactBuf[1024U];
    return CompactDocRemovingGameState(doc, compactBuf, sizeof(compactBuf), historyPolicy);
}

bool CompactDocRemovingGameState(
    JsonDocument& doc, char* const scratch, const size_t scratchCapacity,
    const GameHistoryClearPolicy historyPolicy) {
    ClearGameState(doc, historyPolicy);
    ClearUndoState(doc);

    const size_t jsonLen = measureJson(doc);

    if (scratch == nullptr || jsonLen >= scratchCapacity) {
        return false;
    }

    (void)serializeJson(doc, scratch, scratchCapacity);

    doc.clear();

    if (jsonLen == 0) {
        return true;
    }

    // Use const char* overload so ArduinoJson copies strings into the doc
    // pool rather than referencing the local buffer.
    const char* src = scratch;
    return deserializeJson(doc, src) == DeserializationError::Ok;
}

namespace {

bool IsReplacedGameKey(const char* key, const GameHistoryClearPolicy historyPolicy) {
    return std::strcmp(key, KEY_GAME_FEN) == 0 ||
        std::strcmp(key, KEY_GAME_ACCEPTED_PLAY) == 0 ||
        std::strcmp(key, KEY_GAME_MOVED_SQUARES) == 0 ||
        std::strcmp(key, KEY_GAME_CPU_MOVE) == 0 ||
        std::strcmp(key, KEY_GAME_CASTLING_ROOK_PROMPT) == 0 ||
        std::strcmp(key, KEY_GAME_INTERACTION_V1) == 0 ||
        std::strcmp(key, KEY_GAME_VARIANT_V1) == 0 ||
        std::strcmp(key, KEY_GAME_CLOCK_V1) == 0 ||
        std::strcmp(key, KEY_GAME_CLOCK_TERMINAL_V1) == 0 ||
        std::strcmp(key, KEY_GAME_UNDO_BASE_FEN) == 0 ||
        std::strcmp(key, KEY_GAME_UNDO_BASE_MOVED_SQUARES) == 0 ||
        std::strcmp(key, KEY_GAME_UNDO_HISTORY) == 0 ||
        std::strcmp(key, KEY_GAME_DUCK_UNDO_V1) == 0 ||
        (historyPolicy == GameHistoryClearPolicy::Remove &&
         std::strcmp(key, KEY_GAME_HISTORY_V1) == 0);
}

} // namespace

bool PrepareGamePersistenceDocument(JsonDocument& candidate,
                                    const JsonDocument& current,
                                    const GamePersistenceState& state) {
    candidate.clear();
    const auto historyPolicy = state.gameHistory.valid
        ? GameHistoryClearPolicy::Remove : GameHistoryClearPolicy::Preserve;
    for (const JsonPairConst pair : current.as<JsonObjectConst>()) {
        if (!IsReplacedGameKey(pair.key().c_str(), historyPolicy) &&
            !candidate[pair.key()].set(pair.value())) {
            return false;
        }
    }
    return !candidate.overflowed() && WriteGamePersistenceState(candidate, state) &&
        measureJson(candidate) < fxe::SETTINGS_STORE_MAX_FILE_SIZE_BYTES;
}

bool WriteGamePersistenceState(JsonDocument& doc, const GamePersistenceState& s) {
    if (s.hasAcceptedMove) {
        if (!doc[KEY_GAME_ACCEPTED_PLAY].set(true)) return false;
    } else {
        (void)doc.remove(KEY_GAME_ACCEPTED_PLAY);
    }
    // Force copy: `s` often lives on the stack in persistence snapshots.
    if (!doc[KEY_GAME_FEN].set(const_cast<char*>(s.fen.CStr()))) {
        return false;
    }

    const uint64_t movedMask = SquareArrayToMask64(s.movedSquares, s.movedSquaresCount);
    if (!WriteBitmask64Hex(doc, KEY_GAME_MOVED_SQUARES, movedMask)) {
        return false;
    }

    if (s.undoBaseFen.Size() != 0) {
        // Force copy: `s` often lives on the stack in persistence snapshots.
        if (!doc[KEY_GAME_UNDO_BASE_FEN].set(const_cast<char*>(s.undoBaseFen.CStr()))) {
            return false;
        }
    } else {
        (void)doc.remove(KEY_GAME_UNDO_BASE_FEN);
    }

    const uint64_t baseMask = SquareArrayToMask64(s.undoBaseMovedSquares,
                                                  s.undoBaseMovedSquaresCount);
    if (!WriteBitmask64Hex(doc, KEY_GAME_UNDO_BASE_MOVED_SQUARES, baseMask)) {
        return false;
    }

    if (!WriteUndoHistory(doc, s.undoHistory, s.undoHistoryCount)) {
        return false;
    }

    if (s.cpuMoveValid) {
        JsonObject cpuMoveObj = doc[KEY_GAME_CPU_MOVE].to<JsonObject>();
        if (cpuMoveObj.isNull()) {
            return false;
        }

        cpuMoveObj["from"] = s.cpuMoveFrom;
        cpuMoveObj["to"] = s.cpuMoveTo;
        cpuMoveObj["capture_valid"] = s.cpuMoveCaptureSquareValid ? 1U : 0U;
        cpuMoveObj["capture_square"] = s.cpuMoveCaptureSquare;

        if (s.cpuMovePromotionSuffixLower != '\0') {
            char promo[2] = {s.cpuMovePromotionSuffixLower, '\0'};
            if (!cpuMoveObj["promo"].set(static_cast<char*>(promo))) {
                return false;
            }
        } else {
            (void)cpuMoveObj.remove("promo");
        }
        if (s.cpuMoveDuckDestinationValid) {
            cpuMoveObj["duck"] = s.cpuMoveDuckDestination;
        } else {
            (void)cpuMoveObj.remove("duck");
        }
    } else {
        (void)doc.remove(KEY_GAME_CPU_MOVE);
    }

    // Castling progress is persisted only as the complete durable interaction
    // plan. Remove the retired endpoint-only record from older snapshots.
    (void)doc.remove(KEY_GAME_CASTLING_ROOK_PROMPT);

    if (!s.interactionState.Empty()) {
        if (!doc[KEY_GAME_INTERACTION_V1].set(
                const_cast<char*>(s.interactionState.CStr()))) {
            return false;
        }
    } else {
        (void)doc.remove(KEY_GAME_INTERACTION_V1);
    }

    if (!s.variantState.Empty()) {
        if (!doc[KEY_GAME_VARIANT_V1].set(
                const_cast<char*>(s.variantState.CStr()))) {
            return false;
        }
    } else {
        (void)doc.remove(KEY_GAME_VARIANT_V1);
    }

    if (!s.duckUndoState.Empty()) {
        if (!doc[KEY_GAME_DUCK_UNDO_V1].set(
                const_cast<char*>(s.duckUndoState.CStr()))) {
            return false;
        }
    } else {
        (void)doc.remove(KEY_GAME_DUCK_UNDO_V1);
    }

    if (s.clockStatePresent) {
        if (!WriteGameClockPersistenceState(doc, s.clockState)) {
            return false;
        }
    } else {
        (void)doc.remove(KEY_GAME_CLOCK_V1);
        (void)doc.remove(KEY_GAME_CLOCK_TERMINAL_V1);
    }

    if (!WriteGameHistoryPersistenceState(doc, s.gameHistory)) return false;

    if (doc.overflowed()) {
        return false;
    }
    return true;
}

bool ReadGamePersistenceState(const JsonDocument& doc, GamePersistenceState& out) {
    out = GamePersistenceState{};
    if (doc.containsKey(KEY_GAME_ACCEPTED_PLAY)) {
        const JsonVariantConst accepted = doc[KEY_GAME_ACCEPTED_PLAY];
        if (!accepted.is<bool>()) return false;
        out.hasAcceptedMove = accepted.as<bool>();
    }
    if (!ReadGameHistoryPersistenceState(doc, out.gameHistory)) {
        out.gameHistory = {};
        out.gameHistory.valid = false;
    }

    if (doc.containsKey(KEY_GAME_FEN)) {
        const char* fen = doc[KEY_GAME_FEN] | "";
        if (!out.fen.Assign(fen)) {
            return false;
        }
    }

    {
        uint64_t movedMask = 0;
        if (ReadBitmask64HexOrLegacyArray(doc, KEY_GAME_MOVED_SQUARES, movedMask)) {
            out.movedSquaresCount = Mask64ToSquareArray(
                movedMask, out.movedSquares, static_cast<uint8_t>(sizeof(out.movedSquares)));
        } else {
            // Be tolerant of partially written/corrupt data.
            // Preserve the core game FEN even if auxiliary fields are invalid.
            out.movedSquaresCount = 0;
        }
    }

    if (doc.containsKey(KEY_GAME_UNDO_BASE_FEN)) {
        const char* fen = doc[KEY_GAME_UNDO_BASE_FEN] | "";
        if (!out.undoBaseFen.Assign(fen)) {
            return false;
        }
    }

    {
        uint64_t baseMask = 0;
        if (ReadBitmask64HexOrLegacyArray(doc, KEY_GAME_UNDO_BASE_MOVED_SQUARES, baseMask)) {
            out.undoBaseMovedSquaresCount = Mask64ToSquareArray(
                baseMask, out.undoBaseMovedSquares,
                static_cast<uint8_t>(sizeof(out.undoBaseMovedSquares)));
        } else {
            // Lossy recovery: treat invalid base-moved metadata as absent.
            out.undoBaseMovedSquaresCount = 0;
        }
    }

    if (!ReadUndoHistory(doc, out.undoHistory, static_cast<uint16_t>(sizeof(out.undoHistory)), out.undoHistoryCount)) {
        // Lossy recovery: preserve FEN and clear undo history.
        out.undoHistoryCount = 0;
    }

    if (doc.containsKey(KEY_GAME_CPU_MOVE)) {
        const JsonVariantConst v = doc[KEY_GAME_CPU_MOVE];
        if (v.is<JsonObjectConst>()) {
            const JsonObjectConst cpuMoveObj = v.as<JsonObjectConst>();

            const uint32_t from = cpuMoveObj["from"] | 255U;
            const uint32_t to = cpuMoveObj["to"] | 255U;
            const bool captureValid = (cpuMoveObj["capture_valid"] | 0U) != 0U;
            const uint32_t captureSquare = cpuMoveObj["capture_square"] | 255U;
            const bool hasDuckDestination =
                cpuMoveObj.containsKey("duck");
            const uint32_t duckDestination =
                cpuMoveObj["duck"] | 255U;

            bool valid = (from < 64U) && (to < 64U);
            if (captureValid && captureSquare >= 64U) {
                valid = false;
            }
            if (hasDuckDestination && duckDestination >= 64U) {
                valid = false;
            }

            if (valid) {
                out.cpuMoveValid = true;
                out.cpuMoveFrom = static_cast<uint8_t>(from);
                out.cpuMoveTo = static_cast<uint8_t>(to);
                out.cpuMoveCaptureSquareValid = captureValid;
                out.cpuMoveCaptureSquare = captureValid ? static_cast<uint8_t>(captureSquare) : 0U;

                out.cpuMovePromotionSuffixLower = '\0';
                if (cpuMoveObj["promo"].is<const char*>()) {
                    const char* promo = cpuMoveObj["promo"].as<const char*>();
                    if (promo != nullptr && promo[0] != '\0') {
                        out.cpuMovePromotionSuffixLower = promo[0];
                    }
                }
                out.cpuMoveDuckDestinationValid =
                    hasDuckDestination;
                out.cpuMoveDuckDestination = hasDuckDestination
                    ? static_cast<uint8_t>(duckDestination)
                    : 0U;
            }
        }
    }

    if (doc.containsKey(KEY_GAME_INTERACTION_V1)) {
        const JsonVariantConst encoded = doc[KEY_GAME_INTERACTION_V1];
        if (!encoded.is<const char*>() ||
            !out.interactionState.Assign(encoded.as<const char*>())) {
            return false;
        }
    }

    if (doc.containsKey(KEY_GAME_VARIANT_V1)) {
        const JsonVariantConst encoded = doc[KEY_GAME_VARIANT_V1];
        if (!encoded.is<const char*>() ||
            !out.variantState.Assign(encoded.as<const char*>())) {
            return false;
        }
    }

    if (doc.containsKey(KEY_GAME_DUCK_UNDO_V1)) {
        const JsonVariantConst encoded = doc[KEY_GAME_DUCK_UNDO_V1];
        if (!encoded.is<const char*>() ||
            !out.duckUndoState.Assign(encoded.as<const char*>())) {
            return false;
        }
    }

    if (doc.containsKey(KEY_GAME_CLOCK_V1)) {
        if (!ReadGameClockPersistenceState(doc, out.clockState)) {
            return false;
        }
        out.clockStatePresent = true;
    }

    if (out.gameHistory.present && !out.gameHistory.IsCaptureExhausted() &&
        out.gameHistory.currentPly <
        out.undoHistoryCount / UNDO_HISTORY_FIELDS_PER_MOVE) {
        out.gameHistory = {};
        out.gameHistory.valid = false;
    }
    return true;
}

} // namespace modules
