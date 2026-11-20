#include "modules/first_player_migration.hpp"

namespace modules::first_player_migration {

uint32_t ReadFirstPlayer1To2FromDoc(const JsonDocument& doc) {
    if (doc.containsKey("GAME_FIRST_PLAYER")) {
        const JsonVariantConst value = doc["GAME_FIRST_PLAYER"];
        if (value.is<uint32_t>() || value.is<uint64_t>() || value.is<int64_t>()) {
            const uint32_t numeric = value | 1U;
            return (numeric == 2U) ? 2U : 1U;
        }
        return 1U;
    }

    if (doc.containsKey("GAME_P1_PLAYS_BLACK")) {
        return (doc["GAME_P1_PLAYS_BLACK"] | 0U) != 0U ? 2U : 1U;
    }
    if (doc.containsKey("p1_plays_black")) {
        return (doc["p1_plays_black"] | 0U) != 0U ? 2U : 1U;
    }

    return 1U;
}

bool ReadFirstPlayerIsP2FromDoc(const JsonDocument& doc) {
    return ReadFirstPlayer1To2FromDoc(doc) == 2U;
}

bool CanonicalizeAndCleanup(JsonDocument& doc) {
    bool mutated = false;

    const uint32_t canonical = ReadFirstPlayer1To2FromDoc(doc);
    bool needsCanonicalWrite = !doc.containsKey("GAME_FIRST_PLAYER");

    if (!needsCanonicalWrite &&
        (doc["GAME_FIRST_PLAYER"].is<uint32_t>() || doc["GAME_FIRST_PLAYER"].is<uint64_t>() ||
         doc["GAME_FIRST_PLAYER"].is<int64_t>())) {
        const uint32_t existing = doc["GAME_FIRST_PLAYER"] | 1U;
        needsCanonicalWrite = (existing != 1U && existing != 2U);
    }

    if (!needsCanonicalWrite && !doc["GAME_FIRST_PLAYER"].is<uint32_t>() &&
        !doc["GAME_FIRST_PLAYER"].is<uint64_t>() && !doc["GAME_FIRST_PLAYER"].is<int64_t>()) {
        needsCanonicalWrite = true;
    }

    if (needsCanonicalWrite) {
        doc["GAME_FIRST_PLAYER"] = canonical;
        mutated = true;
    }

    if (doc.containsKey("GAME_P1_PLAYS_BLACK")) {
        (void)doc.remove("GAME_P1_PLAYS_BLACK");
        mutated = true;
    }
    if (doc.containsKey("p1_plays_black")) {
        (void)doc.remove("p1_plays_black");
        mutated = true;
    }

    return mutated;
}

} // namespace modules::first_player_migration
