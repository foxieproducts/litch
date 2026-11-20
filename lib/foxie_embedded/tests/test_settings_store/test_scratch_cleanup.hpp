#pragma once

#include <cstdio>
#include <cstring>

#include <string>

#include "foxie_embedded/settings_store.hpp"

namespace settings_store_scratch_cleanup_test {

using CheckFn = void (*)(bool, const char*);

inline void RemoveStoreFiles(const char* path) {
    (void)std::remove(path);
    const std::string backup = std::string(path) + ".bak";
    const std::string tmp = std::string(path) + ".tmp";
    (void)std::remove(backup.c_str());
    (void)std::remove(tmp.c_str());
}

inline bool WriteFile(const char* path, const std::string& contents) {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }
    const size_t written = std::fwrite(contents.data(), 1, contents.size(), file);
    (void)std::fclose(file);
    return written == contents.size();
}

inline std::string ReadFile(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return {};
    }
    std::string contents;
    char buffer[256];
    for (;;) {
        const size_t count = std::fread(buffer, 1, sizeof(buffer), file);
        contents.append(buffer, count);
        if (count < sizeof(buffer)) {
            break;
        }
    }
    (void)std::fclose(file);
    return contents;
}

inline void CheckCollisionPolicy(const fxe::SettingsStoreConfig& baseConfig,
                                 const CheckFn check) {
    struct Fixture {
        const char* json;
        int32_t expected;
    };
    const Fixture fixtures[] = {
        {R"({"mixed":1,"MIXED":2})", 2},
        {R"({"MIXED":2,"mixed":1})", 2},
        {R"({"mixed":1,"Mixed":3})", 1},
    };

    fxe::SettingsStoreConfig config = baseConfig;
    config.path = "test_settings_store_collisions.json";
    for (const Fixture& fixture : fixtures) {
        RemoveStoreFiles(config.path);
        check(WriteFile(config.path, fixture.json), "write case-collision fixture");

        fxe::SettingsStore store(config);
        check(store.Load(), "load case-collision fixture");
        check(store.ReadValue("MIXED", static_cast<int32_t>(0)) == fixture.expected,
              "canonical uppercase wins; otherwise first alias wins");
        check(!store.Doc().containsKey("mixed") && !store.Doc().containsKey("Mixed"),
              "case-collision aliases collapse to one setting");
    }
    RemoveStoreFiles(config.path);
}

inline void CheckDeduplicatedStringSafety(const fxe::SettingsStoreConfig& baseConfig,
                                          const CheckFn check) {
    fxe::SettingsStoreConfig config = baseConfig;
    config.path = "test_settings_store_shared_strings.json";
    RemoveStoreFiles(config.path);
    check(WriteFile(config.path, R"({"shared":{"shared":"shared"}})"),
          "write deduplicated-string fixture");

    fxe::SettingsStore store(config);
    check(store.Load(), "load deduplicated-string fixture");
    check(store.Doc().containsKey("SHARED"), "top-level shared key canonicalized");
    const JsonObjectConst nested = store.Doc()["SHARED"].as<JsonObjectConst>();
    check(nested.containsKey("shared"), "nested shared key remains lowercase");
    check(std::strcmp(nested["shared"] | "", "shared") == 0,
          "deduplicated nested string value is not rewritten");
    RemoveStoreFiles(config.path);
}

inline void CheckRecoveryHooks(const fxe::SettingsStoreConfig& baseConfig,
                               const CheckFn check) {
    fxe::SettingsStoreConfig config = baseConfig;
    config.path = "test_settings_store_recovery_hooks.json";
    config.migrateFunc = [](JsonDocument& doc) {
        if (!doc.containsKey("legacy") || doc.containsKey("RECOVERED")) {
            return false;
        }
        doc["RECOVERED"] = doc["legacy"];
        (void)doc.remove("legacy");
        return true;
    };
    config.scrubFunc = [](JsonDocument& doc, fxe::SettingsRemovedKeys* removed) {
        if (!doc.containsKey("REMOVE_ME")) {
            return false;
        }
        if (removed != nullptr && !removed->Record("REMOVE_ME")) return false;
        (void)doc.remove("REMOVE_ME");
        return true;
    };

    RemoveStoreFiles(config.path);
    check(WriteFile(config.path, R"({"RECOVERED":"bad\u0000","PREFERRED":1})"),
          "write recovery-hook primary");
    check(WriteFile("test_settings_store_recovery_hooks.json.bak",
                    R"({"legacy":7,"PREFERRED":9,"REMOVE_ME":4})"),
          "write recovery-hook backup");

    fxe::SettingsStore store(config);
    StaticJsonDocument<fxe::SETTINGS_STORE_JSON_CAPACITY> scratch;
    check(store.Load(&scratch), "load recovery-hook fixture");
    check(store.ReadValue("RECOVERED", static_cast<int32_t>(0)) == 7,
          "migration runs on recovery document");
    check(store.ReadValue("PREFERRED", static_cast<int32_t>(0)) == 1,
          "primary setting wins recovery merge");
    check(!store.Doc().containsKey("REMOVE_ME"), "scrub hook runs on recovery document");
    RemoveStoreFiles(config.path);
}

inline void CheckLargeRecovery(const fxe::SettingsStoreConfig& baseConfig,
                               const CheckFn check) {
    fxe::SettingsStoreConfig config = baseConfig;
    config.path = "test_settings_store_stress.json";
    config.verifyWrites = true;
    RemoveStoreFiles(config.path);

    const std::string undoHistory(7190, 'a');
    const std::string corruptHistory = std::string(7000, 'b') + R"(\u0000)";
    const std::string primary =
        std::string(R"({"BRIGHTNESS":3,"GAME_UNDO_HISTORY":")") +
        corruptHistory + R"("})";
    check(WriteFile(config.path, primary),
          "write near-capacity corrupt recovery primary");
    const std::string backup =
        std::string(R"({"BRIGHTNESS":9,"GAME_UNDO_HISTORY":")") + undoHistory + R"("})";
    check(WriteFile("test_settings_store_stress.json.bak", backup),
          "write large-recovery backup");

    fxe::SettingsStore store(config);
    check(store.Load(), "load large recovery document");
    check(store.ReadValue("BRIGHTNESS", static_cast<int32_t>(0)) == 3,
          "large recovery preserves primary value");
    const char* recovered = store.ReadValue("GAME_UNDO_HISTORY", "");
    check(recovered != nullptr && undoHistory == recovered,
          "large missing undo history recovers exactly");
    fxe::ManualMillis::AdvanceMs(config.mustSaveDelayMs);
    store.Update();
    check(store.GetSaveCount() == 0U, "large boot recovery stays pending without a user action");
    store.Doc()["TEST_USER_ACTION"] = 1U;
    store.MarkMustSaveDirty();
    fxe::ManualMillis::AdvanceMs(config.mustSaveDelayMs);
    store.Update();
    check(store.GetLastSaveSizeBytes() > 7000,
          "large recovery is re-saved through streaming persistence");

    fxe::SettingsStore reloaded(config);
    check(reloaded.Load(), "reload large recovered document");
    const char* restored = reloaded.ReadValue("GAME_UNDO_HISTORY", "");
    check(restored != nullptr && undoHistory == restored,
          "large recovered undo history round-trips exactly");
    RemoveStoreFiles(config.path);
}

inline void CheckRecoveryOverflowPreservesPrimary(
    const fxe::SettingsStoreConfig& baseConfig,
    const CheckFn check) {
    fxe::SettingsStoreConfig config = baseConfig;
    config.path = "test_settings_store_recovery_overflow.json";
    RemoveStoreFiles(config.path);

    const std::string primary = R"({"BRIGHTNESS":3,"BAD":"bad\u0000"})";
    check(WriteFile(config.path, primary), "write overflow-recovery primary");

    std::string backup = "{";
    for (size_t i = 0; i < 500U; ++i) {
        char member[24];
        const int written = std::snprintf(
            member, sizeof(member), "%s\"K%03u\":0", (i == 0U) ? "" : ",",
            static_cast<unsigned>(i));
        check(written > 0 && static_cast<size_t>(written) < sizeof(member),
              "format overflow-recovery member");
        backup += member;
    }
    backup += "}";
    check(backup.size() < fxe::SETTINGS_STORE_MAX_FILE_SIZE_BYTES,
          "overflow-recovery fixture is below file-size bound");
    check(WriteFile("test_settings_store_recovery_overflow.json.bak", backup),
          "write overflow-recovery backup");

    fxe::SettingsStore store(config);
    check(!store.Load(), "recovery arena overflow fails load");
    check(store.GetLastError() == fxe::SettingsStoreError::ResourceExhausted,
          "recovery arena overflow reports resource exhaustion rather than malformed data");
    check(store.ReadValue("BRIGHTNESS", static_cast<int32_t>(0)) == 3,
          "failed recovery restores compacted primary in memory");
    check(!store.Doc().containsKey("K000"),
          "failed recovery does not expose partial alternate data");
    check(ReadFile(config.path) == primary,
          "failed recovery does not overwrite scrubbed primary on disk");
    check(ReadFile("test_settings_store_recovery_overflow.json.bak") == backup,
          "failed recovery preserves backup on disk");
    RemoveStoreFiles(config.path);
}

inline void Run(const fxe::SettingsStoreConfig& config, const CheckFn check) {
    CheckCollisionPolicy(config, check);
    CheckDeduplicatedStringSafety(config, check);
    CheckRecoveryHooks(config, check);
    CheckLargeRecovery(config, check);
    CheckRecoveryOverflowPreservesPrimary(config, check);
}

} // namespace settings_store_scratch_cleanup_test
