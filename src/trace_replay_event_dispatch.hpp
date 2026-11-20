#pragma once

#include "modules/game_persistence_coordinator.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/interaction_dispatcher.hpp"

namespace trace {

inline void DispatchReplayAppEvent(
    runtime::GameStateMachine& stateMachine,
    modules::GamePersistenceCoordinator* const gamePersistence,
    const runtime::AppEvent& appEvent) {
    if (runtime::InteractionDispatcher::
            TryDispatchCoordinatorBoardPolicy(
                gamePersistence, stateMachine, appEvent)) {
        return;
    }
    runtime::DispatchAppEventToStateMachine(stateMachine, appEvent);
}

} // namespace trace
