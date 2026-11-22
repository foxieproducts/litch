#pragma once

#include <cstddef>
#include <cstdint>

#include "runtime/clock.hpp"
#include "runtime/game_state_machine.hpp"

namespace modules {
class GamePersistenceCoordinator;
class IEngine;
} // namespace modules

namespace trace {

struct ReplayResult {
    bool ok = true;
    uint32_t failureLine = 0;
    char message[128] = {};
};

// Replays a JSONL trace (as an array of lines) into the state machine using the
// trace time as the single time source.
//
// Supported record types match the current TraceRecorder output:
// - header (ignored)
// - settings (ignored)
// - tick -> sm.Tick()
// - sensor -> sm.HandleSensorChange(square, occ)
// - button -> sm.HandleButtonEvent(button, edge)
// - control -> ordered Undo/Hint/Reset command (requires gamePersistence)
// - checkpoint_state -> compares expected game_state
// - checkpoint_interaction/checkpoint_variant -> compare deterministic state
// - checkpoint_engine -> compares expected fen (requires engine)
// - checkpoint_frame -> compares expected semantic LED frame hash (board/edge/all)
// - end (ignored)
// - cmd -> optionally applies known mutations (requires gamePersistence)
bool ReplayJsonLines(const char* const* lines,
                     size_t lineCount,
                     runtime::ManualClock& clock,
                     runtime::GameStateMachine& sm,
                     ReplayResult& outResult,
                     modules::GamePersistenceCoordinator* gamePersistence = nullptr,
                     modules::IEngine* engine = nullptr);

#ifndef ARDUINO
// Convenience wrapper for host-based replay.
bool ReplayJsonlFile(const char* path,
                     runtime::ManualClock& clock,
                     runtime::GameStateMachine& sm,
                     ReplayResult& outResult,
                     modules::GamePersistenceCoordinator* gamePersistence = nullptr,
                     modules::IEngine* engine = nullptr);
#endif

} // namespace trace
