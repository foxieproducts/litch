#include "modules/game_persistence_coordinator.hpp"
#include "modules/initial_fen.hpp"

namespace modules {

bool GamePersistenceCoordinator::TryGetBootResumeOccupancy(uint64_t& outOccupancy) const {
    if ((!m_gameHasAcceptedMove && !m_pendingDuckTurn.valid &&
         !m_clockSessionStarted && !m_pendingCpuMoveUciValid) || IsGameOverVariantAware()) {
        return false;
    }
    const auto& interaction = m_stateMachine.GetChessInteractionModel();
    outOccupancy = runtime::ProjectPhysicalOccupancyForInteractionProgress(
        interaction.acknowledged, interaction.phase);
    return true;
}

bool GamePersistenceCoordinator::BeginNewGameHistory(
    const bool captureIncomplete) {
    m_gameHasAcceptedMove = captureIncomplete;
    m_gameHistoryPersistence = {};
    if (m_gameHistoryIdentityGenerator == nullptr) {
        return false;
    }
    game_history::GameId identity;
    if (!m_gameHistoryIdentityGenerator(
            m_gameHistoryIdentityContext, identity) ||
        !game_history::IsValidGameId(identity)) {
        return false;
    }
    m_gameHistoryPersistence.present = true;
    m_gameHistoryPersistence.gameId = identity;
    m_gameHistoryPersistence.lineRevision = 1U;
    // A partial capture uses ply one as a bounded unknown-prefix sentinel, so
    // the phone cannot mistake an externally supplied position for a complete
    // game beginning at ply zero.
    m_gameHistoryPersistence.currentPly = captureIncomplete ? 1U : 0U;
    m_gameHistoryPersistence.captureIncomplete = captureIncomplete;
    return true;
}

void GamePersistenceCoordinator::RecordGameHistoryPlyMutation(
    const bool appended) {
    if (appended) m_gameHasAcceptedMove = true;
    if (!m_gameHistoryPersistence.valid || !m_gameHistoryPersistence.present ||
        m_gameHistoryPersistence.IsCaptureExhausted()) return;
    if (m_gameHistoryPersistence.lineRevision >= game_history::MAX_LINE_REVISION ||
        (appended && m_gameHistoryPersistence.currentPly >= 4096U)) {
        m_gameHistoryPersistence.captureIncomplete = true;
        return;
    }
    if (!appended && m_gameHistoryPersistence.currentPly == 0U) {
        // A contradictory Undo has lost exact authority. Persist the same
        // exhausted marker rather than allowing a later append to alias it.
        m_gameHistoryPersistence.lineRevision = game_history::MAX_LINE_REVISION;
        m_gameHistoryPersistence.captureIncomplete = true;
        return;
    }
    ++m_gameHistoryPersistence.lineRevision;
    if (appended) ++m_gameHistoryPersistence.currentPly;
    else --m_gameHistoryPersistence.currentPly;
}

void GamePersistenceCoordinator::RestoreGameHistoryIdentity(
    const GamePersistenceState& persisted) {
    m_gameHistoryPersistence = persisted.gameHistory;
    if (m_gameHistoryPersistence.valid && !m_gameHistoryPersistence.present &&
        m_gameHistoryIdentityGenerator != nullptr) {
        const uint16_t retainedMoves = static_cast<uint16_t>(
            m_undoHistoryFieldCount / UNDO_HISTORY_FIELDS_PER_MOVE);
        const bool completeLegacyPrefix =
            modules::IsAnyInitialFen(m_undoBaseFen.CStr());
        if (BeginNewGameHistory(!completeLegacyPrefix)) {
            m_gameHistoryPersistence.currentPly = completeLegacyPrefix
                ? retainedMoves
                : static_cast<uint16_t>(retainedMoves + 1U);
        }
    }
    m_gameHasAcceptedMove = persisted.hasAcceptedMove ||
        !modules::IsAnyInitialFen(m_cachedFen.CStr()) ||
        m_stateMachine.GetMovedSquaresMask() != 0ULL ||
        m_undoHistoryFieldCount != 0U ||
        (m_gameHistoryPersistence.present &&
         (m_gameHistoryPersistence.lineRevision > 1U ||
          m_gameHistoryPersistence.captureIncomplete));
}

void GamePersistenceCoordinator::CaptureGameHistoryPersistenceState(
    GamePersistenceState& s) const {
    s.gameHistory = m_gameHistoryPersistence;
    if (s.gameHistory.present) {
        s.gameHistory.shouldSeal = false;
        s.gameHistory.status = game_history::ArchiveStatus::Unfinished;
        s.gameHistory.outcome = game_history::Outcome::Ongoing;
        s.gameHistory.winner = game_history::Winner::None;
        if (s.clockStatePresent && s.clockState.clock.timedOut) {
            s.gameHistory.shouldSeal = true;
            s.gameHistory.status = game_history::ArchiveStatus::Completed;
            s.gameHistory.outcome = game_history::Outcome::Timeout;
            s.gameHistory.winner =
                s.clockState.clock.timedOutSide == runtime::ClockSide::White
                    ? game_history::Winner::Black
                    : game_history::Winner::White;
        } else if (m_variantOutcome.IsTerminal()) {
            s.gameHistory.shouldSeal = true;
            s.gameHistory.status = game_history::ArchiveStatus::Completed;
            s.gameHistory.outcome =
                m_variantOutcome.reason == VariantOutcomeReason::Resigned
                    ? game_history::Outcome::Resignation
                    : m_variantOutcome.reason == VariantOutcomeReason::KingCaptured
                    ? game_history::Outcome::KingCaptured
                    : game_history::Outcome::Fowled;
            s.gameHistory.winner =
                m_variantOutcome.winner == GameWinner::White
                    ? game_history::Winner::White
                    : game_history::Winner::Black;
        } else if (m_gameVariant != GameVariant::Duck &&
                   m_engineDerived.inCheckmate) {
            s.gameHistory.shouldSeal = true;
            s.gameHistory.status = game_history::ArchiveStatus::Completed;
            s.gameHistory.outcome = game_history::Outcome::Checkmate;
            s.gameHistory.winner =
                m_engineDerived.sideToMove == SideToMove::White
                    ? game_history::Winner::Black
                    : game_history::Winner::White;
        } else if (m_gameVariant != GameVariant::Duck &&
                   (m_engineDerived.inDraw || m_engineDerived.inStalemate)) {
            s.gameHistory.shouldSeal = true;
            s.gameHistory.status = game_history::ArchiveStatus::Completed;
            s.gameHistory.outcome = game_history::Outcome::Draw;
            s.gameHistory.winner = game_history::Winner::Draw;
        }
    }

}

} // namespace modules
