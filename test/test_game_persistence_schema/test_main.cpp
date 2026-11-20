#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

#include "modules/game_persistence.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[GAME_PERSISTENCE_SCHEMA] start");
    std::printf("[GAME_PERSISTENCE_SCHEMA] state_bytes=%zu\n", sizeof(modules::GamePersistenceState));

    {
        StaticJsonDocument<2048> doc;

        modules::GamePersistenceState s;
        Check(s.fen.Assign("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),
              "fen Assign fits");
        s.movedSquares[0] = 0;
        s.movedSquares[1] = 7;
        s.movedSquares[2] = 63;
        s.movedSquaresCount = 3;

        Check(s.undoBaseFen.Assign("base"), "undoBaseFen Assign fits");
        s.undoBaseMovedSquares[0] = 1;
        s.undoBaseMovedSquares[1] = 2;
        s.undoBaseMovedSquaresCount = 2;

        // Two moves worth of fields (2 * 7).
        s.undoHistoryCount = 14;
        for (uint16_t i = 0; i < s.undoHistoryCount; ++i) {
            s.undoHistory[i] = static_cast<uint8_t>(i);
        }

        s.cpuMoveValid = true;
        s.cpuMoveFrom = 12;
        s.cpuMoveTo = 28;
        s.cpuMoveCaptureSquareValid = true;
        s.cpuMoveCaptureSquare = 28;
        s.cpuMovePromotionSuffixLower = 'q';
        s.cpuMoveDuckDestinationValid = true;
        s.cpuMoveDuckDestination = 35;

        Check(modules::WriteGamePersistenceState(doc, s), "WriteGamePersistenceState ok");
        Check(doc.containsKey(modules::KEY_GAME_FEN), "doc has GAME_FEN");
        Check(doc.containsKey(modules::KEY_GAME_MOVED_SQUARES), "doc has GAME_MOVED_SQUARES");
        Check(doc.containsKey(modules::KEY_GAME_UNDO_BASE_FEN), "doc has GAME_UNDO_BASE_FEN");
        Check(doc.containsKey(modules::KEY_GAME_UNDO_BASE_MOVED_SQUARES), "doc has GAME_UNDO_BASE_MOVED_SQUARES");
        Check(doc.containsKey(modules::KEY_GAME_UNDO_HISTORY), "doc has GAME_UNDO_HISTORY");
        Check(doc.containsKey(modules::KEY_GAME_CPU_MOVE), "doc has GAME_CPU_MOVE");

        // Moved squares are now stored as a 16-char bitmask hex string.
        const JsonVariantConst movedV = doc[modules::KEY_GAME_MOVED_SQUARES];
        Check(movedV.is<const char*>(), "moved squares is a hex string");
        if (movedV.is<const char*>()) {
            const char* movedHex = movedV.as<const char*>();
            Check(movedHex != nullptr && std::strlen(movedHex) == 16,
                  "moved squares hex is 16 chars");
        }

        // Undo history is persisted in a compact form to avoid SettingsStore JSON
        // capacity issues. Accept either:
        // - current: hex string (2 chars per byte)
        // - legacy: JSON int array
        const JsonVariantConst histV = doc[modules::KEY_GAME_UNDO_HISTORY];
        if (histV.is<const char*>()) {
            const char* sHex = histV.as<const char*>();
            Check(sHex != nullptr, "undo history hex present");
            if (sHex != nullptr) {
                Check(std::strlen(sHex) == 28, "undo history hex size=28 chars (14 bytes)");
            }
        } else {
            const JsonArrayConst hist = histV.as<JsonArrayConst>();
            Check(hist.size() == 14, "undo history size=14 fields");
        }

        modules::GamePersistenceState out;
        Check(modules::ReadGamePersistenceState(doc, out), "ReadGamePersistenceState ok");
        Check(out.fen.Equals(s.fen.View()), "fen round-trips");
        Check(out.movedSquaresCount == 3, "movedSquaresCount round-trips");
        Check(out.movedSquares[0] == 0 && out.movedSquares[1] == 7 && out.movedSquares[2] == 63,
              "movedSquares round-trip values");

        Check(out.undoBaseFen.Equals("base"), "undoBaseFen round-trips");
        Check(out.undoBaseMovedSquaresCount == 2, "undoBaseMovedSquaresCount round-trips");
        Check(out.undoBaseMovedSquares[0] == 1 && out.undoBaseMovedSquares[1] == 2,
              "undoBaseMovedSquares round-trip values");

        Check(out.undoHistoryCount == 14, "undoHistoryCount round-trips");
        Check(out.undoHistory[0] == 0 && out.undoHistory[13] == 13, "undoHistory values round-trip");

        Check(out.cpuMoveValid, "cpu move valid round-trips");
        Check(out.cpuMoveFrom == 12 && out.cpuMoveTo == 28, "cpu move from/to round-trip");
        Check(out.cpuMoveCaptureSquareValid && out.cpuMoveCaptureSquare == 28,
              "cpu move capture round-trips");
        Check(out.cpuMovePromotionSuffixLower == 'q', "cpu move promo suffix round-trips");
        Check(out.cpuMoveDuckDestinationValid &&
                  out.cpuMoveDuckDestination == 35U,
              "cpu Duck destination round-trips");
    }

    {
        // Invalid square index should be rejected.
        StaticJsonDocument<256> doc;
        uint8_t squares[2] = {0, 64};
        Check(!modules::WriteSquareIndexArray(doc, modules::KEY_GAME_MOVED_SQUARES, squares, 2),
              "WriteSquareIndexArray rejects >=64");
    }

    {
        // Legacy int array format should be readable by the new bitmask reader.
        StaticJsonDocument<512> doc;
        JsonArray arr = doc[modules::KEY_GAME_MOVED_SQUARES].to<JsonArray>();
        (void)arr.add(0U);
        (void)arr.add(7U);
        (void)arr.add(63U);

        uint64_t mask = 0;
        Check(modules::ReadBitmask64HexOrLegacyArray(doc, modules::KEY_GAME_MOVED_SQUARES, mask),
              "ReadBitmask64HexOrLegacyArray reads legacy int array");
        Check(mask == ((1ULL << 0) | (1ULL << 7) | (1ULL << 63)),
              "legacy int array mask matches expected bits");
    }

    {
        // WriteBitmask64Hex + ReadBitmask64HexOrLegacyArray round-trip.
        StaticJsonDocument<512> doc;
        const uint64_t original = (1ULL << 3) | (1ULL << 17) | (1ULL << 62);
        Check(modules::WriteBitmask64Hex(doc, "TEST_MASK", original),
              "WriteBitmask64Hex ok");

        uint64_t readBack = 0;
        Check(modules::ReadBitmask64HexOrLegacyArray(doc, "TEST_MASK", readBack),
              "ReadBitmask64HexOrLegacyArray ok");
        Check(readBack == original, "bitmask round-trips correctly");
    }

    {
        // CompactDocRemovingGameState should reclaim pool memory.
        StaticJsonDocument<2048> doc;
        doc["BRIGHTNESS"] = 42U;
        doc[modules::KEY_GAME_FEN] = "test-fen";
        doc[modules::KEY_GAME_UNDO_HISTORY] = "aabbccdd";
        Check(modules::CompactDocRemovingGameState(doc),
              "CompactDocRemovingGameState succeeds");
        Check(!doc.containsKey(modules::KEY_GAME_FEN),
              "game keys removed after compact");
        Check(!doc.containsKey(modules::KEY_GAME_UNDO_HISTORY),
              "undo keys removed after compact");
        Check((doc["BRIGHTNESS"] | 0U) == 42U,
              "non-game settings survive compaction");
    }

    {
        // Undo history must be multiple-of-7 fields.
        StaticJsonDocument<256> doc;
        uint8_t fields[8] = {};
        Check(!modules::WriteUndoHistory(doc, fields, 8), "WriteUndoHistory rejects non-multiple-of-7");

        JsonArray arr = doc[modules::KEY_GAME_UNDO_HISTORY].to<JsonArray>();
        arr.clear();
        (void)arr.add(1);
        (void)arr.add(2);

        uint8_t outFields[16] = {};
        uint16_t outCount = 0;
        Check(!modules::ReadUndoHistory(doc, outFields, sizeof(outFields), outCount),
              "ReadUndoHistory rejects non-multiple-of-7");
    }

    {
        // Clear helpers should remove their respective keys.
        StaticJsonDocument<256> doc;
        doc[modules::KEY_GAME_FEN] = "x";
        doc[modules::KEY_GAME_MOVED_SQUARES].to<JsonArray>().add(1);
        doc[modules::KEY_GAME_UNDO_BASE_FEN] = "y";
        doc[modules::KEY_GAME_UNDO_HISTORY].to<JsonArray>().add(0);
        doc[modules::KEY_GAME_CPU_MOVE]["from"] = 12U;

        modules::ClearGameState(doc);
        Check(!doc.containsKey(modules::KEY_GAME_FEN), "ClearGameState removes GAME_FEN");
        Check(!doc.containsKey(modules::KEY_GAME_MOVED_SQUARES), "ClearGameState removes GAME_MOVED_SQUARES");
        Check(!doc.containsKey(modules::KEY_GAME_CPU_MOVE), "ClearGameState removes GAME_CPU_MOVE");

        modules::ClearUndoState(doc);
        Check(!doc.containsKey(modules::KEY_GAME_UNDO_BASE_FEN), "ClearUndoState removes GAME_UNDO_BASE_FEN");
        Check(!doc.containsKey(modules::KEY_GAME_UNDO_HISTORY), "ClearUndoState removes GAME_UNDO_HISTORY");
    }

    {
        StaticJsonDocument<2048> doc;
        modules::GamePersistenceState state;
        state.fen.Assign("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        state.hasAcceptedMove = true;
        Check(modules::WriteGamePersistenceState(doc, state), "accepted-play provenance writes");
        modules::GamePersistenceState read;
        Check(modules::ReadGamePersistenceState(doc, read) && read.hasAcceptedMove,
              "accepted-play provenance round-trips independently of zero history");
        for (const char* malformed : {"1", "\"true\"", "null", "[]"}) {
            StaticJsonDocument<32> value;
            Check(!deserializeJson(value, malformed), "provenance malformed fixture parses");
            doc[modules::KEY_GAME_ACCEPTED_PLAY].set(value.as<JsonVariantConst>());
            Check(!modules::ReadGamePersistenceState(doc, read), "non-boolean provenance is rejected");
        }
        doc.remove(modules::KEY_GAME_ACCEPTED_PLAY);
        Check(modules::ReadGamePersistenceState(doc, read) && !read.hasAcceptedMove,
              "legacy absence leaves provenance inference with the coordinator");
        doc[modules::KEY_GAME_ACCEPTED_PLAY] = true;
        state.hasAcceptedMove = false;
        StaticJsonDocument<2048> candidate;
        Check(modules::PrepareGamePersistenceDocument(candidate, doc, state), "explicit new-game document replaces provenance");
        doc.set(candidate);
        Check(!doc.containsKey(modules::KEY_GAME_ACCEPTED_PLAY), "false provenance has no persisted field");
        doc[modules::KEY_GAME_ACCEPTED_PLAY] = true;
        modules::ClearGameState(doc);
        Check(!doc.containsKey(modules::KEY_GAME_ACCEPTED_PLAY), "game clear removes provenance");
    }

    for (const char* key : {modules::KEY_GAME_INTERACTION_V1,
                            modules::KEY_GAME_VARIANT_V1,
                            modules::KEY_GAME_DUCK_UNDO_V1}) {
        StaticJsonDocument<512U> doc;
        modules::GamePersistenceState state;
        Check(modules::ReadGamePersistenceState(doc, state),
              "missing optional durable record retains legacy interpretation");
        doc[key] = "";
        Check(modules::ReadGamePersistenceState(doc, state),
              "empty string durable placeholder retains its existing interpretation");
        for (const char* malformed : {"null", "true", "1", "[]", "{}"}) {
            StaticJsonDocument<64U> value;
            Check(!deserializeJson(value, malformed), "malformed durable field fixture parses");
            Check(doc[key].set(value.as<JsonVariantConst>()), "malformed durable field fits");
            Check(!modules::ReadGamePersistenceState(doc, state),
                  "present non-string durable record fails extraction instead of becoming absent");
        }
    }

    if (g_failures == 0) {
        std::puts("[GAME_PERSISTENCE_SCHEMA] ok");
        return 0;
    }

    std::printf("[GAME_PERSISTENCE_SCHEMA] failures=%d\n", g_failures);
    return 1;
}
