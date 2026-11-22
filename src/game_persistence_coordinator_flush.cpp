#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include "modules/game_persistence_engine_helpers.hpp"

namespace modules {

bool GamePersistenceCoordinator::FlushToSettingsNow() {
    return PrepareSettingsSnapshotForLifecycle() && m_settings.SaveNow();
}

bool GamePersistenceCoordinator::PrepareSettingsSnapshotForLifecycle() {
    // Capture accounting and consume a coincident timeout before preparing the
    // planned reboot/sleep snapshot. This remains an in-memory settings update;
    // The caller owns admission and confirmation of the complete snapshot.
    UpdateGameClock();
    if (HasClockTerminalOutcome()) {
        // Late physical handling cannot make a terminal game resumable or
        // prevent its durable drain. PersistSnapshot waits for search cleanup
        // and omits every live interaction/CPU token from terminal records.
        return PersistSnapshotDeferredWithRetries("flush_terminal", false);
    }

    if (m_duckUndoRelocationPending || m_duckRedoRelocationPending) {
        logger::Warn(
            "persist: flush_now aborted (duck undo relocation active)");
        return false;
    }

    const bool thinkingAtStart = m_vsCpuThinking || m_hintThinking ||
                                 m_stateMachine.IsCpuThinkingActive() ||
                                 m_stateMachine.IsHintThinkingActive() ||
                                 (UsesAsyncThinkWorker() &&
                                  ThinkWorker().IsActive());
    if (!thinkingAtStart) {
        RequestCancelAllThinking();
        if (m_stateMachine.IsUndoPromptActive() && !RequestUndo(1U)) {
            logger::Warn("persist: flush_now aborted (undo prompt active)");
            return false;
        }
        for (uint8_t i = 0; i < 6U; ++i) {
            if (!m_stateMachine.HasCommittedMovePending() &&
                m_pendingUndoCommand.kind == UndoCommandKind::None) {
                break;
            }
            Update();
        }
    }

    // Canceling an active chess undo above may have restored the forward chess
    // state and entered the fixed Duck redo. That relocation is RAM-only, so a
    // snapshot here could pair the restored FEN/history with the prior square.
    if (m_duckUndoRelocationPending || m_duckRedoRelocationPending) {
        logger::Warn(
            "persist: flush_now aborted (duck undo relocation active)");
        return false;
    }

    if (m_stateMachine.IsUndoPromptActive()) {
        logger::Warn("persist: flush_now aborted (undo prompt active)");
        return false;
    }
    if (m_stateMachine.IsPromotionActive() || m_promotionPending) {
        logger::Warn("persist: flush_now aborted (promotion active)");
        return false;
    }

    const bool thinking = m_vsCpuThinking || m_hintThinking ||
                          m_stateMachine.IsCpuThinkingActive() ||
                          m_stateMachine.IsHintThinkingActive() ||
                          (UsesAsyncThinkWorker() &&
                           ThinkWorker().IsActive());
    if (!thinking) {
        (void)game_persistence_engine_helpers::SafeGetFen(
            m_engine, m_cachedFen, true);
    }
    if (m_cachedFen.Empty()) {
        const char* persistedFen = m_settings.ReadValue(KEY_GAME_FEN, "");
        if (persistedFen != nullptr && persistedFen[0] != '\0') {
            (void)m_cachedFen.Assign(persistedFen);
        }
    }
    if (m_cachedFen.Empty()) {
        return false;
    }
    return PersistSnapshotDeferredWithRetries("flush_now", false);
}

} // namespace modules
