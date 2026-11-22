#include "modules/game_persistence_coordinator.hpp"

#include <litch_max/chess_notation.hpp>
#include "trace/trace_recorder.hpp"

namespace modules {

GameVariantStateSnapshot
GamePersistenceCoordinator::GetGameVariantStateSnapshot() const {
    GameVariantStateSnapshot state;
    state.variant = m_gameVariant;
    state.duckSquare = m_duckSquare;
    state.duckTurnPhase = m_duckTurnPhase;
    state.pendingChessMove = m_pendingDuckTurn.valid;
    state.outcome = m_variantOutcome;

    litch_max::ParsedUciMove parsed;
    if (m_pendingDuckTurn.valid &&
        litch_max::TryParseUciMove(m_pendingDuckTurn.uci.CStr(), parsed)) {
        state.pendingMoveFrom = parsed.fromSquare;
        state.pendingMoveTo = parsed.toSquare;
        state.pendingPromotionSuffix =
            parsed.hasPromotion ? parsed.promotionSuffixLower : '\0';
    }
    return state;
}

void GamePersistenceCoordinator::RecordVariantTraceCheckpoint() {
    if (m_traceRecorder == nullptr || !m_traceRecorder->IsActive()) {
        return;
    }
    const GameVariantStateSnapshot state =
        GetGameVariantStateSnapshot();
    const uint64_t interactionHash =
        runtime::ComputeChessInteractionModelHash(
            m_stateMachine.GetChessInteractionModel());
    m_traceRecorder->RecordCheckpointVariant(state, interactionHash);
}

} // namespace modules
