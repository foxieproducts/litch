#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "../../src/settings_store_internal.hpp"

namespace {

std::string ReadFileToString(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        return {};
    }

    std::string out;
    char buf[1024];
    while (true) {
        const size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n > 0) {
            out.append(buf, n);
        }
        if (n < sizeof(buf)) {
            break;
        }
    }

    (void)std::fclose(f);
    return out;
}

fxe::test_support::TestResults* g_results = nullptr;

void Check(const bool cond, const char* msg) {
    if (!g_results) {
        return;
    }

    g_results->Check(cond, msg);
}

void PersistAfterUserAction(fxe::SettingsStore& store,
                            const fxe::SettingsStoreConfig& cfg) {
    const auto before = ReadFileToString(cfg.path);
    fxe::ManualMillis::AdvanceMs(cfg.deferredSaveDelayMs + cfg.mustSaveDelayMs);
    store.Update();
    Check(store.GetSaveCount() == 0U && ReadFileToString(cfg.path) == before,
          "boot recovery and idle do not write the selected primary");
    store.Doc()["TEST_USER_ACTION"] = 1U;
    store.MarkMustSaveDirty();
    fxe::ManualMillis::AdvanceMs(cfg.mustSaveDelayMs);
    store.Update();
}

struct PartitionFixture {
    uint8_t bytes[160] = {};
    size_t failAt = static_cast<size_t>(-1);
};

bool ReadPartitionFixture(const void* const context, const size_t offset,
                          uint8_t* const out, const size_t size) {
    const auto& fixture = *static_cast<const PartitionFixture*>(context);
    if (offset >= fixture.failAt || size > sizeof(fixture.bytes) - offset) {
        return false;
    }
    std::memcpy(out, fixture.bytes + offset, size);
    return true;
}

void CheckPartitionErasureClassification() {
    PartitionFixture fixture;
    std::memset(fixture.bytes, 0xFF, sizeof(fixture.bytes));
    uint8_t scratch[17] = {};
    Check(fxe::ClassifyPartitionErasure(
              sizeof(fixture.bytes), ReadPartitionFixture, &fixture,
              scratch, sizeof(scratch)) == fxe::PartitionErasure::Erased,
          "complete erased partition authorizes initialization");

    fixture.bytes[96] = 0x42U;
    Check(fxe::ClassifyPartitionErasure(
              sizeof(fixture.bytes), ReadPartitionFixture, &fixture,
              scratch, sizeof(scratch)) ==
              fxe::PartitionErasure::ContainsData,
          "erased prefix cannot hide later recoverable partition data");

    fixture.bytes[96] = 0xFFU;
    fixture.failAt = 68U;
    Check(fxe::ClassifyPartitionErasure(
              sizeof(fixture.bytes), ReadPartitionFixture, &fixture,
              scratch, sizeof(scratch)) == fxe::PartitionErasure::ReadError,
          "partition read error never authorizes formatting");
}

void CheckRejectedPromotionSaveTotal() {
    char directory[] = "/tmp/litch-native-save-total-XXXXXX";
    const bool created = ::mkdtemp(directory) != nullptr;
    Check(created, "create rejected-promotion fixture");
    if (!created) return;
    for (const bool verify : {true, false}) {
        const std::string path = std::string(directory) + (verify ? "/verified.json" : "/unchecked.json");
        fxe::SettingsStoreConfig config; config.path = path.c_str(); config.verifyWrites = verify;
        fxe::SettingsStore store(config);
        store.Doc()["VALUE"] = 7U;
        Check(store.SaveNowBlocking(), "initial native promotion succeeds");
        const auto primary = ReadFileToString(path.c_str());
        const auto total = store.GetPersistedSaveTotal();
        std::filesystem::create_directory(path + ".bak");
        const std::string obstruction = path + ".bak/keep";
        std::FILE* file = std::fopen(obstruction.c_str(), "wb");
        Check(file != nullptr, "nonempty directory blocks native backup promotion");
        if (file != nullptr) { std::fputs("retained", file); std::fclose(file); }
        store.Doc()["VALUE"] = 9U;
        store.MarkDirty();
        Check(!store.SaveNowBlocking(), "native rename refusal rejects the save");
        Check(ReadFileToString(path.c_str()) == primary &&
                  ReadFileToString(obstruction.c_str()) == "retained" &&
                  std::filesystem::exists(path + ".tmp"),
              "native refusal retains primary, obstruction and completed staging");
        std::printf("NATIVE_PROMOTION_REFUSAL verify=%u before=%u after=%u\n",
                    unsigned(verify), total, store.GetPersistedSaveTotal());
        Check(store.GetPersistedSaveTotal() == total && store.GetSaveCount() == 1U,
              "rejected native promotion never advances the persisted save total");
        std::filesystem::remove_all(path + ".bak");
        Check(store.SaveNowBlocking(), "native promotion retries through retained staging");
        Check(store.GetPersistedSaveTotal() == total + 1U && store.GetSaveCount() == 2U &&
                  ReadFileToString((path + ".bak").c_str()) == primary &&
                  !std::filesystem::exists(path + ".tmp") &&
                  !std::filesystem::exists(path + ".retry"),
              "successful native retry counts once and preserves recovery authority");
    }
    std::filesystem::remove_all(directory);
}

} // namespace

#include "test_scratch_cleanup.hpp"
#include "test_persistence_guard.hpp"
#include "test_scoped_recovery.hpp"
#include "test_repair_pending.hpp"


// Independent sanitation must preserve intentional omissions in the selected game record.
static void CheckUnrelatedScrubPreservesGameOmissions() {
    char directory[] = "/tmp/litch-scrub-omission-XXXXXX";
    const bool created = ::mkdtemp(directory) != nullptr;
    Check(created, "create unrelated-scrub omission fixture");
    if (!created) return;
    const std::string path = std::string(directory) + "/settings.json";
    settings_store_scoped_recovery_test::WriteFile(path.c_str(), R"probe({"VS_CPU_LEVEL":0,"TIMER":0,"GAME_FIRST_PLAYER":1,"BOARD_ROTATE":false,"GAME_ACCEPTED_PLAY":true,"GAME_FEN":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","GAME_MOVED_SQUARES":"0000000000000000","GAME_UNDO_BASE_FEN":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","GAME_UNDO_BASE_MOVED_SQUARES":"0000000000000000","GAME_VARIANT_V1":"0100ff00000000ffff00","GAME_CLOCK_V1":[1,3,7,0,0,0,0,0,0,0,2],"GAME_HISTORY_V1":[1,"00000000000000000000000000000002","0000000000000001",0,0,0,2,0,0],"SETTINGS_SAVE_TOTAL":5,"UNRELATED_TEXT":"bad\u0000text"})probe");
    settings_store_scoped_recovery_test::WriteFile((path + ".bak").c_str(), R"probe({"VS_CPU_LEVEL":0,"TIMER":0,"GAME_FIRST_PLAYER":1,"BOARD_ROTATE":false,"GAME_ACCEPTED_PLAY":true,"GAME_FEN":"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1","GAME_MOVED_SQUARES":"0010001010001000","GAME_UNDO_BASE_FEN":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","GAME_UNDO_BASE_MOVED_SQUARES":"0000000000000000","GAME_UNDO_HISTORY":"005000ff003424","GAME_VARIANT_V1":"0101ff00000000ffff00","GAME_CLOCK_V1":[1,3,6,0,0,0,0,1,0,0,2],"GAME_HISTORY_V1":[1,"00000000000000000000000000000001","0000000000000004",1,0,0,2,0,0],"SETTINGS_SAVE_TOTAL":4,"CURRENT_GAME_LOG_V2":[2,1,2,164,3395820480,"00000000000000000000000000000001","0000000000000004",1,1],"UNRELATED_TEXT":"repaired"})probe");
    const std::string rawPrimary = ReadFileToString(path.c_str());
    const std::string rawBackup = ReadFileToString((path + ".bak").c_str());
    {
        fxe::SettingsStoreConfig config;
        config.path = path.c_str();
        fxe::SettingsStore store(config); // No migration: only the real generic scrub can trigger recovery.
        const bool loaded = store.Load();
        Check(loaded, "unrelated genuine text scrub retains a usable selected document");
        Check(std::strcmp(store.ReadValue("UNRELATED_TEXT", ""), "repaired") == 0,
              "unrelated corrupt text recovers from its validated backup value");
        Check(store.Doc()["GAME_ACCEPTED_PLAY"].as<bool>() &&
                  std::strcmp(store.Doc()["GAME_HISTORY_V1"][1U] | "", "00000000000000000000000000000002") == 0 &&
                  store.Doc()["GAME_HISTORY_V1"][3U] == 0U,
              "unrelated text repair retains selected zero-ply game identity and accepted provenance");
        const bool undoPresent = store.Doc().containsKey("GAME_UNDO_HISTORY");
        std::printf("SCRUB_OMISSION_PROBE load=%u undo_present=%u undo=%s text=%s game_id=%s ply=%u\n",
            loaded, undoPresent, store.Doc()["GAME_UNDO_HISTORY"] | "",
            store.ReadValue("UNRELATED_TEXT", ""), store.Doc()["GAME_HISTORY_V1"][1U] | "",
            store.Doc()["GAME_HISTORY_V1"][3U].as<unsigned>());
        Check(!undoPresent,
              "unrelated genuine scrub must not resurrect intentionally absent GAME_UNDO_HISTORY");
        Check(ReadFileToString(path.c_str()) == rawPrimary &&
                  ReadFileToString((path + ".bak").c_str()) == rawBackup,
              "unrelated scrub probe preserves both raw sources before a verified repair save");
    }
    std::filesystem::remove_all(directory);
}


static void CheckMigrationAndCaseOmissions() {
    char directory[] = "/tmp/litch-migration-omission-XXXXXX";
    const bool created = ::mkdtemp(directory) != nullptr;
    Check(created, "create migration omission fixture");
    if (!created) return;
    for (const bool caseOnly : {false, true}) {
        for (unsigned selected = 0; selected < 3U; ++selected) {
            const std::string path = std::string(directory) + "/settings.json";
            for (const char* suffix : {"", ".tmp", ".bak", ".retry"})
                (void)std::remove((path + suffix).c_str());
            const char* canonical = caseOnly
                ? R"({"preferred":2,"canonical":7})"
                : R"({"PREFERRED":2,"legacy":7,"RETIRED":13})";
            const char* older = R"({"PREFERRED":1,"OLD_ONLY":9,"RETIRED":17})";
            settings_store_scoped_recovery_test::WriteFile(path.c_str(),
                selected == 0U ? canonical : selected == 1U ? older : "{broken");
            settings_store_scoped_recovery_test::WriteFile((path + ".bak").c_str(),
                selected == 2U ? canonical : older);
            if (selected == 1U)
                settings_store_scoped_recovery_test::WriteFile((path + ".tmp").c_str(), canonical);
            fxe::SettingsStoreConfig config;
            config.path = path.c_str();
            if (!caseOnly) config.migrateFunc = [](JsonDocument& doc) {
                bool changed = doc.containsKey("RETIRED");
                doc.remove("RETIRED");
                if (doc.containsKey("legacy")) {
                    doc["CANONICAL"] = doc["legacy"];
                    doc.remove("legacy"); changed = true;
                }
                return changed;
            };
            const auto check = [&](fxe::SettingsStore& store) {
                Check(store.ReadValue("PREFERRED", 0U) == 2U &&
                          store.ReadValue("CANONICAL", 0U) == 7U,
                      "migration/case normalization retains selected authority");
                Check(!store.Doc().containsKey("OLD_ONLY"),
                      "migration/case normalization never revives an intentionally absent backup key");
                Check(!store.Doc().containsKey("RETIRED"),
                      "migration-retired keys stay absent after recovery and reboot");
            };
            fxe::SettingsStore first(config);
            Check(first.Load(), "selected primary/tmp/backup migration loads"); check(first);
            Check(first.IsLoadRepairPending() &&
                      first.GetLastError() == fxe::SettingsStoreError::None &&
                      first.GetErrorCount() == 0U && !first.HasConfirmedStorageFailure(),
                  "deferred alternate canonicalization remains pending without confirmed storage failure");
            Check(first.SaveNowBlocking(), "first canonical save succeeds");
            Check(!first.IsLoadRepairPending(),
                  "verified canonical save clears deferred alternate repair");
            fxe::SettingsStore second(config);
            Check(second.Load(), "first canonical reboot succeeds"); check(second);
            Check(second.SaveNowBlocking(), "second canonical save succeeds");
            fxe::SettingsStore third(config);
            Check(third.Load(), "second canonical reboot succeeds"); check(third);
        }
    }
    std::filesystem::remove_all(directory);
}

static void CheckOwnedRecoveryKeysAndBounds() {
    static_assert(sizeof(fxe::FixedFunction<bool(JsonDocument&), 64>) ==
                  sizeof(fxe::SettingsStoreConfig::ScrubFunc),
                  "sanitation context must not grow resident callback storage");
    char directory[] = "/tmp/litch-owned-recovery-XXXXXX";
    const bool created = ::mkdtemp(directory) != nullptr;
    Check(created, "create owned recovery fixture");
    if (!created) return;
    const std::string path = std::string(directory) + "/settings.json";
    settings_store_scoped_recovery_test::WriteFile(path.c_str(),
        R"({"art":"bad\u0000","BAD\u0000suffix":1,"USER":"art"})");
    settings_store_scoped_recovery_test::WriteFile((path + ".bak").c_str(),
        R"({"ART":7,"BAD":8,"OLD_ONLY":9})");
    fxe::SettingsStoreConfig config; config.path = path.c_str();
    config.migrateFunc = [](JsonDocument& doc) {
        if (!doc.containsKey("USER")) return false;
        const std::string user = doc["USER"] | "";
        doc.clear();
        doc["USER"] = user;
        return true;
    };
    {
        fxe::SettingsStore store(config);
        Check(store.Load() && store.ReadValue("ART", 0U) == 7U,
              "recorded lowercase key is owned across source pool replacement");
        Check(std::strcmp(store.ReadValue("USER", ""), "art") == 0,
              "canonical key ownership never mutates equal user string bytes");
        Check(!store.Doc().containsKey("BAD") && !store.Doc().containsKey("OLD_ONLY"),
              "embedded-NUL keys never authorize truncated or unrelated fallback");
        Check(store.Doc().is<JsonObject>() && store.SaveNowBlocking(),
              "scoped recovery saves only selected object, never metadata wrapper");
        const auto saved = ReadFileToString(path.c_str());
        Check(!saved.empty() && saved.front() == '{' && saved.find("OLD_ONLY") == std::string::npos,
              "eligibility metadata and omitted keys never reach disk");
    }
    for (const char* suffix : {"", ".tmp", ".bak", ".retry"})
        (void)std::remove((path + suffix).c_str());
    // A near-full selected document fits the live pool but not selected+provenance.
    DynamicJsonDocument fixture(fxe::SETTINGS_STORE_JSON_CAPACITY);
    fixture["ART"] = "bad\n";
    unsigned fields = 0;
    while (fixture.memoryUsage() < fxe::SETTINGS_STORE_JSON_CAPACITY - 50U) {
        const std::string key = "FIELD_" + std::to_string(fields++);
        fixture[key] = fields;
        if (fixture.overflowed()) break;
    }
    // Rebuild without the last insertion if it exceeded the live bound.
    if (fixture.overflowed()) {
        fixture.remove("FIELD_" + std::to_string(fields - 1U));
        fixture.garbageCollect();
    }
    std::string json; serializeJson(fixture, json);
    Check(json.size() < fxe::SETTINGS_STORE_MAX_FILE_SIZE_BYTES,
          "capacity fixture remains within actual streamed input file bound");
    settings_store_scoped_recovery_test::WriteFile(path.c_str(), json.c_str());
    settings_store_scoped_recovery_test::WriteFile((path + ".bak").c_str(), R"({"ART":7})");
    const std::string rawPrimary = ReadFileToString(path.c_str());
    const std::string rawBackup = ReadFileToString((path + ".bak").c_str());
    {
        fxe::SettingsStore store(config);
        const bool loaded = store.Load();
        std::printf("RECOVERY_RESOURCE pool_limit=%zu recorder_stack=%zu config=%zu store=%zu input_memory=%zu loaded=%u error=%u\n",
            fxe::SETTINGS_STORE_JSON_CAPACITY, sizeof(fxe::SettingsRemovedKeys),
            sizeof(fxe::SettingsStoreConfig), sizeof(fxe::SettingsStore), fixture.memoryUsage(),
            loaded, unsigned(store.GetLastError()));
        Check(!loaded && store.GetLastError() == fxe::SettingsStoreError::ResourceExhausted,
              "provenance plus selected-capacity overflow fails closed");
        Check(ReadFileToString(path.c_str()) == rawPrimary &&
                  ReadFileToString((path + ".bak").c_str()) == rawBackup,
              "resource failure preserves every selected and alternate byte");
    }
    (void)std::remove((path + ".bak").c_str());
    {
        fxe::SettingsStore store(config);
        Check(store.Load() && !store.Doc().containsKey("ART"),
              "same sanitation without an alternate needs no recovery pool");
    }
    std::filesystem::remove_all(directory);
}

static void CheckUnavailableAlternateAfterSanitation() {
    char directory[] = "/tmp/litch-unavailable-sanitation-XXXXXX";
    const bool created = ::mkdtemp(directory) != nullptr;
    Check(created, "create unavailable sanitation fixture");
    if (!created) return;
    const std::string path = std::string(directory) + "/settings.json";
    for (const char* suffix : {".bak", ".tmp", ""}) {
        const bool selectedBackup = suffix[0] == '\0';
        const std::string selected = selectedBackup ? path + ".bak" : path;
        settings_store_scoped_recovery_test::WriteFile(selected.c_str(),
            R"({"TEXT":"bad\u0000text","VALUE":7})");
        const std::string rawSelected = ReadFileToString(selected.c_str());
        const std::string alternate = selectedBackup ? path : path + suffix;
        if (selectedBackup) std::filesystem::remove(path);
        std::filesystem::create_symlink(alternate, alternate);
        fxe::SettingsStoreConfig config; config.path = path.c_str();
        fxe::SettingsStore store(config);
        unsigned accesses = 0U;
        store.SetStorageAccessCallbacks(nullptr, [&]() { ++accesses; });
        const bool loaded = store.Load();
        std::printf("UNAVAILABLE_SANITATION source=%s loaded=%u error=%u accesses=%u\n",
                    selectedBackup ? "primary" : suffix, loaded, unsigned(store.GetLastError()), accesses);
        Check(!loaded && store.GetLastError() == fxe::SettingsStoreError::OpenReadFailed,
              "sanitation cannot treat an unavailable alternate as proven absent");
        Check(accesses == (std::strcmp(suffix, ".tmp") == 0 ? 4U : 5U),
              "unavailable sanitation uses only the existing bounded read attempts");
        Check(ReadFileToString(selected.c_str()) == rawSelected &&
                  std::filesystem::is_symlink(alternate) && store.GetSaveCount() == 0U,
              "failed sanitation preserves selected bytes and unavailable alternate");
        std::filesystem::remove(alternate);
        std::filesystem::remove(selected);
    }
    std::filesystem::remove_all(directory);
}

int main() {
    fxe::test_support::TestResults results("SETTINGS_STORE");
    results.Start();
    g_results = &results;

    CheckUnrelatedScrubPreservesGameOmissions();
    CheckMigrationAndCaseOmissions();
    CheckOwnedRecoveryKeysAndBounds();
    CheckUnavailableAlternateAfterSanitation();

    CheckPartitionErasureClassification();

    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    fxe::SettingsStoreConfig cfg;
    cfg.path = "test_settings_store_settings.json";
    cfg.deferredSaveDelayMs = 50;

    // Ensure a clean starting point regardless of suite order.
    (void)std::remove(cfg.path);
    (void)std::remove("test_settings_store_settings.json.bak");
    (void)std::remove("test_settings_store_settings.json.tmp");

    {
        fxe::SettingsStore store(cfg);
        Check(store.Load(), "Load succeeds when file missing");

        store.Doc()["brightness"] = 7;
        store.Doc()["FEATURE_FLAG"] = 1;
        fxe::ManualMillis::SetNowMs(0);
        store.MarkDirty();
        fxe::ManualMillis::SetNowMs(10);
        store.Update();
        // Not yet saved.
        fxe::ManualMillis::SetNowMs(49);
        store.Update();
        fxe::ManualMillis::SetNowMs(50);
        store.Update();
        Check(store.SaveNow(), "SaveNow succeeds");

        const std::string saved = ReadFileToString(cfg.path);
        Check(saved.find("\"BRIGHTNESS\":7") != std::string::npos,
              "saved keys are canonical uppercase");
    }

    // Every filesystem operation in a save, including the verification read,
    // must pass through the storage-access hooks. Firmware uses these hooks to
    // prevent LittleFS from running concurrently with an RMT LED transfer.
    {
        fxe::SettingsStoreConfig accessCfg;
        accessCfg.path = "test_settings_store_access_hooks.json";
        accessCfg.verifyWrites = true;
        (void)std::remove(accessCfg.path);
        (void)std::remove("test_settings_store_access_hooks.json.bak");
        (void)std::remove("test_settings_store_access_hooks.json.tmp");

        uint32_t beforeCount = 0U;
        uint32_t afterCount = 0U;
        uint32_t activeDepth = 0U;
        uint32_t maxDepth = 0U;
        fxe::SettingsStore store(accessCfg);
        store.SetStorageAccessCallbacks(
            [&]() {
                ++beforeCount;
                ++activeDepth;
                if (activeDepth > maxDepth) {
                    maxDepth = activeDepth;
                }
            },
            [&]() {
                ++afterCount;
                if (activeDepth > 0U) {
                    --activeDepth;
                }
            });
        store.Doc()["VALUE"] = 7U;
        Check(store.SaveNow(), "save succeeds with storage-access hooks");
        Check(beforeCount > 0U,
              "save routes filesystem access through coordination hooks");
        Check(beforeCount == afterCount && activeDepth == 0U,
              "storage-access hooks remain balanced");
        Check(maxDepth == 1U,
              "storage-access hooks do not nest");
        constexpr size_t runtimeSnapshotBytes =
            fxe::SETTINGS_STORE_SNAPSHOT_BUFFER_CAPACITY;
        constexpr size_t payloadWindows = 2U *
            ((runtimeSnapshotBytes + fxe::SETTINGS_SAVE_IO_CHUNK_BYTES - 1U) /
             fxe::SETTINGS_SAVE_IO_CHUNK_BYTES);
        std::printf("[SETTINGS_SAVE_CADENCE] snapshot_bound=%zu chunk=%zu payload_windows=%zu\n",
                    runtimeSnapshotBytes, fxe::SETTINGS_SAVE_IO_CHUNK_BYTES, payloadWindows);
        Check(payloadWindows <= 22U && fxe::SETTINGS_SAVE_IO_CHUNK_BYTES <= 512U,
              "runtime snapshot payload uses at most twenty-two bounded write/read render windows");

        const uint32_t verifiedAccessCount = beforeCount;
        accessCfg.verifyWrites = false;
        accessCfg.path = "test_settings_store_access_hooks_no_verify.json";
        (void)std::remove(accessCfg.path);
        (void)std::remove("test_settings_store_access_hooks_no_verify.json.bak");
        (void)std::remove("test_settings_store_access_hooks_no_verify.json.tmp");
        beforeCount = 0U;
        afterCount = 0U;
        activeDepth = 0U;
        maxDepth = 0U;
        fxe::SettingsStore noVerifyStore(accessCfg);
        noVerifyStore.SetStorageAccessCallbacks(
            [&]() {
                ++beforeCount;
                ++activeDepth;
                if (activeDepth > maxDepth) {
                    maxDepth = activeDepth;
                }
            },
            [&]() {
                ++afterCount;
                if (activeDepth > 0U) {
                    --activeDepth;
                }
            });
        noVerifyStore.Doc()["VALUE"] = 7U;
        Check(noVerifyStore.SaveNow(),
              "save without verification succeeds with storage-access hooks");
        Check(verifiedAccessCount > beforeCount,
              "verification read contributes coordinated storage accesses");
        Check(beforeCount == afterCount && activeDepth == 0U,
              "non-verifying storage-access hooks remain balanced");

        (void)std::remove("test_settings_store_access_hooks.json");
        (void)std::remove("test_settings_store_access_hooks.json.bak");
        (void)std::remove("test_settings_store_access_hooks.json.tmp");
        (void)std::remove("test_settings_store_access_hooks_no_verify.json");
        (void)std::remove("test_settings_store_access_hooks_no_verify.json.bak");
        (void)std::remove("test_settings_store_access_hooks_no_verify.json.tmp");
    }

    {
        fxe::SettingsStore store(cfg);
        Check(store.Load(), "Load reads saved file");
        const int32_t brightness = store.ReadValue("brightness", static_cast<int32_t>(0));
        Check(brightness == 7, "brightness round-trips");

        const int32_t featureFlag = store.ReadValue("FEATURE_FLAG", static_cast<int32_t>(0));
        Check(featureFlag == 1, "FEATURE_FLAG round-trips");
    }

    // Deferred save is debounced from the most recent MarkDirty(). This prevents
    // saving while a user is actively adjusting a value (e.g., button repeat).
    {
        (void)std::remove(cfg.path);
        (void)std::remove("test_settings_store_settings.json.bak");
        (void)std::remove("test_settings_store_settings.json.tmp");

        fxe::SettingsStore store(cfg);
        Check(store.Load(), "Load succeeds (debounced MarkDirty test)");

        store.Doc()["brightness"] = 3;
        fxe::ManualMillis::SetNowMs(0);
        store.MarkDirty();
        // Another change arrives before the original delay elapses.
        store.Doc()["brightness"] = 4;
        fxe::ManualMillis::SetNowMs(40);
        store.MarkDirty();

        const uint32_t savesBefore = store.GetSaveCount();
        // If not debounced, this would save at t=50.
        fxe::ManualMillis::SetNowMs(50);
        store.Update();
        Check(store.GetSaveCount() == savesBefore, "Update does not save before (last dirty + delay)");

        // Debounced save should occur at t>=40+50=90.
        fxe::ManualMillis::SetNowMs(89);
        store.Update();
        Check(store.GetSaveCount() == savesBefore, "Update does not save before debounce deadline");
        fxe::ManualMillis::SetNowMs(90);
        store.Update();
        Check(store.GetSaveCount() == savesBefore + 1, "Update saves once debounce deadline elapses");
    }

    // Persistence uses two independent trailing-debounce tiers. Must-save
    // changes are durable within 100 ms of their latest change, while ordinary
    // settings coalesce for 2 seconds. An ordinary change must never postpone
    // an already-pending must-save deadline.
    {
        fxe::SettingsStoreConfig tieredCfg;
        tieredCfg.path = "test_settings_store_tiered.json";
        tieredCfg.mustSaveDelayMs = 100U;
        tieredCfg.deferredSaveDelayMs = 2000U;
        (void)std::remove(tieredCfg.path);

        fxe::SettingsStore store(tieredCfg);
        Check(store.Load(), "Load succeeds (tiered debounce test)");

        uint32_t requests = 0U;
        store.SetAsyncSaveRequestCallback(
            [&requests](const JsonDocument&) {
                ++requests;
                return true;
            });

        fxe::ManualMillis::SetNowMs(0U);
        store.MarkMustSaveDirty();
        fxe::ManualMillis::SetNowMs(99U);
        store.Update();
        Check(requests == 0U, "must-save debounce waits through 99 ms");
        fxe::ManualMillis::SetNowMs(100U);
        store.Update();
        Check(requests == 1U, "must-save debounce enqueues at 100 ms");

        fxe::ManualMillis::SetNowMs(200U);
        store.MarkDirty();
        fxe::ManualMillis::SetNowMs(2199U);
        store.Update();
        Check(requests == 1U, "ordinary debounce waits through 1999 ms");
        fxe::ManualMillis::SetNowMs(2200U);
        store.Update();
        Check(requests == 2U, "ordinary debounce enqueues at 2 seconds");

        fxe::ManualMillis::SetNowMs(3000U);
        store.MarkMustSaveDirty();
        fxe::ManualMillis::SetNowMs(3050U);
        store.MarkDirty();
        fxe::ManualMillis::SetNowMs(3099U);
        store.Update();
        Check(requests == 2U, "ordinary change does not accelerate must-save debounce");
        fxe::ManualMillis::SetNowMs(3100U);
        store.Update();
        Check(requests == 3U, "ordinary change does not postpone must-save deadline");

        fxe::ManualMillis::SetNowMs(4000U);
        store.MarkMustSaveDirty();
        fxe::ManualMillis::SetNowMs(4050U);
        store.MarkMustSaveDirty();
        fxe::ManualMillis::SetNowMs(4149U);
        store.Update();
        Check(requests == 3U, "must-save burst coalesces from latest change");
        fxe::ManualMillis::SetNowMs(4150U);
        store.Update();
        Check(requests == 4U, "must-save burst emits one request at its deadline");

        (void)std::remove(tieredCfg.path);
    }

    // Firmware can prohibit the generic synchronous fallback. A missed worker
    // start then leaves the change pending instead of writing flash from the
    // cooperative render/input loop; explicit lifecycle drains still work.
    {
        fxe::SettingsStoreConfig asyncOnlyCfg;
        asyncOnlyCfg.path = "test_settings_store_async_only.json";
        asyncOnlyCfg.mustSaveDelayMs = 100U;
        asyncOnlyCfg.allowSynchronousDeferredSaves = false;
        (void)std::remove(asyncOnlyCfg.path);

        fxe::SettingsStore store(asyncOnlyCfg);
        Check(store.Load(), "Load succeeds (async-only deferred save test)");
        store.Doc()["IMPORTANT"] = 1U;
        fxe::ManualMillis::SetNowMs(0U);
        store.MarkMustSaveDirty();
        fxe::ManualMillis::SetNowMs(100U);
        store.Update();
        Check(store.GetSaveCount() == 0U,
              "async-only deferred save does not fall back to caller flash I/O");
        Check(ReadFileToString(asyncOnlyCfg.path).empty(),
              "async-only deferred save leaves storage untouched without worker");
        Check(store.SaveNow(),
              "explicit lifecycle save still works in async-only mode");
        Check(store.GetSaveCount() == 1U,
              "explicit lifecycle save persists pending async-only state");

        (void)std::remove(asyncOnlyCfg.path);
    }

    // Backup recovery: if the primary file is corrupt, Load should fall back
    // to the last known-good backup.
    {
        (void)std::remove(cfg.path);
        (void)std::remove("test_settings_store_settings.json.bak");
        (void)std::remove("test_settings_store_settings.json.tmp");

        fxe::SettingsStore store(cfg);
        Check(store.Load(), "Load succeeds when file missing (backup recovery setup)");

        store.Doc()["brightness"] = 7;
        store.Doc()["FEATURE_FLAG"] = 1;
        Check(store.SaveNow(), "SaveNow succeeds (seed primary)");

        store.Doc()["brightness"] = 8;
        Check(store.SaveNow(), "SaveNow succeeds (creates backup)");

        fxe::SettingsStore corruptor(cfg);
        Check(corruptor.CorruptFileForTest(), "CorruptFileForTest succeeds (primary corrupted)");

        fxe::SettingsStore recovered(cfg);
        Check(recovered.Load(), "Load succeeds by recovering from backup");

        const int32_t brightness = recovered.ReadValue("brightness", static_cast<int32_t>(0));
        Check(brightness == 7, "Recovered brightness uses backup value");

        const int32_t featureFlag = recovered.ReadValue("FEATURE_FLAG", static_cast<int32_t>(0));
        Check(featureFlag == 1, "Recovered FEATURE_FLAG uses backup value");
    }

    // Power-loss window recovery: SaveImpl() renames main->bak then tmp->main.
    // If power is lost between those renames, the primary file can be missing
    // while a valid backup exists. Load should recover from the backup.
    {
        (void)std::remove(cfg.path);
        (void)std::remove("test_settings_store_settings.json.bak");
        (void)std::remove("test_settings_store_settings.json.tmp");

        fxe::SettingsStore store(cfg);
        Check(store.Load(), "Load succeeds when file missing (power-loss recovery setup)");

        store.Doc()["brightness"] = 7;
        store.Doc()["FEATURE_FLAG"] = 1;
        Check(store.SaveNow(), "SaveNow succeeds (seed primary)");

        store.Doc()["brightness"] = 8;
        Check(store.SaveNow(), "SaveNow succeeds (creates backup)");

        // Simulate a power loss after main->bak rename by deleting the primary.
        Check(std::remove(cfg.path) == 0, "remove(primary) succeeds");

        fxe::SettingsStore recovered(cfg);
        Check(recovered.Load(), "Load succeeds by recovering from backup when primary missing");

        const int32_t brightness = recovered.ReadValue("brightness", static_cast<int32_t>(0));
        Check(brightness == 7, "Recovered brightness uses backup value when primary missing");

        const int32_t featureFlag = recovered.ReadValue("FEATURE_FLAG", static_cast<int32_t>(0));
        Check(featureFlag == 1, "Recovered FEATURE_FLAG uses backup value when primary missing");

        PersistAfterUserAction(recovered, cfg);
        const std::string saved = ReadFileToString(cfg.path);
        Check(!saved.empty(), "Recovered settings are re-saved to primary file");
    }

    // Interrupted-save recovery: if a valid .tmp exists alongside an older primary,
    // Load should prefer the tmp in RAM (it likely contains the newest settings).
    // A later user mutation owns its promotion.
    {
        (void)std::remove(cfg.path);
        (void)std::remove("test_settings_store_settings.json.bak");
        (void)std::remove("test_settings_store_settings.json.tmp");

        // Primary (older).
        {
            std::FILE* f = std::fopen(cfg.path, "wb");
            Check(f != nullptr, "fopen primary succeeds (interrupted-save recovery)");
            if (f != nullptr) {
                const char* contents = "{\"brightness\":7}";
                const size_t n = std::strlen(contents);
                (void)std::fwrite(contents, 1, n, f);
                std::fclose(f);
            }
        }

        // Tmp (newer), written but not yet renamed into place.
        {
            std::FILE* f = std::fopen("test_settings_store_settings.json.tmp", "wb");
            Check(f != nullptr, "fopen tmp succeeds (interrupted-save recovery)");
            if (f != nullptr) {
                const char* contents = "{\"brightness\":9}";
                const size_t n = std::strlen(contents);
                (void)std::fwrite(contents, 1, n, f);
                std::fclose(f);
            }
        }

        fxe::SettingsStore recovered(cfg);
        Check(recovered.Load(), "Load succeeds with primary+tmp present");

        const int32_t brightness = recovered.ReadValue("brightness", static_cast<int32_t>(0));
        Check(brightness == 9, "tmp brightness is preferred over primary");

        PersistAfterUserAction(recovered, cfg);
        const std::string saved = ReadFileToString(cfg.path);
        Check(saved.find("\"BRIGHTNESS\":9") != std::string::npos,
              "tmp settings promoted to primary file");
    }

    // Scrub recovery: if the primary parses but contains an actual decoded NUL,
    // Load should recover the missing key from the backup.
    {
        (void)std::remove(cfg.path);
        (void)std::remove("test_settings_store_settings.json.bak");
        (void)std::remove("test_settings_store_settings.json.tmp");

        // The JSON escape decodes to a NUL byte and must be scrubbed.
        {
            std::FILE* f = std::fopen(cfg.path, "wb");
            Check(f != nullptr, "fopen primary succeeds (scrub recovery)");
            if (f != nullptr) {
                const char* contents =
                    R"({"BRIGHTNESS":3,"GAME_FEN":"bad\u0000","FEATURE_FLAG":1})";
                const size_t n = std::strlen(contents);
                (void)std::fwrite(contents, 1, n, f);
                std::fclose(f);
            }
        }

        // Backup contains the last known-good value for GAME_FEN.
        {
            std::FILE* f = std::fopen("test_settings_store_settings.json.bak", "wb");
            Check(f != nullptr, "fopen backup succeeds (scrub recovery)");
            if (f != nullptr) {
                const char* contents =
                    "{\"BRIGHTNESS\":7,\"GAME_FEN\":\"4k3/8/8/8/8/8/8/4K3 w - - 0 1\"}";
                const size_t n = std::strlen(contents);
                (void)std::fwrite(contents, 1, n, f);
                std::fclose(f);
            }
        }

        fxe::SettingsStore store(cfg);
        Check(store.Load(), "Load succeeds (scrub recovery)");

        // Non-scrubbed scalar from primary must be preserved.
        Check(store.ReadValue("BRIGHTNESS", static_cast<int32_t>(0)) == 3, "primary brightness preserved (scrub recovery)");
        Check(store.ReadValue("FEATURE_FLAG", static_cast<int32_t>(0)) == 1, "primary FEATURE_FLAG preserved (scrub recovery)");

        // Scrubbed key must be recovered from backup.
        const char* fen = store.ReadValue("GAME_FEN", "");
        Check(fen != nullptr && std::strcmp(fen, "4k3/8/8/8/8/8/8/4K3 w - - 0 1") == 0,
              "GAME_FEN recovered from backup after scrub");

        PersistAfterUserAction(store, cfg);
        const std::string saved = ReadFileToString(cfg.path);
        Check(saved.find("GAME_FEN") != std::string::npos, "recovered GAME_FEN re-saved to primary");
    }

    // Corrupt file should fail to parse and be recoverable by deleting.
    {
        // Ensure backup recovery is not involved in this specific failure-mode test.
        (void)std::remove("test_settings_store_settings.json.bak");
        (void)std::remove("test_settings_store_settings.json.tmp");

        fxe::SettingsStore store(cfg);
        Check(store.CorruptFileForTest(), "CorruptFileForTest succeeds");

        fxe::SettingsStore store2(cfg);
        Check(!store2.Load(), "Load fails on corrupt JSON");
        Check(store2.GetLastError() == fxe::SettingsStoreError::ParseFailed, "LastError is ParseFailed");

        Check(store2.DeleteFile(), "DeleteFile succeeds after corruption");

        fxe::SettingsStore store3(cfg);
        Check(store3.Load(), "Load succeeds after delete");
    }

    // A syntactically incomplete file must fail at the public SettingsStore seam.
    {
        (void)std::remove(cfg.path);
        (void)std::remove("test_settings_store_settings.json.bak");
        (void)std::remove("test_settings_store_settings.json.tmp");

        std::FILE* f = std::fopen(cfg.path, "wb");
        Check(f != nullptr, "fopen succeeds (truncated input)");
        if (f != nullptr) {
            const char* contents = R"({"BRIGHTNESS":7,"TEXT":"unfinished)";
            const size_t n = std::strlen(contents);
            Check(std::fwrite(contents, 1, n, f) == n,
                  "fwrite succeeds (truncated input)");
            (void)std::fclose(f);
        }

        fxe::SettingsStore store(cfg);
        Check(!store.Load(), "Load rejects truncated JSON");
        Check(store.GetLastError() == fxe::SettingsStoreError::ParseFailed,
              "truncated JSON reports ParseFailed");
    }

    // A valid JSON prefix must not bypass the persisted-file size limit by padding
    // the remainder with whitespace.
    {
        (void)std::remove(cfg.path);
        (void)std::remove("test_settings_store_settings.json.bak");
        (void)std::remove("test_settings_store_settings.json.tmp");

        std::FILE* f = std::fopen(cfg.path, "wb");
        Check(f != nullptr, "fopen succeeds (oversize input)");
        if (f != nullptr) {
            const char* prefix = R"({"BRIGHTNESS":12})";
            const size_t prefixLen = std::strlen(prefix);
            Check(std::fwrite(prefix, 1, prefixLen, f) == prefixLen,
                  "fwrite prefix succeeds (oversize input)");

            char padding[256];
            std::memset(padding, ' ', sizeof(padding));
            size_t remaining = fxe::SETTINGS_STORE_MAX_FILE_SIZE_BYTES - prefixLen;
            while (remaining > 0) {
                const size_t chunk = (remaining < sizeof(padding)) ? remaining : sizeof(padding);
                Check(std::fwrite(padding, 1, chunk, f) == chunk,
                      "fwrite padding succeeds (oversize input)");
                remaining -= chunk;
            }
            (void)std::fclose(f);
        }

        fxe::SettingsStore store(cfg);
        Check(!store.Load(), "Load rejects file at persisted-file size limit");
        Check(store.GetLastError() == fxe::SettingsStoreError::FileTooLarge,
              "oversize JSON reports the size limit rather than malformed data");
    }

    // JSON can legally include embedded NUL characters via \u0000 escapes.
    // We treat these as invalid for persisted text fields and scrub them.
    {
        std::FILE* f = std::fopen(cfg.path, "wb");
        Check(f != nullptr, "fopen succeeds");
        if (f != nullptr) {
            const char* json =
                R"({"TEXT_PRIMARY":"alpha\u0000","TEXT_SECONDARY":"x\u0000y","BRIGHTNESS":5})";
            Check(std::fwrite(json, 1, std::strlen(json), f) == std::strlen(json), "fwrite succeeds");
            (void)std::fclose(f);
        }

        fxe::SettingsStore store(cfg);
        Check(store.Load(), "Load succeeds with JSON containing \\u0000 escapes");
        Check(!store.Doc().containsKey("TEXT_PRIMARY"), "Load scrubs TEXT_PRIMARY containing NUL");
        Check(!store.Doc().containsKey("TEXT_SECONDARY"), "Load scrubs TEXT_SECONDARY containing NUL");

        const int32_t brightness = store.ReadValue("BRIGHTNESS", static_cast<int32_t>(0));
        Check(brightness == 5, "Other keys remain intact");

        PersistAfterUserAction(store, cfg);
        const std::string saved = ReadFileToString(cfg.path);
        Check(saved.find("TEXT_PRIMARY") == std::string::npos, "Scrubbed keys are not re-saved");
        Check(saved.find("\\u0000") == std::string::npos, "Scrubbed output contains no \\u0000");
    }

    // Printable escape-like spelling is ordinary text, including in nested
    // structures. It must not be mistaken for a decoded control character.
    {
        std::FILE* f = std::fopen(cfg.path, "wb");
        Check(f != nullptr, "fopen succeeds");
        if (f != nullptr) {
            const char* json =
                R"({"BRIGHTNESS":4,"nested":{"ok":1,"bad":"x\\u0000y"},"arr":["ok","x\\u0000y",{"bad2":"x\\u0000y"}]})";
            Check(std::fwrite(json, 1, std::strlen(json), f) == std::strlen(json), "fwrite succeeds");
            (void)std::fclose(f);
        }

        fxe::SettingsStore store(cfg);
        Check(store.Load(), "Load succeeds with nested JSON containing literal \\\\u0000 text");

        const int32_t brightness = store.ReadValue("BRIGHTNESS", static_cast<int32_t>(0));
        Check(brightness == 4, "Other keys remain intact");

        const JsonObjectConst root = store.Doc().as<JsonObjectConst>();

        Check(root.containsKey("NESTED"), "nested object remains");
        const JsonObjectConst nested = root["NESTED"].as<JsonObjectConst>();
        Check(std::strcmp(nested["bad"] | "", "x\\u0000y") == 0,
              "nested printable spelling preserved");

        Check(root.containsKey("ARR"), "arr remains");
        const JsonArrayConst arr = root["ARR"].as<JsonArrayConst>();
        Check(arr.size() == 3, "arr size remains");
        Check(std::strcmp(arr[1] | "", "x\\u0000y") == 0,
              "array printable spelling preserved");
        Check(std::strcmp(arr[2]["bad2"] | "", "x\\u0000y") == 0,
              "nested array printable spelling preserved");

        PersistAfterUserAction(store, cfg);
        const std::string saved = ReadFileToString(cfg.path);
        Check(saved.find("\\\\u0000") != std::string::npos,
              "unchanged output retains printable escape-like spelling");
    }

    // Top-level printable escape-like spelling follows the same rule.
    {
        std::FILE* f = std::fopen(cfg.path, "wb");
        Check(f != nullptr, "fopen succeeds");
        if (f != nullptr) {
            const char* json =
                R"({"TEXT_PRIMARY":"alpha\\u0000","TEXT_SECONDARY":"x\\u0000y","BRIGHTNESS":6})";
            Check(std::fwrite(json, 1, std::strlen(json), f) == std::strlen(json), "fwrite succeeds");
            (void)std::fclose(f);
        }

        fxe::SettingsStore store(cfg);
        Check(store.Load(), "Load succeeds with JSON containing literal \\u0000 text");
        Check(std::strcmp(store.ReadValue("TEXT_PRIMARY", ""),
                          "alpha\\u0000") == 0,
              "Load preserves TEXT_PRIMARY printable spelling");
        Check(std::strcmp(store.ReadValue("TEXT_SECONDARY", ""),
                          "x\\u0000y") == 0,
              "Load preserves TEXT_SECONDARY printable spelling");

        const int32_t brightness = store.ReadValue("BRIGHTNESS", static_cast<int32_t>(0));
        Check(brightness == 6, "Other keys remain intact");

        PersistAfterUserAction(store, cfg);
        const std::string saved = ReadFileToString(cfg.path);
        Check(saved.find("TEXT_PRIMARY") != std::string::npos,
              "printable key remains persisted");
        Check(saved.find("\\\\u0000") != std::string::npos,
              "persisted output retains printable escape-like spelling");
    }

    // Cached access: values refresh when the document revision changes.
    {
        (void)std::remove(cfg.path);
        (void)std::remove("test_settings_store_settings.json.bak");
        (void)std::remove("test_settings_store_settings.json.tmp");

        fxe::SettingsStore store(cfg);
        Check(store.Load(), "Load succeeds (cached settings)");

        fxe::CachedSetting<uint32_t> brightnessCache("BRIGHTNESS", 2U);
        Check(brightnessCache.Get(store) == 2U, "cached default uses default value");

        store.Doc()["BRIGHTNESS"] = 5;
        store.NotifyDocChanged();
        Check(brightnessCache.Get(store) == 5U, "cache updates after NotifyDocChanged");

        store.Doc()["BRIGHTNESS"] = 6;
        store.MarkDirty();
        Check(brightnessCache.Get(store) == 6U, "cache updates after MarkDirty");

        store.Doc()["FLAG"] = 1;
        store.NotifyDocChanged();
        Check(store.ReadValue("FLAG", false), "ReadValue bool returns true");
        Check(store.ReadValue("MISSING_KEY", 3U) == 3U, "ReadValue returns default when missing");
    }

    // Firmware can install an asynchronous persistence sink. SaveNow and the
    // deferred Update path must enqueue a snapshot without touching storage on
    // the calling task.
    {
        (void)std::remove(cfg.path);
        fxe::SettingsStore store(cfg);
        Check(store.Load(), "Load succeeds (async persistence test)");

        uint32_t requests = 0U;
        uint32_t guardedRequests = 0U;
        uint32_t queuedValue = 0U;
        store.SetBeforePersistenceCallback([&guardedRequests]() {
            ++guardedRequests;
        });
        store.SetAsyncSaveRequestCallback(
            [&requests, &queuedValue](const JsonDocument& doc) {
                ++requests;
                queuedValue = doc["ASYNC_VALUE"] | 0U;
                return true;
            });

        store.Doc()["ASYNC_VALUE"] = 7U;
        store.ArmPersistenceGuard();
        Check(store.SaveNow(), "SaveNow accepts async save request");
        Check(requests == 1U, "SaveNow calls async sink once");
        Check(guardedRequests == 0U, "async admission does not invoke blocking persistence guard");
        Check(queuedValue == 7U, "async sink sees current document snapshot");
        Check(store.GetSaveCount() == 0U, "async request does not save on caller");
        Check(ReadFileToString(cfg.path).empty(), "async request does not write settings file");

        store.Doc()["ASYNC_VALUE"] = 9U;
        fxe::ManualMillis::SetNowMs(100U);
        store.MarkDirty();
        fxe::ManualMillis::SetNowMs(149U);
        store.Update();
        Check(requests == 1U, "deferred async request waits for debounce");
        fxe::ManualMillis::SetNowMs(150U);
        store.Update();
        Check(requests == 2U, "deferred Update enqueues through async sink");
        Check(queuedValue == 9U, "deferred async sink sees latest value");
        store.Update();
        Check(requests == 2U, "accepted async request clears deferred dirty state");

        store.ReportAsyncSaveFailure(fxe::SettingsStoreError::WriteFailed);
        Check(store.GetLastError() == fxe::SettingsStoreError::WriteFailed,
              "async worker failure reaches store diagnostics");
        fxe::ManualMillis::SetNowMs(200U);
        store.Update();
        Check(requests == 2U, "async failure retry is debounced");
        fxe::ManualMillis::SetNowMs(299U);
        store.Update();
        Check(requests == 2U, "async failure retry waits through 99 ms");
        fxe::ManualMillis::SetNowMs(300U);
        store.Update();
        Check(requests == 3U, "async failure schedules a later retry");

        store.ReportAsyncSaveSuccess(321U, 42U);
        Check(store.GetLastError() == fxe::SettingsStoreError::None,
              "async success clears worker error");
        Check(store.GetSaveCount() == 1U,
              "async success contributes to save diagnostics");
        Check(store.GetLastSaveSizeBytes() == 321U,
              "async success reports saved byte count");
        Check(store.GetPersistedSaveTotal() == 42U,
              "async success reports persisted total");

        bool acceptRequest = false;
        store.SetAsyncSaveRequestCallback(
            [&acceptRequest](const JsonDocument&) { return acceptRequest; });
        Check(!store.SaveNow(), "rejected async request reports failure");
        Check(store.GetLastError() == fxe::SettingsStoreError::AdmissionRejected &&
                  !store.HasConfirmedStorageFailure(),
              "rejected request exposes unavailable durability");
        acceptRequest = true;
        Check(store.SaveNow(), "retry can enqueue successfully");
        Check(store.GetLastError() == fxe::SettingsStoreError::AdmissionRejected,
              "enqueue alone cannot clear a prior durability failure");
        store.ReportAsyncSaveSuccess(321U, 43U);
        Check(store.GetLastError() == fxe::SettingsStoreError::None,
              "verified retry completion clears the rejected-request failure");
    }

    settings_store_persistence_guard_test::Run(cfg, &Check);
    settings_store_scratch_cleanup_test::Run(cfg, &Check);
    settings_store_scoped_recovery_test::Run();
    settings_store_repair_pending_test::Run();
    CheckRejectedPromotionSaveTotal();

    (void)std::remove(cfg.path);
    (void)std::remove("test_settings_store_settings.json.bak");
    (void)std::remove("test_settings_store_settings.json.tmp");

    (void)std::remove("test_settings_store_verify_writes.json");
    (void)std::remove("test_settings_store_verify_writes.json.bak");
    (void)std::remove("test_settings_store_verify_writes.json.tmp");

    return results.Finish();
}
