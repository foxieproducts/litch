#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/current_game_log.hpp"
#include "modules/current_game_sync_protocol.hpp"
#include "modules/game_history_protocol.hpp"

namespace modules::current_game_sync {

inline constexpr uint16_t MAX_SEGMENT_BYTES = 2048U;
inline constexpr uint8_t MAX_SEGMENT_RECORDS = 32U;

enum class SourceResult : uint8_t {
    Ready = 0U,
    Complete,
    NoLine,
    CursorMismatch,
    Bounds,
    Corrupt,
    IoFailure,
    BadState,
};

struct SourceSnapshot {
    Digest digest{};
    Bounds bounds{};
};

struct SegmentPlan {
    game_history::SegmentDescriptor descriptor{};
    uint32_t byteOffset = 0U;
};

// Reads the immutable committed prefix selected by a CurrentGameLog pointer.
// Callers must keep all methods on the settings persistence owner; StorageOps
// enforces that requirement in production.
class CurrentGameSyncSource final {
  public:
    explicit CurrentGameSyncSource(current_game_log::StorageOps storage)
        : m_storage(storage) {}

    SourceResult Open(const current_game_log::CommitPointer& authority,
                      SourceSnapshot& out);
    SourceResult Plan(const current_game_log::CommitPointer& authority,
                      const ResumeCursor& cursor,
                      SegmentPlan& out);
    SourceResult ReadSegmentChunk(
        const current_game_log::CommitPointer& authority,
        const SegmentPlan& plan,
        uint16_t offset,
        uint8_t* out,
        size_t capacity,
        size_t& outLength);
    void Close();

  private:
    current_game_log::StorageOps m_storage;
    current_game_log::CommitPointer m_authority{};
    SourceSnapshot m_snapshot{};
    SegmentPlan m_plan{};
    bool m_open = false;
    bool m_planReady = false;
};

} // namespace modules::current_game_sync
