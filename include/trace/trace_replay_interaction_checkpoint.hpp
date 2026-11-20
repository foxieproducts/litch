#pragma once

#include <cstdint>

#include "runtime/game_state_machine.hpp"
#include "trace/trace_jsonl_event_reader.hpp"
#include "trace/trace_replay.hpp"

namespace modules {
class GamePersistenceCoordinator;
} // namespace modules

namespace trace {

bool ValidateInteractionCheckpoint(const JsonlEventRecord& record,
                                   const runtime::GameStateMachine& stateMachine,
                                   modules::GamePersistenceCoordinator* gamePersistence,
                                   uint32_t lineNumber,
                                   ReplayResult& outResult);

} // namespace trace
