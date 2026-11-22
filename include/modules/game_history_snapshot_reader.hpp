#pragma once

#include <array>
#include <cstdint>

#include "modules/game_history_capture.hpp"
#include "modules/game_persistence.hpp"

namespace modules::game_history {

// Converts one immutable, already-durable settings snapshot into the compact
// history capture view. Replay is allocation-free and never touches the shared
// chess engine, so it is safe to run on the settings storage worker.
class GameHistorySnapshotReader final {
  public:
    explicit GameHistorySnapshotReader(const GamePersistenceState& state)
        : m_state(state) {}

    bool Build(GameHistoryCaptureSnapshot& outSnapshot);

  private:
    static bool ReadStepThunk(void* context,
                              uint16_t index,
                              GameHistoryCaptureStep& outStep);
    bool ReadStep(uint16_t index, GameHistoryCaptureStep& outStep);
    bool ApplyStep(uint16_t index,
                   Position& inOutPosition,
                   Transition& outTransition) const;
    void ResetReplay();

    const GamePersistenceState& m_state;
    Variant m_variant = Variant::Classic;
    uint16_t m_transitionCount = 0U;
    std::array<uint8_t, UNDO_HISTORY_MOVE_CAPACITY> m_duckFrom{};
    std::array<uint8_t, UNDO_HISTORY_MOVE_CAPACITY> m_duckTo{};
    Position m_basePosition{};
    Position m_replayPosition{};
    uint16_t m_replayNextIndex = 0U;
};

} // namespace modules::game_history
