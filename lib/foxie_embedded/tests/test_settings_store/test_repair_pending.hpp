#pragma once

namespace settings_store_repair_pending_test {

inline void CheckRetryPreservesOnlyRecoveredAuthority(const std::string& root) {
    const std::string primary = root + "/authority.json";
    const std::string pending = primary + ".tmp";
    const std::string backup = primary + ".bak";
    const std::string retry = primary + ".retry";
    for (const auto& blocked : {primary, backup}) {
        std::filesystem::create_directory(blocked);
        settings_store_scoped_recovery_test::WriteFile((blocked + "/block").c_str(), "blocked");
    }
    settings_store_scoped_recovery_test::WriteFile(pending.c_str(), "{\"VALUE\":2}");
    fxe::SettingsStoreConfig config; config.path = primary.c_str();
    fxe::SettingsStore boot(config);
    Check(boot.Load() && boot.IsLoadRepairPending() && boot.ReadValue("VALUE", 0U) == 2U,
          "only valid pending authority loads despite blocked promotion");

    // Firmware retries from a separate worker store. Interrupt its newly
    // written candidate before verification, whichever scratch name it uses.
    fxe::SettingsStore worker(config);
    worker.Doc()["VALUE"] = 3U;
    worker.SetStorageAccessCallbacks(nullptr, [&]() {
        for (const auto& scratch : {pending, retry}) {
            if (ReadFileToString(scratch.c_str()).find("\"VALUE\":3") != std::string::npos) {
                settings_store_scoped_recovery_test::WriteFile(scratch.c_str(), "interrupted");
            }
        }
    });
    Check(!worker.SaveNowBlocking(), "worker retry fails before a replacement verifies");
    Check(ReadFileToString(pending.c_str()) == "{\"VALUE\":2}",
          "failed retry never truncates the only selected recovered authority");
    fxe::SettingsStore coldBoot(config);
    Check(coldBoot.Load() && coldBoot.ReadValue("VALUE", 0U) == 2U,
          "cold boot after interrupted retry restores the selected recovered values");
    std::filesystem::remove_all(primary);
    std::filesystem::remove_all(backup);
    worker.SetStorageAccessCallbacks(nullptr, nullptr);
    Check(worker.SaveNowBlocking() && !std::filesystem::exists(pending) &&
              !std::filesystem::exists(retry),
          "successful retry installs primary before retiring the old pending authority");
    fxe::SettingsStore savedBoot(config);
    Check(savedBoot.Load() && savedBoot.ReadValue("VALUE", 0U) == 3U,
          "cold boot after completed retry selects the new saved values");
    settings_store_scoped_recovery_test::WriteFile(retry.c_str(), "orphaned scratch");
    Check(worker.DeleteFile() && !std::filesystem::exists(retry),
          "explicit settings deletion also removes the bounded retry scratch");
}

inline void CheckTransientReadRecovery(const std::string& root) {
    const std::string primary = root + "/transient.json";
    const std::string pending = primary + ".tmp";
    const std::string held = root + "/held.json";
    settings_store_scoped_recovery_test::WriteFile(primary.c_str(), "{\"VALUE\":1}");
    settings_store_scoped_recovery_test::WriteFile(held.c_str(), "{\"VALUE\":2}");
    std::filesystem::create_directory(pending);
    settings_store_scoped_recovery_test::WriteFile((pending + "/block").c_str(), "blocked");
    fxe::SettingsStoreConfig config; config.path = primary.c_str();
    fxe::SettingsStore store(config);
    unsigned accesses = 0U;
    store.SetStorageAccessCallbacks(nullptr, [&]() {
        ++accesses;
        if (std::filesystem::is_directory(pending)) {
            std::filesystem::remove_all(pending);
            std::filesystem::rename(held, pending);
        }
    });
    Check(store.Load() && store.ReadValue("VALUE", 0U) == 2U,
          "transient read failure retries and recovers the newer pending document");
    Check(accesses >= 2U && store.GetLastError() == fxe::SettingsStoreError::None,
          "successful bounded read retry avoids a false permanent storage failure");
    settings_store_scratch_cleanup_test::RemoveStoreFiles(primary.c_str());
}

inline void CheckStaleErrorAndRetryRecovery(const std::string& root) {
    const std::string primary = root + "/later.json";
    const std::string pending = primary + ".tmp";
    std::filesystem::create_directory(pending);
    settings_store_scoped_recovery_test::WriteFile((pending + "/block").c_str(), "blocked");
    fxe::SettingsStoreConfig config; config.path = primary.c_str();
    fxe::SettingsStore source(config);
    source.Doc()["VALUE"] = 1U;
    Check(!source.SaveNowBlocking(), "real initial synchronous write failure is observable");
    std::string queued;
    unsigned requests = 0U;
    source.SetAsyncSaveRequestCallback([&](const JsonDocument& doc) {
        ++requests; queued.clear(); serializeJson(doc, queued); return true;
    });
    for (unsigned failure = 0U; failure < 8U; ++failure) {
        source.ReportAsyncSaveFailure(fxe::SettingsStoreError::WriteFailed);
        source.Update();
        const unsigned before = requests;
        source.Doc()["VALUE"] = failure + 2U;
        source.MarkDirty();
        constexpr uint32_t delays[] = {200U, 400U, 800U, 1600U, 3200U, 5000U, 5000U, 5000U};
        fxe::ManualMillis::AdvanceMs(delays[failure] - 1U); source.Update();
        Check(requests == before, "increasing failure cooldown survives further edits and prevents hot retrying");
        fxe::ManualMillis::AdvanceMs(1U); source.Update();
        Check(requests == before + 1U && !source.IsDisabled(),
              "exhausted attempts retain a bounded future retry rather than disabling saving");
    }
    std::filesystem::remove_all(pending);
    fxe::SettingsStore writer(config);
    Check(!deserializeJson(writer.Doc(), queued) && writer.SaveNowBlocking(),
          "a later worker attempt persists the latest edited document");
    source.ReportAsyncSaveSuccess(writer.GetLastSaveSizeBytes(), writer.GetPersistedSaveTotal());
    Check(source.GetLastError() == fxe::SettingsStoreError::None,
          "verified async recovery clears the previous synchronous error too");
    fxe::SettingsStore reboot(config);
    Check(reboot.Load() && reboot.ReadValue("VALUE", 0U) == 9U,
          "later successful retry survives reboot with the newest preferences");
    settings_store_scratch_cleanup_test::RemoveStoreFiles(primary.c_str());
}

inline void CheckUnavailableReadClassification(const std::string& root) {
    const std::string primary = root + "/unavailable.json";
    const std::string pending = primary + ".tmp";
    settings_store_scoped_recovery_test::WriteFile(primary.c_str(), "{\"VALUE\":1}");
    std::filesystem::create_symlink(pending, pending);
    fxe::SettingsStoreConfig config; config.path = primary.c_str();
    fxe::SettingsStore store(config);
    unsigned attempts = 0U;
    store.SetStorageAccessCallbacks(nullptr, [&]() { ++attempts; });
    Check(store.Load() && !store.IsDisabled() && store.IsLoadRepairPending() &&
              store.ReadValue("VALUE", 0U) == 1U &&
              store.GetLastError() == fxe::SettingsStoreError::OpenReadFailed,
          "unavailable pending record cannot suppress valid primary settings or masquerade as missing");
    Check(attempts == 4U && std::filesystem::is_symlink(pending) &&
              ReadFileToString(primary.c_str()) == "{\"VALUE\":1}",
          "three pending read attempts plus valid primary preserve both artifacts during load");
    std::filesystem::remove(pending);
    Check(store.Load() && store.ReadValue("VALUE", 0U) == 1U,
          "a subsequent explicit load can recover after the file error disappears");
    settings_store_scratch_cleanup_test::RemoveStoreFiles(primary.c_str());

    std::filesystem::create_symlink(primary, primary);
    attempts = 0U;
    Check(!store.Load() && store.GetLastError() == fxe::SettingsStoreError::OpenReadFailed,
          "unavailable only copy cannot become a successful defaults load");
    Check(attempts == 5U && std::filesystem::is_symlink(primary),
          "missing pending and backup plus three primary opens retain the unavailable record");
    std::filesystem::remove(primary);
}

inline void CheckNonRetryableLoadBounds(const std::string& root) {
    const std::string primary = root + "/bounds.json";
    const std::string pending = primary + ".tmp";
    fxe::SettingsStoreConfig config; config.path = primary.c_str();
    for (const bool tooLarge : {false, true}) {
        settings_store_scoped_recovery_test::WriteFile(primary.c_str(), "{\"VALUE\":3}");
        std::string contents = "{";
        for (unsigned i = 0U; i < 500U; ++i) {
            contents += (i == 0U ? "\"K" : ",\"K") + std::to_string(i) + "\":0";
        }
        contents += "}";
        if (tooLarge) contents.append(fxe::SETTINGS_STORE_MAX_FILE_SIZE_BYTES, ' ');
        settings_store_scoped_recovery_test::WriteFile(pending.c_str(), contents.c_str());
        fxe::SettingsStore store(config);
        unsigned attempts = 0U;
        store.SetStorageAccessCallbacks(nullptr, [&]() { ++attempts; });
        Check(store.Load() && store.ReadValue("VALUE", 0U) == 3U && store.IsLoadRepairPending(),
              "unsupported alternate size or memory demand retains valid primary values");
        Check(attempts == 2U && store.GetLastError() ==
                  (tooLarge ? fxe::SettingsStoreError::FileTooLarge : fxe::SettingsStoreError::ResourceExhausted),
              "capacity and size failures are classified without pointless read retries");
        Check(ReadFileToString(pending.c_str()) == contents,
              "unsupported alternate remains intact until a replacement is saved");
        settings_store_scratch_cleanup_test::RemoveStoreFiles(primary.c_str());
    }
}

inline void Run() {
    char directory[] = "/tmp/fxe-repair-XXXXXX";
    Check(mkdtemp(directory) != nullptr, "repair fixture creates an isolated directory");
    const std::string root = directory;
    CheckRetryPreservesOnlyRecoveredAuthority(root);
    CheckTransientReadRecovery(root);
    CheckStaleErrorAndRetryRecovery(root);
    CheckUnavailableReadClassification(root);
    CheckNonRetryableLoadBounds(root);
    const std::string primary = root + "/settings.json";
    const std::string backup = primary + ".bak";
    const std::string pending = primary + ".tmp";
    fxe::SettingsStoreConfig config;
    config.path = primary.c_str();
    for (const bool recoverBackup : {false, true}) {
        const std::string blocked = recoverBackup ? primary : backup;
        const std::string candidate = recoverBackup ? backup : pending;
        std::filesystem::create_directory(blocked);
        settings_store_scoped_recovery_test::WriteFile(
            (blocked + "/block").c_str(), "occupied");
        if (!recoverBackup) {
            settings_store_scoped_recovery_test::WriteFile(primary.c_str(), "{\"VALUE\":1}");
        }
        settings_store_scoped_recovery_test::WriteFile(candidate.c_str(), "{\"VALUE\":2}");
        fxe::SettingsStore store(config);
        Check(store.Load() && store.IsLoadRepairPending() && !store.IsDisabled(),
              "valid alternate stays enabled with repair pending until user action");
        Check(!store.HasConfirmedStorageFailure(),
              "directory/path refusal is not evidence of flash failure");
        Check(store.ReadValue("VALUE", 0U) == 2U &&
                  store.GetLastError() == (recoverBackup ? fxe::SettingsStoreError::ReadFailed
                                                       : fxe::SettingsStoreError::None),
              "pending recovery preserves value and read errors without inventing an attempted save");
        Check(ReadFileToString(candidate.c_str()) == "{\"VALUE\":2}",
              "failed repair preserves the valid alternate file");
        if (!recoverBackup) {
            Check(!store.SaveNowBlocking() &&
                      ReadFileToString(pending.c_str()).find("\"VALUE\":2") != std::string::npos,
                  "failed save promotion preserves its verified temporary candidate for reboot");
        }
        std::filesystem::remove_all(blocked);
        Check(store.SaveNowBlocking() && !store.IsLoadRepairPending() &&
                  store.GetLastError() == fxe::SettingsStoreError::None,
              "verified synchronous recovery clears pending repair and error");
        fxe::SettingsStore reboot(config);
        Check(reboot.Load() && !reboot.IsLoadRepairPending() &&
                  reboot.ReadValue("VALUE", 0U) == 2U,
              "reboot selects the successfully repaired document");
        std::filesystem::remove(primary);
        std::filesystem::remove(backup);
        std::filesystem::remove(pending);
    }
    std::filesystem::remove(root);
}

} // namespace settings_store_repair_pending_test
