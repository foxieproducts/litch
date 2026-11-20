#pragma once

#include "modules/home_state_notification.hpp"

namespace fxe {
class SettingsStore;
}

namespace modules {

class GamePersistenceCoordinator;
class ControlMutationService;

// After a confirmed terminal storage failure, retain the entire live RAM session. Busy
// admission and readable boot repair are not failures. Both production runtime
// and VirtualLitch use this transition before accepting further controls.
bool UpdateRuntimePersistenceSession(
    fxe::SettingsStore& settings, ControlMutationService& controls,
    PersistenceDegradation& degradation, bool workerAvailable = true);

PersistenceDegradation QuarantineUnavailableSettings(
    fxe::SettingsStore& settings, bool loadSucceeded);
bool LoadGameForPersistenceSession(
    fxe::SettingsStore& settings, GamePersistenceCoordinator& game,
    PersistenceDegradation& degradation);

// Preserve durable failure evidence and establish a clean game. A rejected game
// record removes only game/undo fields in RAM and remains eligible for a verified
// repair save; unavailable settings storage falls back to a RAM-only session.
// Firmware and VirtualLitch share this transition so the recovery journey cannot
// drift into a second implementation.
bool BeginVolatilePersistenceSession(
    fxe::SettingsStore& settings,
    GamePersistenceCoordinator& gamePersistence,
    PersistenceDegradation degradation);

// Clear a pending game-record repair only after the settings worker has
// completed and verified the replacement snapshot.
bool NoteVerifiedPersistenceSave(PersistenceDegradation& degradation);

} // namespace modules
