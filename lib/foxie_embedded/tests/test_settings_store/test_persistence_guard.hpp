#pragma once

#include <cstdio>

#include "foxie_embedded/settings_store.hpp"

namespace settings_store_persistence_guard_test {

using CheckFn = void (*)(bool, const char*);

inline void RemoveStoreFiles(const char* path) {
    (void)std::remove(path);

    char backup[96] = {};
    char temporary[96] = {};
    const int backupLength =
        std::snprintf(backup, sizeof(backup), "%s.bak", path);
    const int temporaryLength =
        std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (backupLength > 0 &&
        static_cast<size_t>(backupLength) < sizeof(backup)) {
        (void)std::remove(backup);
    }
    if (temporaryLength > 0 &&
        static_cast<size_t>(temporaryLength) < sizeof(temporary)) {
        (void)std::remove(temporary);
    }
}

inline void CheckSuccessfulGuardedSave(
    const fxe::SettingsStoreConfig& baseConfig,
    const CheckFn check) {
    fxe::SettingsStoreConfig config = baseConfig;
    config.path = "test_settings_store_verify_writes.json";
    config.verifyWrites = true;
    RemoveStoreFiles(config.path);

    fxe::SettingsStore store(config);
    check(store.Load(), "Load succeeds (verify writes)");

    uint32_t persistenceAttempts = 0U;
    store.SetBeforePersistenceCallback(
        [&persistenceAttempts]() { ++persistenceAttempts; });

    store.Doc()["UNRELATED"] = 1U;
    store.MarkDirty();
    check(store.SaveNow(), "unguarded SaveNow succeeds");
    check(persistenceAttempts == 0U,
          "ordinary save does not invoke unarmed persistence guard");
    check(!store.ArmPersistenceGuardIfSavePending(),
          "completed explicit save has no pending guard work");
    check(!store.IsPersistenceGuardArmed(),
          "completed explicit save does not leave a stale guard");

    store.Doc()["BRIGHTNESS"] = 9;
    store.MarkDirty();
    check(store.ArmPersistenceGuardIfSavePending(),
          "pending save can arm persistence guard conditionally");
    check(store.SaveNow(), "SaveNow succeeds with verifyWrites enabled");
    check(persistenceAttempts == 1U,
          "successful armed save invokes persistence guard");
    check(!store.IsPersistenceGuardArmed(),
          "successful save clears persistence guard");
    check(store.GetLastError() == fxe::SettingsStoreError::None,
          "LastError remains None after verified save");

    RemoveStoreFiles(config.path);
}

inline void CheckFailedSaveRetry(
    const fxe::SettingsStoreConfig& baseConfig,
    const CheckFn check) {
    static constexpr const char* BLOCKING_FILE =
        "test_settings_store_not_directory";
    (void)std::remove(BLOCKING_FILE);
    std::FILE* blockingFile = std::fopen(BLOCKING_FILE, "wb");
    check(blockingFile != nullptr,
          "create non-directory persistence blocker");
    if (blockingFile != nullptr) {
        (void)std::fclose(blockingFile);
    }

    fxe::SettingsStoreConfig config = baseConfig;
    config.path = "test_settings_store_not_directory/retry.json";
    config.deferredSaveDelayMs = 0U;

    fxe::SettingsStore store(config);
    uint32_t persistenceAttempts = 0U;
    store.SetBeforePersistenceCallback(
        [&persistenceAttempts]() { ++persistenceAttempts; });
    store.Doc()["BRIGHTNESS"] = 7U;
    store.ArmPersistenceGuard();

    check(!store.SaveNow(),
          "SaveNow fails when parent directory is not a directory");
    check(persistenceAttempts == 1U,
          "explicit failed save invokes persistence guard");
    check(store.IsPersistenceGuardArmed(),
          "failed save preserves persistence guard");

    fxe::ManualMillis::AdvanceMs(config.mustSaveDelayMs);
    store.Update();
    check(persistenceAttempts == 2U,
          "deferred retry invokes persistence guard again");
    check(store.IsPersistenceGuardArmed(),
          "failed retry preserves persistence guard");

    (void)std::remove(BLOCKING_FILE);
}

inline void CheckAsyncAdmissionAndDirectOperations(
    const fxe::SettingsStoreConfig& baseConfig, const CheckFn check) {
    fxe::SettingsStoreConfig config = baseConfig;
    config.path = "test_settings_guard_async.json";
    config.allowSynchronousDeferredSaves = false;
    config.mustSaveDelayMs = 100U;
    RemoveStoreFiles(config.path);
    fxe::SettingsStore store(config);
    uint32_t blockingOperations = 0U;
    uint32_t admissions = 0U;
    using Admission = fxe::SettingsStore::AsyncSaveAdmission;
    auto admission = Admission::Deferred;
    store.SetBeforePersistenceCallback([&]() { ++blockingOperations; });
    store.SetAsyncSaveAdmissionCallback([&](const JsonDocument&) {
        ++admissions;
        return admission;
    });
    store.Doc()["VALUE"] = 7U;
    store.ArmPersistenceGuard();
    store.MarkMustSaveDirty();
    for (uint32_t elapsed = 0U; elapsed <= 5000U; elapsed += 20U) {
        store.Update();
        fxe::ManualMillis::AdvanceMs(20U);
    }
    check(admissions > 200U && blockingOperations == 0U &&
              store.IsPersistenceGuardArmed() &&
              !store.HasTerminalSaveFailure() && store.GetSaveCount() == 0U,
          "five seconds of Deferred admission retains intent without blocking guard or false failure");
    admission = Admission::Rejected;
    check(!store.SaveNow() && blockingOperations == 0U &&
              store.IsPersistenceGuardArmed() && !store.HasTerminalSaveFailure(),
          "consumer rejection retains retry and armed intent without disabling shared saving");
    admission = Admission::Accepted;
    check(store.SaveNow() && blockingOperations == 0U &&
              !store.IsPersistenceGuardArmed() && !store.HasTerminalSaveFailure(),
          "queue acceptance clears intent without invoking blocking guard or inventing a terminal failure");
    store.ReportAsyncSaveFailure(fxe::SettingsStoreError::WriteFailed,
        fxe::SettingsStore::StorageFailureEvidence::Confirmed);
    check(store.HasTerminalSaveFailure(), "actual shared storage failure retains its terminal guard");
    store.ReportAsyncSaveSuccess(100U, 1U);
    check(!store.HasTerminalSaveFailure(),
          "only verified async success clears terminal failure");

    const uint32_t beforeDirectOperations = blockingOperations;
    store.ArmPersistenceGuard();
    check(store.SaveNowBlocking() && blockingOperations == beforeDirectOperations + 1U &&
              !store.IsPersistenceGuardArmed(),
          "direct blocking save keeps guard even with async callback installed");
    store.ArmPersistenceGuard();
    check(store.Load() && blockingOperations == beforeDirectOperations + 2U,
          "direct load keeps guard even with async callback installed");
    store.ArmPersistenceGuard();
    check(store.CorruptFileForTest() && blockingOperations == beforeDirectOperations + 3U,
          "direct test corruption keeps guard even with async callback installed");
    store.ArmPersistenceGuard();
    check(store.DeleteFile() && blockingOperations == beforeDirectOperations + 4U,
          "direct deletion keeps guard even with async callback installed");
    store.SetAsyncSaveAdmissionCallback(nullptr);
    store.ArmPersistenceGuard();
    check(store.SaveNow() && blockingOperations == beforeDirectOperations + 5U,
          "removing async admission restores the guarded synchronous path");
    std::printf("[ASYNC_GUARD] admissions=%u blocking_callbacks_before_direct=%u direct_callbacks=%u\n",
                admissions, beforeDirectOperations,
                blockingOperations - beforeDirectOperations);
    RemoveStoreFiles(config.path);
}

inline void Run(const fxe::SettingsStoreConfig& config,
                const CheckFn check) {
    CheckSuccessfulGuardedSave(config, check);
    CheckFailedSaveRetry(config, check);
    CheckAsyncAdmissionAndDirectOperations(config, check);
}

} // namespace settings_store_persistence_guard_test
