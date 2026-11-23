#include "runtime/game_state_machine.hpp"

#include "modules/beginner_mode_policy.hpp"

namespace runtime {

bool GameStateMachine::IsContinuousDuckHint() const {
    if (!m_renderState.hintDisplayRequested || !m_renderState.hintMoveValid) return false;
    const auto& phase = m_chessInteractionModel.phase;
    if (m_renderState.hintDuckMoveValid) {
        return modules::IsMatchingDuckHintChessProgress(
            phase, m_renderState.hintMoveFromSquare, m_renderState.hintMoveToSquare);
    }
    return phase.tag == ChessInteractionPhase::DuckRelocation &&
           phase.plan.source == PhysicalTransitionSource::Human &&
           phase.requiredAction != RequiredPhysicalAction::CorrectBoard &&
           phase.plan.moveFrom == m_renderState.hintMoveFromSquare;
}

void GameStateMachine::SetDuckRelocationHint(
    const uint8_t fromSquare, const uint8_t toSquare) {
    ClearHintMove();
    SetHintMoveSquares(fromSquare, toSquare);
}

bool GameStateMachine::BeginDuckRelocation(
    const uint8_t fromSquare,
    const uint8_t requiredDestination,
    const PhysicalTransitionSource source,
    const DirectedMoveOrigin directedOrigin) {
    if (!BeginDuckRelocationPlan(
            m_chessInteractionModel, fromSquare, requiredDestination,
            source, directedOrigin)) {
        return false;
    }
    m_settleTimer.Stop();
    m_activeSettleDurationMs = 0U;
    m_humanSettleActive = false;
    // Duck relocation takes ownership after the chess acknowledgement.
    // A later CPU setting change must not cancel it as an old chess settle.
    m_cpuExecution.moveSettleActive = false;
    m_cpuExecution.settleDestinationSuppressed = false;
    m_renderState.settleSquare = INVALID_SQUARE;
    ClearValidDestMask();
    if (m_chessInteractionModel.phase.tag == ChessInteractionPhase::Settling) {
        StartSettling(m_chessInteractionModel.phase.plan.moveTo);
    }
    return true;
}

bool GameStateMachine::CancelDuckRelocationForUndo() {
    if (!IsDuckRelocationActive()) {
        return false;
    }
    const PhysicalTransitionPlan plan =
        m_chessInteractionModel.phase.plan;
    if (!m_chessInteractionModel.board.valid ||
        m_chessInteractionModel.board.occupiedMask !=
            plan.beforeOccupiedMask) {
        return false;
    }

    ReduceInteraction(ChessInteractionEvent::MakePlanCanceled());
    m_humanIllegalMask = 0ULL;
    m_captureReturnMask = 0ULL;
    ClearValidDestMask();
    return m_chessInteractionModel.phase.tag ==
           ChessInteractionPhase::Idle;
}

bool GameStateMachine::IsDuckRelocationActive() const {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    return phase.plan.kind == PhysicalTransitionKind::DuckRelocation &&
           (phase.tag == ChessInteractionPhase::DuckRelocation ||
            phase.tag == ChessInteractionPhase::Settling ||
            phase.tag == ChessInteractionPhase::AwaitingAction ||
            phase.tag == ChessInteractionPhase::Correction);
}

bool GameStateMachine::TryGetDuckRelocation(
    uint8_t& outFromSquare,
    uint8_t& outToSquare) const {
    if (!IsDuckRelocationActive()) {
        return false;
    }
    outFromSquare = m_chessInteractionModel.phase.plan.moveFrom;
    outToSquare = m_chessInteractionModel.phase.plan.moveTo;
    return true;
}

} // namespace runtime
