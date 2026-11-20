#pragma once

#include <cstddef>
#include <cstdint>

#include "console.hpp"
#include "modules/settings_store.hpp"

namespace runtime {
class GameStateMachine;
} // namespace runtime

namespace modules {

class GamePersistenceCoordinator;
class ControlMutationService;

struct GameConsoleCommandContext {
    Console* console = nullptr;
    SettingsStore* settings = nullptr;
    GamePersistenceCoordinator* gamePersistence = nullptr;
    runtime::GameStateMachine* stateMachine = nullptr;
    ControlMutationService* controlMutation = nullptr;
    void* asyncControlTrackingContext = nullptr;
    void (*trackAsyncControlRequest)(void* context, uint16_t requestId) =
        nullptr;
};

bool GameCommand(GameConsoleCommandContext& ctx, const Console::Args& args);
bool GameBenchmarkCommand(GameConsoleCommandContext& ctx, const Console::Args& args);

// The legacy engine-fen alias uses the same bounded argument tail at index 2.
bool FenCommand(GameConsoleCommandContext& ctx,
                 const Console::Args& args,
                 size_t firstFenArgIndex = 1U);

void RegisterGameConsoleCommands(Console& console,
                                 SettingsStore& settings,
                                 GamePersistenceCoordinator& gamePersistence,
                                 runtime::GameStateMachine& stateMachine,
                                 GameConsoleCommandContext& ctx,
                                 ControlMutationService* controlMutation = nullptr);

} // namespace modules
