#pragma once

#include <cstddef>
#include <cstdint>

#include "console.hpp"

namespace runtime {
class ManualClock;
class GameStateMachine;
} // namespace runtime

namespace fxe {
class SettingsStore;
class Console;
} // namespace fxe

namespace trace {
class TraceRecorder;
} // namespace trace

namespace modules {

class GamePersistenceCoordinator;

struct TraceConsoleCommandContext {
    fxe::Console* console = nullptr;
    fxe::SettingsStore* settings = nullptr;
    runtime::GameStateMachine* stateMachine = nullptr;
    GamePersistenceCoordinator* gamePersistence = nullptr;

    trace::TraceRecorder* recorder = nullptr;

    const char* buildId = nullptr;
    const char* boardId = nullptr;
    uint32_t rngSeed = 0;

#ifndef ARDUINO
    runtime::ManualClock* replayClock = nullptr;
#endif
};

bool TraceCommand(TraceConsoleCommandContext& ctx, const fxe::Console::Args& args);

void RegisterTraceConsoleCommands(fxe::Console& console,
                                  fxe::SettingsStore& settings,
                                  runtime::GameStateMachine& stateMachine,
                                  GamePersistenceCoordinator* gamePersistence,
                                  trace::TraceRecorder* recorder,
                                  const char* buildId,
                                  const char* boardId,
                                  uint32_t rngSeed,
                                  TraceConsoleCommandContext& ctx
#ifndef ARDUINO
                                  ,
                                  runtime::ManualClock* replayClock
#endif
);

} // namespace modules
