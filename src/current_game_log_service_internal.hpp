#pragma once

#ifdef ARDUINO

#include "modules/current_game_log.hpp"
#include "modules/game_history_capture.hpp"

namespace firmware::current_game_log_service_internal {

inline bool SameGame(const modules::game_history::GameId& left,
                     const modules::game_history::GameId& right) {
    return left == right;
}

inline bool RequiresArchive(
    const modules::game_history::GameHistoryCaptureSnapshot& snapshot) {
    return snapshot.CurrentPly() != 0U || snapshot.shouldSeal ||
        snapshot.lineRevision > 1U;
}

inline bool CanRebaseUnplayedDuckOpening(
    const modules::current_game_log::CommitPointer& authority,
    const modules::game_history::GameHistoryCaptureSnapshot& snapshot,
    const modules::game_history::Record& latest) {
    using namespace modules::game_history;
    if (authority.currentPly != 0U || authority.lineRevision != 1U ||
        authority.archiveRequired || snapshot.basePly != 0U ||
        snapshot.variant != Variant::Duck || latest.type != RecordType::LineBase ||
        latest.variant != Variant::Duck ||
        latest.position.bytes.back() == snapshot.basePosition.bytes.back()) return false;
    auto relocated = latest.position;
    relocated.bytes.back() = snapshot.basePosition.bytes.back();
    return relocated.bytes == snapshot.basePosition.bytes;
}

class StorageBatch final {
  public:
    explicit StorageBatch(
        const modules::current_game_log::StorageOps& storage)
        : m_storage(storage) {
        if (m_storage.beginBatch == nullptr &&
            m_storage.endBatch == nullptr) {
            m_ready = true;
            return;
        }
        if (m_storage.beginBatch != nullptr &&
            m_storage.endBatch != nullptr &&
            m_storage.beginBatch(m_storage.context)) {
            m_started = true;
            m_ready = true;
        }
    }

    ~StorageBatch() {
        if (m_started) m_storage.endBatch(m_storage.context);
    }

    bool IsReady() const { return m_ready; }

  private:
    const modules::current_game_log::StorageOps& m_storage;
    bool m_started = false;
    bool m_ready = false;
};

} // namespace firmware::current_game_log_service_internal

#endif
