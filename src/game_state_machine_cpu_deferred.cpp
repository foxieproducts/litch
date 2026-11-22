#include "runtime/game_state_machine.hpp"

namespace runtime {

void GameStateMachine::SetCpuThinkingActive(const bool active) {
    m_cpuThinkingActive = active;
    RefreshThinkingControls();
}

void GameStateMachine::RefreshThinkingControls() {
    const bool visibleHint = m_renderState.hintDisplayRequested &&
        (m_renderState.hintThinkingActive ||
         (m_renderState.hintFeedbackPending && !m_renderState.hintMoveValid));
    const auto& position = m_chessInteractionModel.acknowledged;
    const auto owner = m_cpuThinkingActive || visibleHint
        ? (position.sideToMoveBlack ? modules::UiOwner::P2 : modules::UiOwner::P1)
        : modules::UiOwner::None;
    // Retain the work kind while the existing presentation envelope retires.
    if (owner != modules::UiOwner::None) {
        m_renderState.thinkingControlsCpu = m_cpuThinkingActive;
    } else if (m_renderState.thinkingControls.Scale256() == 0U) {
        m_renderState.thinkingControlsCpu = false;
    }
    m_renderState.thinkingControls.SetRequested(owner);
    // Manual O and eligible automatic Coaching share Hint visibility. Both
    // retain the selected thinking cue until that position's analysis ends.
    const bool humanAnalysis = visibleHint && !m_cpuThinkingActive;
    m_renderState.hintBrewing.SetRequested(humanAnalysis
        ? (position.sideToMoveBlack ? modules::UiOwner::P2 : modules::UiOwner::P1)
        : modules::UiOwner::None);
}

} // namespace runtime
