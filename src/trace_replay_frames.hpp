#pragma once

#include <cstdint>

namespace modules {
class EngineDerivedState;
class GamePersistenceCoordinator;
} // namespace modules

namespace runtime {
class GameStateMachine;
} // namespace runtime

namespace trace::replay_internal {

enum class FrameHashKind : uint8_t {
    Any = 0,
    Board,
    Edge,
};

struct FrameHashes {
    uint64_t board = 0ULL;
    uint64_t edge = 0ULL;
    uint64_t all = 0ULL;
};

uint64_t MapOccupancyMask(uint64_t rawMask, bool squareRotate);

FrameHashes ComputeComposedFrameHashes(
    const runtime::GameStateMachine& stateMachine,
    const modules::EngineDerivedState* derived,
    const modules::GamePersistenceCoordinator* gamePersistence,
    bool boardRotate,
    bool firstPlayerIsP2,
    bool squareRotateForOutput);

} // namespace trace::replay_internal
