#include <cstdint>

#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include "modules/orientation.hpp"

namespace modules {

namespace {

constexpr bool IsGameOver(const EngineDerivedState& d) {
    return d.inCheckmate || d.inDraw || d.inStalemate;
}

// Opening king/queen arrangement is a rules concern, not the physical square
// mapping. Preserve its independent setting transition while sensor/output
// orientation follows BOARD_ROTATE alone.
constexpr bool OpeningArrangement(const bool boardRotate, const bool firstP2) {
    return boardRotate != firstP2;
}

} // namespace

void GamePersistenceCoordinator::UpdateKingQueenSwapPrompt(const bool boardRotateEnabled,
                                                           const bool firstPlayerIsP2,
                                                           const bool atEitherInitialFen) {
    // A started generation that is no longer active completed naturally.
    // A generation that has not started yet must retain its baseline so a
    // rapid setting revert can still collapse it.
    if (m_kqPromptBaselineValid &&
        !m_stateMachine.IsKingQueenSwapPromptActive() &&
        m_kqPromptLastStartedGeneration == m_kqPromptGeneration) {
        m_kqPromptBaselineValid = false;
    }

    bool firstPlayerChanged = false;
    bool canceledPromptThisTick = false;
    bool previousSquareRotate = OpeningArrangement(
        boardRotateEnabled, firstPlayerIsP2);

    if (!m_kqPromptInitialized) {
        m_kqPromptInitialized = true;
        m_lastBoardRotateEnabled = boardRotateEnabled;
        m_kqPromptGeneration = 0;
        m_kqPromptLastStartedGeneration = 0;
        m_firstPlayerInitialized = true;
        m_lastFirstPlayerIsP2 = firstPlayerIsP2;
    } else {
        previousSquareRotate = OpeningArrangement(
            m_lastBoardRotateEnabled, m_lastFirstPlayerIsP2);
        firstPlayerChanged = (m_lastFirstPlayerIsP2 != firstPlayerIsP2);

        // The control gate above prevents normal admission during search. If
        // an already-admitted setting races worker start, retain the prior
        // marker and retry the latest setting on a later cadence instead of
        // silently consuming it without resetting the initial projection.
        const bool canResetInitialPlayerMapping =
            firstPlayerChanged && atEitherInitialFen &&
            m_stateMachine.GetChessInteractionModel().phase.tag ==
                runtime::ChessInteractionPhase::Idle &&
            !m_stateMachine.IsMoveSettlingActive() &&
            !m_stateMachine.HasQueuedCpuMove() &&
            !m_stateMachine.IsPlacementModeActive() &&
            !m_stateMachine.IsKingQueenSwapPromptActive() &&
            !m_stateMachine.IsUndoPromptActive() &&
            !m_stateMachine.IsTextOverlayActive() &&
            !m_promotionPending;
        if (canResetInitialPlayerMapping && !QuiesceAllThinking()) {
            m_cancelAllThinkingRequested = true;
            return;
        }

        m_lastBoardRotateEnabled = boardRotateEnabled;
        m_lastFirstPlayerIsP2 = firstPlayerIsP2;
    }

    const bool squareRotate = OpeningArrangement(
        boardRotateEnabled, firstPlayerIsP2);
    const bool effectiveOrientationChanged =
        previousSquareRotate != squareRotate;
    // Incomplete Fresh Setup already guides placement in the new arrangement.
    // Do not retain that setting change for a swap after assembly finishes.
    if (atEitherInitialFen && effectiveOrientationChanged &&
        m_stateMachine.IsExpectedOccupancyMatched()) {
        if (!m_kqPromptBaselineValid) {
            m_kqPromptBaselineValid = true;
            m_kqPromptBaselineSquareRotate = previousSquareRotate;
        }
        ++m_kqPromptGeneration;
    }

    // If settings are reverted to the pre-prompt baseline orientation, collapse
    // any queued prompt generations. If the prompt is still cancelable (no lifts
    // yet), cancel it immediately.
    if (m_kqPromptBaselineValid &&
        squareRotate == m_kqPromptBaselineSquareRotate) {
        if (!m_stateMachine.IsKingQueenSwapPromptActive()) {
            m_kqPromptLastStartedGeneration = m_kqPromptGeneration;
            canceledPromptThisTick = true;
            m_kqPromptBaselineValid = false;
        } else if (m_stateMachine.CancelKingQueenSwapPromptIfCancelable()) {
            m_kqPromptLastStartedGeneration = m_kqPromptGeneration;
            canceledPromptThisTick = true;
            m_kqPromptBaselineValid = false;
            logger::Trace("kq-swap: prompt canceled (orientation reverted before lifts)");
        }
    }

    // Once an untouched opening-arrangement prompt has been canceled (or
    // before a new one starts), refresh the still-initial game rules. This
    // does not alter sensor coordinates or printed control ownership.
    if (firstPlayerChanged && atEitherInitialFen &&
        m_stateMachine.GetChessInteractionModel().phase.tag ==
            runtime::ChessInteractionPhase::Idle &&
        !m_stateMachine.IsMoveSettlingActive() &&
        !m_stateMachine.HasQueuedCpuMove() &&
        !m_stateMachine.IsPlacementModeActive() &&
        !m_stateMachine.IsKingQueenSwapPromptActive() &&
        !m_stateMachine.IsUndoPromptActive() &&
        !m_stateMachine.IsTextOverlayActive() &&
        !m_promotionPending) {
        (void)ResetEngineInternal(false);
    }

    const bool kqPromptPending =
        atEitherInitialFen && (m_kqPromptLastStartedGeneration != m_kqPromptGeneration);

    // Start the king/queen swap prompt (if needed) before evaluating whether
    // VS-CPU is allowed to think, so we don't race into CPU thinking.
    if (kqPromptPending &&
        !canceledPromptThisTick &&
        !m_stateMachine.IsKingQueenSwapPromptActive() &&
        m_stateMachine.GetChessInteractionModel().phase.tag ==
            runtime::ChessInteractionPhase::Idle &&
        !m_stateMachine.IsMoveSettlingActive() &&
        !m_stateMachine.HasQueuedCpuMove() &&
        !m_stateMachine.IsPlacementModeActive() &&
        !m_stateMachine.IsUndoPromptActive() &&
        !m_stateMachine.IsTextOverlayActive() &&
        !m_promotionPending &&
        m_stateMachine.IsExpectedOccupancyMatched() &&
        !m_stateMachine.HasHumanIllegal() &&
        !m_stateMachine.HasCpuIllegal() &&
        !IsGameOver(m_engineDerived)) {
        // Squares: 0=a8..63=h1. White K/Q: e1/d1 (60/59). Black K/Q: e8/d8 (4/3).
        static constexpr uint64_t KQ_MASK64 = (1ULL << 60) | (1ULL << 59) | (1ULL << 4) | (1ULL << 3);
        if (m_stateMachine.StartKingQueenSwapPrompt(KQ_MASK64)) {
            m_kqPromptLastStartedGeneration = m_kqPromptGeneration;
            logger::Trace("kq-swap: prompt start (opening arrangement changed)");
        }
    }
}

} // namespace modules
