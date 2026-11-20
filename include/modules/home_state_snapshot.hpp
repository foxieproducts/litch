#pragma once
#include "modules/home_state_notification.hpp"
#include "modules/settings_store.hpp"
namespace runtime { class GameStateMachine; }
namespace modules {
class BeginnerModeController;
class GamePersistenceCoordinator;
HomeStateNotification BuildHomeStateSnapshot(
    const runtime::GameStateMachine& state,
    const GamePersistenceCoordinator& game,
    const SettingsStore& settings,
    const BeginnerModeController& beginnerMode,
    PersistenceDegradation degradation,
    const HomeAssistanceRuntimeInputs& assistanceInputs = {});
// Shared firmware/VirtualLitch publication decision. Sequence allocation and
// unchanged forced replay remain owned by the production publisher.
bool TryPrepareHomeStatePublication(
    const runtime::GameStateMachine& state,
    const GamePersistenceCoordinator& game,
    const SettingsStore& settings,
    const BeginnerModeController& beginnerMode,
    PersistenceDegradation degradation,
    const HomeAssistanceRuntimeInputs& assistanceInputs,
    HomeStateNotificationPublisher& publisher,
    bool force,
    HomeStateNotificationLine& outLine);
}
