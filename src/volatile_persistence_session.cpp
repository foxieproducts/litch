#include "modules/volatile_persistence_session.hpp"

#include "foxie_embedded/settings_store.hpp"
#include "modules/game_persistence.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/game_persistence_coordinator.hpp"

namespace modules {

namespace {
void DisablePreservingPowerLatch(fxe::SettingsStore& settings) {
    // Game quarantine must not erase positively known power protection. The
    // original document stays on disk; retain only this safety fact in the new
    // RAM session. Unreadable settings alone are not power-fault evidence.
    const bool brownoutLatched =
        settings.ReadValue("LED_BROWNOUT_LIMIT_LATCH", false);
    settings.DisableForBoot();
    if (brownoutLatched) {
        settings.Doc()["LED_BROWNOUT_LIMIT_LATCH"] = true;
    }
}
}
PersistenceDegradation QuarantineUnavailableSettings(
    fxe::SettingsStore& settings, const bool loadSucceeded) {
    if (loadSucceeded && !settings.IsDisabled() &&
        (settings.GetLastError() == fxe::SettingsStoreError::None ||
         settings.IsLoadRepairPending())) {
        return PersistenceDegradation::None;
    }
    DisablePreservingPowerLatch(settings);
    return settings.HasConfirmedStorageFailure()
        ? PersistenceDegradation::StorageIo : PersistenceDegradation::None;
}

bool LoadGameForPersistenceSession(
    fxe::SettingsStore& settings, GamePersistenceCoordinator& game,
    PersistenceDegradation& degradation) {
    if (game.LoadOrInitialize()) {
        return true;
    }
    degradation = PersistenceDegradation::QuarantinedRecord;
    return BeginVolatilePersistenceSession(settings, game, degradation);
}

bool BeginVolatilePersistenceSession(
    fxe::SettingsStore& settings,
    GamePersistenceCoordinator& gamePersistence,
    const PersistenceDegradation degradation) {
    if (degradation == PersistenceDegradation::None) {
        return false;
    }

    if (degradation == PersistenceDegradation::QuarantinedRecord &&
        !settings.IsDisabled()) {
        // A rejected current-game record is not evidence that the settings
        // filesystem is unavailable. Retain preferences, safety settings and
        // Litchcraft artwork, replace only the coupled game/undo record, and
        // let the ordinary verified settings worker make the repair durable.
        // Until that save completes, the original file remains untouched and
        // available as failure evidence.
        ClearGameState(settings.Doc());
        ClearUndoState(settings.Doc());
        settings.MarkMustSaveDirty();
        return gamePersistence.LoadOrInitialize();
    }

    DisablePreservingPowerLatch(settings);
    return gamePersistence.LoadOrInitialize();
}

bool UpdateRuntimePersistenceSession(
    fxe::SettingsStore& settings, ControlMutationService& controls,
    PersistenceDegradation& degradation, const bool workerAvailable) {
    if (!workerAvailable || settings.HasTerminalSaveFailure()) {
        settings.DisablePersistenceForSession();
    }
    if (!settings.IsDisabled()) return false;
    controls.AllowVolatileUserSettings();
    const auto next = settings.HasConfirmedStorageFailure()
        ? PersistenceDegradation::StorageIo : PersistenceDegradation::None;
    const bool changed = degradation != next;
    degradation = next;
    return changed;
}

bool NoteVerifiedPersistenceSave(PersistenceDegradation& degradation) {
    if (degradation != PersistenceDegradation::QuarantinedRecord) {
        return false;
    }
    degradation = PersistenceDegradation::None;
    return true;
}

} // namespace modules
