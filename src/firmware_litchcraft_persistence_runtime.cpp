#include "firmware/litchcraft_persistence_runtime.hpp"

#ifdef ARDUINO

#include "firmware/firmware_internal.hpp"
#include "logger.hpp"
#include "firmware/sleep_entry_orchestration.hpp"

namespace firmware_internal {
namespace {

using ArtPersistence = modules::LitchcraftPersistence;
using Worker = firmware::SettingsPersistenceWorker;

fxe::SettingsStore::AsyncSaveAdmission Enqueue(
    void* context, const JsonDocument& document, uint32_t& receipt) {
    auto& sys = *static_cast<FirmwareSystem*>(context);
    auto admission = fxe::SettingsStore::AsyncSaveAdmission::Rejected;
    if (!sys.settings.IsDisabled()) {
        (void)sys.settingsPersistence.EnqueueTracked(document, receipt, &admission);
    }
    return admission;
}

ArtPersistence::SaveOutcome Poll(void* context, const uint32_t receipt) {
    auto& sys = *static_cast<FirmwareSystem*>(context);
    switch (sys.settingsPersistence.GetSaveOutcome(receipt)) {
        case Worker::SaveOutcome::Pending: return ArtPersistence::SaveOutcome::Pending;
        case Worker::SaveOutcome::Saved: return ArtPersistence::SaveOutcome::Saved;
        case Worker::SaveOutcome::Unknown:
        case Worker::SaveOutcome::Failed: return ArtPersistence::SaveOutcome::Failed;
    }
    return ArtPersistence::SaveOutcome::Failed;
}

void Release(void* context, const uint32_t receipt) {
    auto& sys = *static_cast<FirmwareSystem*>(context);
    (void)sys.settingsPersistence.ReleaseSaveReceipt(receipt);
}

} // namespace

void ConfigureLitchcraftPersistence(FirmwareSystem& sys) {
    auto& persistence = sys.gamePersistence.ArtPersistence();
    // Keep the gate configured even if the worker failed to start. A failed
    // store must never restore the old synchronous-success exit behavior.
#if defined(LITCH_ESP32S3)
    persistence.ConfigureStoreOnly({&sys, &Enqueue, &Poll, &Release});
#else
    persistence.Configure({&sys, &Enqueue, &Poll, &Release});
    const auto restored = persistence.Restore(
        sys.stateMachine.GetPhysicalOccupancyMask(),
        sys.startupBoardSnapshotObserved);
    const bool staticStorageRecovered = sys.litchcraftStorageRecovered ||
        restored == modules::LitchcraftRecordStatus::Invalid ||
        restored == modules::LitchcraftRecordStatus::FutureVersion;
    if (staticStorageRecovered) {
        sys.litchcraftStorageRecovered = true;
        logger::Warn("litchcraft: invalid stored artwork found; usable settings retained");
    }
#endif
}

bool BeginLifecyclePersistence(FirmwareSystem& sys) {
    if (sys.settings.IsDisabled() || !sys.settingsPersistence.IsStarted() ||
        sys.gamePersistence.ArtPersistence().FlushState() ==
            modules::LitchcraftPersistence::FlushStatus::Pending ||
        !sys.gamePersistence.PrepareSettingsSnapshotForLifecycle()) {
        return false;
    }
#if defined(LITCH_ESP32S3) || defined(LITCH_REBOOT_TEST_STORE)
    sys.settingsPersistence.LitchcraftFrameStream().RequestImmediateSave();
#endif
    sys.gamePersistence.ArtPersistence().RequestFlush();
    return true;
}

bool LifecycleStoreSavePending(const FirmwareSystem& sys) {
#if defined(LITCH_ESP32S3) || defined(LITCH_REBOOT_TEST_STORE)
    return
#if defined(LITCH_ESP32S3)
        sys.settingsPersistence.LitchcraftFrameClearPending() ||
#endif
        sys.settingsPersistence.LitchcraftFrameStream().HasUnpersistedEdit() ||
        sys.settingsPersistence.LitchcraftFrameModeSavePending() ||
        sys.settingsPersistence.LitchcraftFrameCommitPending();
#else
    (void)sys;
    return false;
#endif
}

constexpr uint32_t SETTINGS_SLEEP_FLUSH_TIMEOUT_MS = 10000U;

bool BeginPersistenceBeforeSleep(void* context) {
    auto& sys = *static_cast<FirmwareSystem*>(context);
    sys.sleepPersistenceElapsed.Reset();
    if (!sys.runtimeBootHealth.commitFinished) {
        logger::Warn("sleep: waiting for startup checks; sleep will continue automatically");
    }
    if (sys.settings.IsDisabled()) {
        logger::Warn("sleep: explicit power-off discards the session-only changes");
        return true;
    }
    return BeginLifecyclePersistence(sys);
}

firmware::SleepPersistenceStatus PollPersistenceBeforeSleep(void* context) {
    const auto& sys = *static_cast<FirmwareSystem*>(context);
    if (!sys.runtimeBootHealth.commitFinished) {
        if (sys.sleepPersistenceElapsed.HasElapsed(SETTINGS_SLEEP_FLUSH_TIMEOUT_MS)) {
            logger::Warn("sleep: startup checks did not finish; sleep cancelled; retry after startup completes");
            return firmware::SleepPersistenceStatus::Failed;
        }
        return firmware::SleepPersistenceStatus::Pending;
    }
    if (sys.settings.IsDisabled()) {
        if (!sys.settingsPersistence.IsStarted() || sys.settingsPersistence.IsIdle()) {
            return firmware::SleepPersistenceStatus::Succeeded;
        }
        return sys.sleepPersistenceElapsed.HasElapsed(SETTINGS_SLEEP_FLUSH_TIMEOUT_MS)
            ? firmware::SleepPersistenceStatus::Failed
            : firmware::SleepPersistenceStatus::Pending;
    }
    if (LifecycleStoreSavePending(sys)) {
        return sys.sleepPersistenceElapsed.HasElapsed(SETTINGS_SLEEP_FLUSH_TIMEOUT_MS)
                   ? firmware::SleepPersistenceStatus::Failed
                   : firmware::SleepPersistenceStatus::Pending;
    }
    switch (sys.gamePersistence.ArtPersistence().FlushState()) {
        case modules::LitchcraftPersistence::FlushStatus::Saved:
            return firmware::SleepPersistenceStatus::Succeeded;
        case modules::LitchcraftPersistence::FlushStatus::Idle:
        case modules::LitchcraftPersistence::FlushStatus::Failed:
            return firmware::SleepPersistenceStatus::Failed;
        case modules::LitchcraftPersistence::FlushStatus::Pending:
            return sys.sleepPersistenceElapsed.HasElapsed(SETTINGS_SLEEP_FLUSH_TIMEOUT_MS)
                       ? firmware::SleepPersistenceStatus::Failed
                       : firmware::SleepPersistenceStatus::Pending;
    }
    return firmware::SleepPersistenceStatus::Failed;
}

} // namespace firmware_internal

#endif
