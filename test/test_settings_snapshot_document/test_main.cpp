#include <cstdio>
#include <string>

#include "foxie_embedded/test_support/test_assert.hpp"
#include "modules/game_persistence.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/settings_store.hpp"

int main() {
    fxe::test_support::TestResults results("SETTINGS_SNAPSHOT_DOCUMENT");
    results.Start();
    {
        StaticJsonDocument<fxe::SETTINGS_STORE_JSON_CAPACITY> current;
        StaticJsonDocument<fxe::SETTINGS_STORE_JSON_CAPACITY> candidate;
        modules::GamePersistenceState blank;
        blank.fen.Assign("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
        current[modules::KEY_GAME_CPU_MOVE]["from"] = 52U;
        current[modules::KEY_GAME_CASTLING_ROOK_PROMPT] = 63U;
        current[modules::KEY_GAME_CLOCK_TERMINAL_V1] = "retired";
        current[modules::KEY_GAME_VARIANT_V1] = "01021800000000ffff00";
        current[modules::KEY_GAME_HISTORY_V1]["quarantined"] = true;
        blank.gameHistory.valid = false;
        results.Check(modules::PrepareGamePersistenceDocument(candidate, current, blank) &&
                          !candidate.containsKey(modules::KEY_GAME_CPU_MOVE) &&
                          !candidate.containsKey(modules::KEY_GAME_CASTLING_ROOK_PROMPT) &&
                          !candidate.containsKey(modules::KEY_GAME_CLOCK_TERMINAL_V1) &&
                          !candidate.containsKey(modules::KEY_GAME_VARIANT_V1) &&
                          candidate[modules::KEY_GAME_HISTORY_V1]["quarantined"].as<bool>(),
                      "replacement clears obsolete game keys while preserving quarantined history");
        blank.gameHistory.valid = true;
        results.Check(modules::PrepareGamePersistenceDocument(candidate, current, blank) &&
                          !candidate.containsKey(modules::KEY_GAME_HISTORY_V1),
                      "valid new history authority replaces the old archive metadata");
        current.clear();
        modules::GamePersistenceState prior;
        results.Check(prior.fen.Assign("4k3/8/8/8/8/8/8/4K3 w - - 0 1"), "prior recovery FEN fits");
        prior.undoHistoryCount = modules::UNDO_HISTORY_FIELDS_PER_MOVE;
        prior.undoHistory[5] = 52U; prior.undoHistory[6] = 36U;
        results.Check(modules::WriteGamePersistenceState(current, prior), "prior game and undo form a complete document");
        current["BRIGHTNESS"] = 8U;
        current["RETAINED_STRING"] = std::string(7500U, 'p');
        current["RETAINED_BOOL"] = true;
        current["RETAINED_REAL"] = 1.25;
        current["RETAINED_NULL"] = nullptr;
        current["RETAINED_ARRAY"][0] = 42;
        current["RETAINED_OBJECT"]["name"] = std::string("owned-value");
        std::string previous; serializeJson(current, previous);
        results.Check(!current.overflowed(), "failure fixture starts with complete live RAM state");
        modules::GamePersistenceState oversized = prior;
        oversized.undoHistoryCount = modules::UNDO_HISTORY_FIELDS_PER_MOVE * modules::UNDO_HISTORY_MOVE_CAPACITY;
        results.Check(!modules::PrepareGamePersistenceDocument(candidate, current, oversized), "actual-capacity candidate rejects oversized game snapshot");
        std::string afterFailure; serializeJson(current, afterFailure);
        modules::GamePersistenceState recovered;
        results.Check(previous == afterFailure && current["BRIGHTNESS"].as<unsigned>() == 8U &&
                  modules::ReadGamePersistenceState(current, recovered) && recovered.undoHistoryCount == prior.undoHistoryCount,
              "failed preparation preserves newest preference and exact prior game/undo document");
        results.Check(modules::PrepareGamePersistenceDocument(candidate, current, prior), "bounded replacement succeeds after rejected oversized snapshot");
        current.clear();
        (void)current.set(candidate);
        candidate.clear();
                results.Check(current["RETAINED_BOOL"].as<bool>() && current["RETAINED_NULL"].isNull() &&
                  current["RETAINED_REAL"].as<double>() == 1.25 && current["BRIGHTNESS"].as<unsigned>() == 8U &&
                  modules::ReadGamePersistenceState(current, recovered) && recovered.undoHistoryCount == prior.undoHistoryCount && current["RETAINED_ARRAY"][0].as<int>() == 42 &&
                  current["RETAINED_OBJECT"]["name"].as<std::string>() == "owned-value",
              "published document owns all retained types after candidate reuse");
        current.clear();
        results.Check(modules::WriteGamePersistenceState(current, prior), "file-bound fixture has prior coherent game");
        current["BRIGHTNESS"] = 8U;
        current["RETAINED_STRING"] = std::string(8300U, 'f');
        std::string oversizedFile; serializeJson(current, oversizedFile);
        results.Check(!current.overflowed() &&
                          !modules::PrepareGamePersistenceDocument(candidate, current, prior) &&
                          !candidate.overflowed(),
                      "serialized file limit rejects a candidate that still fits RAM");
        std::string afterFileFailure; serializeJson(current, afterFileFailure);
        results.Check(oversizedFile == afterFileFailure,
                      "file-bound rejection also preserves prior game and newest settings");
        std::printf("ATOMIC_RESULT capacity=%zu exact_prior_preserved=%u committed_mem=%zu coordinator_bytes=%zu\n",
                    current.capacity(), previous == afterFailure, current.memoryUsage(), sizeof(modules::GamePersistenceCoordinator));
    }
    return results.Finish();
}
