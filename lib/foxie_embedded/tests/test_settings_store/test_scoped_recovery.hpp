#pragma once

namespace settings_store_scoped_recovery_test {

inline void WriteFile(const char* path, const char* contents) {
    std::FILE* file = std::fopen(path, "wb");
    Check(file != nullptr, "scoped recovery fixture opens file");
    if (file == nullptr) return;
    const size_t size = std::strlen(contents);
    Check(std::fwrite(contents, 1U, size, file) == size,
          "scoped recovery fixture writes complete JSON");
    (void)std::fclose(file);
}

inline void Run() {
    constexpr const char* primary = "test_settings_scoped_recovery.json";
    constexpr const char* pending = "test_settings_scoped_recovery.json.tmp";
    constexpr const char* backup = "test_settings_scoped_recovery.json.bak";
    for (const bool useTmp : {false, true}) {
        for (const bool validPrimaryRecord : {false, true}) {
            for (const bool validBackupRecord : {false, true}) {
                (void)std::remove(primary);
                (void)std::remove(pending);
                (void)std::remove(backup);
                WriteFile(primary, validPrimaryRecord
                    ? R"({"CHESS":2,"ART":7})" : R"({"CHESS":2,"ART":99})");
                WriteFile(backup, validBackupRecord
                    ? R"({"CHESS":1,"ART":7})" : R"({"CHESS":1,"ART":99})");
                if (useTmp) WriteFile(pending, R"({"CHESS":3,"ART":99})");
                const auto originalPrimary = ReadFileToString(primary);
                const auto originalPending = ReadFileToString(pending);
                const auto originalBackup = ReadFileToString(backup);
                fxe::SettingsStoreConfig config;
                config.path = primary;
                // Stand in for a project-owned version/integrity validator:
                // JSON can parse while one independent record is invalid.
                config.scrubFunc = [](JsonDocument& document, fxe::SettingsRemovedKeys* removed) {
                    if (document.containsKey("ART") && document["ART"] != 7) {
                        if (removed != nullptr && !removed->Record("ART")) return false;
            document.remove("ART");
                        return true;
                    }
                    return false;
                };
                fxe::SettingsStore store(config);
                Check(store.Load(), "scoped recovery retains usable settings");
                const uint32_t expectedChess = useTmp ? 3U : 2U;
                const uint32_t expectedArt = validPrimaryRecord || validBackupRecord ? 7U : 0U;
                Check(store.ReadValue("CHESS", 0U) == expectedChess,
                      "invalid art never rolls back the selected valid chess record");
                Check(store.ReadValue("ART", 0U) == expectedArt,
                      "removed art recovers only from a validated alternate record");
                Check(ReadFileToString(primary) == originalPrimary &&
                          ReadFileToString(pending) == originalPending &&
                          ReadFileToString(backup) == originalBackup,
                      "semantic recovery keeps all source files until a repaired save");
                fxe::SettingsStore rebootBeforeSave(config);
                Check(rebootBeforeSave.Load() &&
                          rebootBeforeSave.ReadValue("CHESS", 0U) == expectedChess &&
                          rebootBeforeSave.ReadValue("ART", 0U) == expectedArt,
                      "power loss before repair repeats the same scoped recovery");
                Check(store.SaveNow(), "repaired settings save succeeds");
                fxe::SettingsStore rebootAfterSave(config);
                Check(rebootAfterSave.Load() &&
                          rebootAfterSave.ReadValue("CHESS", 0U) == expectedChess &&
                          rebootAfterSave.ReadValue("ART", 0U) == expectedArt,
                      "repaired snapshot survives reboot without resurrecting invalid art");
            }
        }
    }
    for (const bool useTmp : {false, true}) {
        (void)std::remove(primary);
        (void)std::remove(pending);
        (void)std::remove(backup);
        WriteFile(primary, R"({"CHESS":2,"art":99})");
        WriteFile(backup, R"({"CHESS":1,"art":7})");
        if (useTmp) WriteFile(pending, R"({"CHESS":3,"art":99})");
        const auto rawPrimary = ReadFileToString(primary);
        const auto rawBackup = ReadFileToString(backup);
        fxe::SettingsStoreConfig config;
        config.path = primary;
        config.scrubFunc = [](JsonDocument& document, fxe::SettingsRemovedKeys* removed) {
            if (!document.containsKey("ART") || document["ART"] == 7) return false;
            if (removed != nullptr && !removed->Record("ART")) return false;
            document.remove("ART");
            return true;
        };
        fxe::SettingsStore store(config);
        Check(store.Load() && store.ReadValue("ART", 0U) == 7U &&
                  store.ReadValue("CHESS", 0U) == (useTmp ? 3U : 2U),
              "noncanonical keys cross project validation before alternate merge");
        Check(ReadFileToString(primary) == rawPrimary && ReadFileToString(backup) == rawBackup,
              "noncanonical corrupt artwork cannot replace valid recovery sources");
    }
    for (const bool missingPrimary : {false, true}) {
        (void)std::remove(primary);
        (void)std::remove(pending);
        (void)std::remove(backup);
        if (!missingPrimary) WriteFile(primary, "{broken");
        WriteFile(backup, R"({"CHESS":4,"ART":99})");
        const auto rawBackup = ReadFileToString(backup);
        const auto rawPrimary = ReadFileToString(primary);
        fxe::SettingsStoreConfig config;
        config.path = primary;
        config.scrubFunc = [](JsonDocument& document, fxe::SettingsRemovedKeys* removed) {
            if (!document.containsKey("ART")) return false;
            if (removed != nullptr && !removed->Record("ART")) return false;
            document.remove("ART");
            return true;
        };
        fxe::SettingsStore store(config);
        Check(store.Load() && store.ReadValue("CHESS", 0U) == 4U &&
                  !store.Doc().containsKey("ART"),
              "fallback backup crosses project validation before becoming authority");
        Check(ReadFileToString(primary) == rawPrimary &&
                  ReadFileToString(backup) == rawBackup,
              "invalid fallback backup is retained without destructive promotion");
    }
    (void)std::remove(primary);
    (void)std::remove(pending);
    (void)std::remove(backup);
    WriteFile(pending, R"({"CHESS":5,"ART":7})");
    fxe::SettingsStoreConfig config;
    config.path = primary;
    uint32_t calls = 0U;
    config.migrateFunc = [&calls](JsonDocument& document) {
        ++calls;
        document["MIGRATIONS"] = (document["MIGRATIONS"] | 0U) + 1U;
        return true;
    };
    fxe::SettingsStore migrated(config);
    Check(migrated.Load() && calls == 1U &&
              migrated.ReadValue("MIGRATIONS", 0U) == 1U,
          "selected tmp invokes each migration exactly once per load");
    (void)std::remove(primary);
    (void)std::remove(pending);
    (void)std::remove(backup);
}

} // namespace settings_store_scoped_recovery_test
