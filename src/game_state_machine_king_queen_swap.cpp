#include "runtime/game_state_machine.hpp"

#include "logger.hpp"

namespace runtime {

bool GameStateMachine::StartKingQueenSwapPrompt(const uint64_t squaresMask64) {
    const uint64_t mask = squaresMask64;
    if (mask == 0ULL) {
        return false;
    }

    if (m_kingQueenSwapPromptActive) {
        return true;
    }

    if (IsUndoPromptActive()) {
        return false;
    }

    if (IsPromotionActive()) {
        return false;
    }

    if (m_placementModeActive) {
        return false;
    }

    // This is a pre-move corrective modal; only allow while idle and with no
    // CPU physical prompt queued.
    if (m_chessInteractionModel.phase.tag != ChessInteractionPhase::Idle ||
        IsMoveSettlingActive() || m_cpuExecution.queued) {
        return false;
    }

    m_kingQueenSwapPromptActive = true;
    m_kingQueenSwapSquaresMask64 = mask;
    m_kingQueenSwapInitialEmptyMask64 =
        (~m_chessInteractionModel.board.occupiedMask) & mask;
    m_kingQueenSwapLiftedMask64 = m_kingQueenSwapInitialEmptyMask64;
    m_kingQueenSwapReadyToExit = false;

    // Start a fresh stability window for this modal so it cannot immediately
    // complete due to an old/stale stability timer.
    m_lastInteractionElapsed.Reset();

    ClearMovePreviewForPlacementMode();

    logger::Trace("kq-swap: start mask=0x%016llx", static_cast<unsigned long long>(mask));

    UpdateKingQueenSwapPromptProgress();
    return true;
}

void GameStateMachine::ClearKingQueenSwapPrompt() {
    m_kingQueenSwapPromptActive = false;
    m_kingQueenSwapSquaresMask64 = 0;
    m_kingQueenSwapLiftedMask64 = 0;
    m_kingQueenSwapInitialEmptyMask64 = 0;
    m_kingQueenSwapReadyToExit = false;

}

void GameStateMachine::UpdateKingQueenSwapPromptProgress() {
    if (!m_kingQueenSwapPromptActive) {
        return;
    }

    const uint64_t mask = m_kingQueenSwapSquaresMask64;
    const uint64_t occ = m_chessInteractionModel.board.occupiedMask & mask;

    // Phase 1: observe each prompted square become empty at least once.
    // This avoids requiring the user to lift all 4 pieces simultaneously.
    const uint64_t empty =
        (~m_chessInteractionModel.board.occupiedMask) & mask;
    const uint64_t before = m_kingQueenSwapLiftedMask64;
    m_kingQueenSwapLiftedMask64 |= empty;
    if (m_kingQueenSwapLiftedMask64 != before) {
        logger::Trace("kq-swap: lifted mask=0x%016llx",
                      static_cast<unsigned long long>(m_kingQueenSwapLiftedMask64));
    }

    if (m_kingQueenSwapLiftedMask64 != mask) {
        return;
    }

    // Phase 2: observe all prompt squares occupied again (then we will exit once
    // sensors have been stable for placementStableRequiredMs).
    if (occ == mask) {
        m_kingQueenSwapReadyToExit = true;
    }
}

} // namespace runtime
