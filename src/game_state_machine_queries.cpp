#include "runtime/game_state_machine.hpp"

#include <utility>

namespace runtime {

uint32_t GameStateMachine::GetUndoPromptElapsedMs() const {
    if (!IsUndoPromptActive()) {
        return 0U;
    }

    const fxe::ElapsedTime sinceStart(m_undoPromptStartMs);
    return sinceStart.ElapsedMs();
}

void GameStateMachine::ClearMovedSquares() {
    m_movedSquaresMask = 0;
    m_movedSquaresMaskBeforeLastAcknowledgedHumanMove = 0;
    m_movedSquaresSnapshotActionToken = 0U;
}

void GameStateMachine::SetMovedSquaresMask(const uint64_t mask) {
    m_movedSquaresMask = mask;
}

void GameStateMachine::SetTextOverlayActive(const bool active) {
    if (active) CancelHintRefinement();
    m_textOverlayActive = active;
}

bool GameStateMachine::TryGetUndoPromptMove(uint8_t& outFromSquare, uint8_t& outToSquare) const {
    if (!IsUndoPromptActive()) {
        return false;
    }
    outFromSquare = m_chessInteractionModel.phase.plan.moveFrom;
    outToSquare = m_chessInteractionModel.phase.plan.moveTo;
    return true;
}

bool GameStateMachine::IsUndoPromptActive() const {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    return phase.plan.source == PhysicalTransitionSource::Undo &&
           phase.plan.kind == PhysicalTransitionKind::Undo &&
           (phase.tag == ChessInteractionPhase::ExecutingPlan ||
            phase.tag == ChessInteractionPhase::AwaitingAction);
}

bool GameStateMachine::TryClaimPendingInteractionMove(
    PhysicalTransitionPlan& out) {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    const uint64_t planControlledMask =
        GetLegalProgressPhysicalMask(phase);
    if (phase.tag != ChessInteractionPhase::AwaitingAction ||
        phase.pendingActionToken == 0U ||
        phase.pendingActionToken == m_claimedInteractionActionToken ||
        (phase.plan.source != PhysicalTransitionSource::Human &&
         !IsCpuDirectedTransition(phase.plan)) ||
        phase.plan.kind == PhysicalTransitionKind::None ||
        (IsCpuDirectedTransition(phase.plan) &&
         ((m_chessInteractionModel.board.occupiedMask ^
           phase.plan.afterOccupiedMask) &
          planControlledMask) != 0ULL)) {
        return false;
    }
    m_claimedInteractionActionToken = phase.pendingActionToken;
    out = phase.plan;
    return true;
}

bool GameStateMachine::HasCommittedMovePending() const {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    const bool unclaimedInteractionMove =
        phase.tag == ChessInteractionPhase::AwaitingAction &&
        phase.pendingActionToken != 0U &&
        phase.pendingActionToken != m_claimedInteractionActionToken &&
        (phase.plan.source == PhysicalTransitionSource::Human ||
         IsCpuDirectedTransition(phase.plan));
    return unclaimedInteractionMove;
}

bool GameStateMachine::IsMoveActionAwaitingResult() const {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    return phase.tag == ChessInteractionPhase::AwaitingAction &&
           (phase.plan.source == PhysicalTransitionSource::Human ||
            IsCpuDirectedTransition(phase.plan)) &&
           phase.pendingActionToken != 0U;
}

bool GameStateMachine::IsCastlingRookPromptActive() const {
    const PhysicalTransitionPlan* plan = GetActiveHumanCastlingPlan();
    return plan != nullptr &&
           (plan->progress.relocationsPlacedMask & 0x01U) != 0U &&
           (plan->progress.relocationsPlacedMask & 0x02U) == 0U;
}

bool GameStateMachine::TryGetUndoPromptMoverSide(modules::PersistedSide& outMoverSidePersist) const {
    if (!IsUndoPromptActive() || m_undoPromptMoverSidePersist == modules::PersistedSide::Unknown) {
        return false;
    }
    outMoverSidePersist = m_undoPromptMoverSidePersist;
    return true;
}

bool GameStateMachine::TryGetUndoPromptMoverPieceFen(char& outMovedPieceFen) const {
    if (!IsUndoPromptActive() || m_undoPromptMoverPieceFen == '\0') {
        return false;
    }
    outMovedPieceFen = m_undoPromptMoverPieceFen;
    return true;
}

bool GameStateMachine::TryGetUndoRestoreInfo(uint8_t& outSquare, char& outCapturedPieceFen) const {
    const PhysicalTransitionPlan& plan = m_chessInteractionModel.phase.plan;
    if (!IsUndoPromptActive() || !plan.restorationRequired) {
        return false;
    }
    outSquare = plan.restorationSquare;
    outCapturedPieceFen = plan.restorationPieceFen;
    return true;
}

bool GameStateMachine::IsKingQueenSwapPromptCancelable() const {
    return m_kingQueenSwapPromptActive && (m_kingQueenSwapLiftedMask64 == m_kingQueenSwapInitialEmptyMask64);
}

bool GameStateMachine::CancelKingQueenSwapPromptIfCancelable() {
    if (!IsKingQueenSwapPromptCancelable()) {
        return false;
    }
    ClearKingQueenSwapPrompt();
    return true;
}

bool GameStateMachine::TryGetKingQueenSwapPromptMask(uint64_t& outSquaresMask64) const {
    if (!m_kingQueenSwapPromptActive) {
        return false;
    }
    outSquaresMask64 = m_kingQueenSwapSquaresMask64;
    return true;
}

bool GameStateMachine::TryGetKingQueenSwapPromptFlashMask(uint64_t& outSquaresMask64) const {
    if (!m_kingQueenSwapPromptActive) {
        return false;
    }

    const uint64_t mask = m_kingQueenSwapSquaresMask64;
    const uint64_t occ = m_chessInteractionModel.board.occupiedMask & mask;
    const uint64_t done = m_kingQueenSwapLiftedMask64 & occ;
    outSquaresMask64 = mask & ~done;
    return true;
}

bool GameStateMachine::ConsumeCheckCacheRefreshRequested() {
    const bool value = m_checkCacheRefreshRequested;
    m_checkCacheRefreshRequested = false;
    return value;
}

bool GameStateMachine::ShouldProcessCommittedMoveImmediately() const {
    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    return phase.tag == ChessInteractionPhase::AwaitingAction &&
           phase.pendingActionToken != 0U &&
           phase.pendingActionToken != m_claimedInteractionActionToken &&
           (phase.plan.source == PhysicalTransitionSource::Human ||
            IsCpuDirectedTransition(phase.plan));
}

bool GameStateMachine::TryGetVisualActiveSideOverride(modules::PersistedSide& outSide) const {
    if (m_visualActiveSideOverridePersist == modules::PersistedSide::Unknown) {
        return false;
    }

    outSide = m_visualActiveSideOverridePersist;
    return true;
}

bool GameStateMachine::TryGetStatusEdgeActiveSideOverride(modules::PersistedSide& outSide) const {
    if (m_statusEdgeActiveSideOverridePersist == modules::PersistedSide::Unknown) {
        return false;
    }

    outSide = m_statusEdgeActiveSideOverridePersist;
    return true;
}

bool GameStateMachine::TryGetPromotionMove(uint8_t& outFromSquare,
                                           uint8_t& outToSquare,
                                           bool& outMoverIsBlack) const {
    const PhysicalTransitionPlan& plan = m_chessInteractionModel.phase.plan;
    if (!IsPromotionActive() ||
        m_chessInteractionModel.phase.tag !=
            ChessInteractionPhase::AwaitingAction ||
        plan.kind != PhysicalTransitionKind::Promotion ||
        plan.source != PhysicalTransitionSource::Human ||
        plan.moveFrom >= 64U || plan.moveTo >= 64U) {
        return false;
    }
    outFromSquare = plan.moveFrom;
    outToSquare = plan.moveTo;
    outMoverIsBlack = m_chessInteractionModel.acknowledged.sideToMoveValid &&
                      m_chessInteractionModel.acknowledged.sideToMoveBlack;
    return true;
}

bool GameStateMachine::ConsumeUndoCanceledQueuedCpuMove() {
    const bool value = m_cpuExecution.undoCanceledQueuedMove;
    m_cpuExecution.undoCanceledQueuedMove = false;
    return value;
}

bool GameStateMachine::TryGetQueuedCpuMove(uint8_t& outFromSquare,
                                           uint8_t& outToSquare,
                                           bool& outCaptureSquareValid,
                                           uint8_t& outCaptureSquare) const {
    if (!m_cpuExecution.queued) {
        return false;
    }
    const PhysicalTransitionPlan& plan =
        m_chessInteractionModel.phase.plan;
    if (!IsCpuDirectedTransition(plan) ||
        plan.moveFrom >= 64U || plan.moveTo >= 64U) {
        return false;
    }
    outFromSquare = plan.moveFrom;
    outToSquare = plan.moveTo;
    outCaptureSquareValid = plan.removalRequired;
    outCaptureSquare = plan.removalRequired ? plan.removalSquare : 0U;
    return true;
}

bool GameStateMachine::TryGetQueuedCpuPromotionSuffix(char& outPromotionSuffixLower) const {
    if (!m_cpuExecution.queued) {
        return false;
    }
    outPromotionSuffixLower = m_cpuExecution.promotionSuffixLower;
    return outPromotionSuffixLower != '\0';
}

uint32_t GameStateMachine::GetCpuMovePromptElapsedMs() const {
    if (!m_cpuExecution.queued) {
        return 0U;
    }
    return m_cpuExecution.promptElapsed.ElapsedMs();
}

void GameStateMachine::SetValidDestMask(const uint64_t mask) {
    m_validDestMask = mask;
    m_validDestMaskKnown = true;
}

void GameStateMachine::ClearValidDestMask() {
    m_validDestMask = 0ULL;
    m_validDestMaskKnown = false;
}

uint32_t GameStateMachine::GetIllegalFlashElapsedMs() const {
    return fxe::Millis() - m_illegalFlashStartMs;
}

uint32_t GameStateMachine::GetIllegalFlashPeriodMs() const {
    return (m_config.illegalFlashMs == 0U) ? 1U : m_config.illegalFlashMs;
}

uint32_t GameStateMachine::GetCaptureReturnFlashElapsedMs() const {
    return fxe::Millis() - m_captureReturnFlashStartMs;
}

uint32_t GameStateMachine::GetLastInteractionElapsedMs() const {
    return m_lastInteractionElapsed.ElapsedMs();
}

bool GameStateMachine::TryGetPendingCaptureSquare(uint8_t& outSquare) const {
    const TaggedInteractionState& phase =
        m_chessInteractionModel.phase;
    if (phase.tag == ChessInteractionPhase::AwaitingAction) {
        const uint64_t stagedMask = GetCaptureFirstStagingMask();
        for (uint8_t square = 0U; square < 64U; ++square) {
            if ((stagedMask & (1ULL << square)) != 0ULL) {
                outSquare = square;
                return true;
            }
        }
    }
    if ((phase.tag == ChessInteractionPhase::ExecutingPlan ||
         phase.tag == ChessInteractionPhase::Settling ||
         phase.tag == ChessInteractionPhase::AwaitingAction) &&
        phase.plan.source == PhysicalTransitionSource::Human &&
        phase.plan.removalRequired &&
        phase.plan.removalSquare < 64U) {
        outSquare = phase.plan.removalSquare;
        return true;
    }
    if (phase.tag == ChessInteractionPhase::CaptureStaged &&
        phase.selectedSquare < 64U) {
        outSquare = phase.selectedSquare;
        return true;
    }
    return false;
}

bool GameStateMachine::HasCaptureFirstStaging() const {
    return GetCaptureFirstStagingMask() != 0ULL;
}

uint64_t GameStateMachine::GetCaptureFirstStagingMask() const {
    const TaggedInteractionState& phase =
        m_chessInteractionModel.phase;
    if (phase.tag == ChessInteractionPhase::CaptureStaged &&
        phase.selectedSquare < 64U) {
        return 1ULL << phase.selectedSquare;
    }
    if (phase.tag != ChessInteractionPhase::AwaitingAction ||
        phase.plan.source != PhysicalTransitionSource::Human) {
        return 0ULL;
    }

    // A sensor event may begin the next interaction while the just-finished
    // move still awaits its engine acknowledgement. Derive that one-event
    // bridge from the canonical observation; ActionResult will replay the same
    // changed square into CaptureStaged after the position is acknowledged.
    const ChessPositionState projected = GetInteractionExpectedPosition();
    const uint64_t removedOpponent =
        m_chessInteractionModel.board.changedMask &
        ~m_chessInteractionModel.board.occupiedMask &
        projected.opponentOccupiedMask;
    return removedOpponent != 0ULL &&
                   (removedOpponent & (removedOpponent - 1ULL)) == 0ULL
               ? removedOpponent
               : 0ULL;
}

bool GameStateMachine::TryGetCpuMoveTargetOccupancyMask(uint64_t& outOcc64) const {
    if (!m_cpuExecution.queued) {
        return false;
    }

    const PhysicalTransitionPlan& plan =
        m_chessInteractionModel.phase.plan;
    if (!IsCpuDirectedTransition(plan)) {
        return false;
    }
    if (plan.removalRequired &&
        plan.removalSquare == plan.moveTo &&
        !plan.progress.removalComplete) {
        outOcc64 = plan.beforeOccupiedMask &
                   ~(1ULL << plan.removalSquare);
        return true;
    }

    outOcc64 = plan.afterOccupiedMask;
    return true;
}

bool GameStateMachine::IsCaptureReturnFlashOn() const {
    const uint32_t period = (m_config.captureReturnFlashMs == 0U) ? 1U : m_config.captureReturnFlashMs;
    const uint32_t elapsedMs = fxe::Millis() - m_captureReturnFlashStartMs;
    return ((elapsedMs / period) % 2U) == 0U;
}

bool GameStateMachine::ConsumeSettleAnimationStarted(uint8_t& outVariant) {
    return runtime::ConsumeSettleAnimationStarted(m_renderState, outVariant);
}

bool GameStateMachine::TryGetHintMoveSquares(uint8_t& outFromSquare, uint8_t& outToSquare) const {
    if (m_hintPresentationObserver == nullptr)
        return runtime::TryGetHintMoveSquares(m_renderState, outFromSquare, outToSquare);
    const auto shown = GetVisibleHintPresentation();
    outFromSquare = shown.fromSquare;
    outToSquare = shown.toSquare;
    return outFromSquare < 64U && outToSquare < 64U;
}

void GameStateMachine::SyncHintRemovalCue() {
    auto& state = m_renderState;
    uint8_t victim = CHESS_INTERACTION_INVALID_SQUARE;
    const auto& position = m_chessInteractionModel.acknowledged;
    const uint8_t from = state.hintMoveFromSquare;
    const uint8_t to = state.hintMoveToSquare;
    if (state.hintDisplayRequested && state.hintMoveValid) victim = HintCaptureSquare(position, from, to);
    if (victim == state.hintRemovalSquare) return;
    const auto& fade = state.hintRefinement;
    if (victim < 64U && victim == state.previousHintRemovalSquare && fade.elapsed.Started()) {
        const auto& shown = fade.elapsed.ElapsedMs() < fade.durationMs / 2U
            ? fade.outgoing : fade.incoming;
        if (HintCaptureSquare(position, shown.fromSquare, shown.toSquare) == victim) {
            std::swap(state.hintRemovalSquare, state.previousHintRemovalSquare);
            std::swap(state.hintRemovalElapsed, state.previousHintRemovalElapsed);
            std::swap(state.hintRemovalDelayMs, state.previousHintRemovalDelayMs);
            return;
        }
    }
    // Coalesced candidates can replace an incoming move repeatedly while the
    // original victim is still on screen. Keep the displayed clock, not an
    // intermediate candidate which never became visible.
    bool previousIsDisplayed = false;
    if (state.hintDisplayRequested && fade.elapsed.Started()) {
        const auto& shown = fade.elapsed.ElapsedMs() < fade.durationMs / 2U
            ? fade.outgoing : fade.incoming;
        previousIsDisplayed = state.previousHintRemovalSquare < 64U &&
            HintCaptureSquare(position, shown.fromSquare, shown.toSquare) ==
                state.previousHintRemovalSquare;
    }
    if (!previousIsDisplayed) {
        state.previousHintRemovalSquare = state.hintRemovalSquare;
        state.previousHintRemovalElapsed = state.hintRemovalElapsed;
        state.previousHintRemovalDelayMs = state.hintRemovalDelayMs;
    }
    state.hintRemovalSquare = victim;
    state.hintRemovalElapsed = {};
    state.hintRemovalDelayMs = 0U;
    if (victim >= 64U) return;
    state.hintRemovalElapsed.Reset();
    if (fade.elapsed.Started()) {
        const uint32_t age = fade.elapsed.ElapsedMs();
        const bool isIncoming = fade.incoming.fromSquare == from && fade.incoming.toSquare == to;
        const uint32_t start = isIncoming ? fade.durationMs / 2U
            : fade.durationMs + fade.durationMs / 4U;
        state.hintRemovalDelayMs = start > age ? start - age : 0U;
    }
}

void GameStateMachine::SetHintDisplayRequested(const bool requested) {
    runtime::SetHintDisplayRequested(m_renderState, requested);
    SyncHintRemovalCue();
    if (!requested) RetireManualHintRequest();
    RefreshThinkingControls();
}

void GameStateMachine::HandleHintButtonTap() {
    runtime::ToggleHintDisplayRequested(m_renderState);
    SyncHintRemovalCue();
    if (!m_renderState.hintDisplayRequested) RetireManualHintRequest();
    RefreshThinkingControls();
}

void GameStateMachine::NoteManualHintRequest() {
    m_renderState.manualHintAcknowledgmentValid = true;
    m_renderState.manualHintRequested = m_renderState.hintDisplayRequested;
    RefreshThinkingControls();
}

void GameStateMachine::RetireManualHintRequest() {
    m_renderState.manualHintRequested = false;
    m_renderState.manualHintAcknowledgmentValid = false;
    m_renderState.hintReplayCurrent = false;
    m_renderState.hintBrewing = modules::ControlLedThinkingPresentation{};
}

HintPresentationTuple GameStateMachine::GetVisibleHintPresentation(const bool retire) const {
    return m_hintPresentationObserver != nullptr
        ? m_hintPresentationObserver(m_hintPresentationContext, retire)
        : runtime::VisibleHintPresentation(m_renderState);
}

void GameStateMachine::ReplayHintMove() {
    runtime::RetainVisibleHintPresentation(m_renderState, GetVisibleHintPresentation(true));
    SyncHintRemovalCue();
    runtime::ClearHintRefinement(m_renderState);
    runtime::ReplayHintMove(m_renderState);
    SyncHintRemovalCue();
    ++m_renderState.hintPresentationEpoch;
    if (m_hintPresentationPublisher != nullptr)
        m_hintPresentationPublisher(m_hintPresentationContext);
    RefreshThinkingControls();
}

void GameStateMachine::SetHintFeedbackPending(const bool pending) {
    runtime::SetHintFeedbackPending(m_renderState, pending);
    RefreshThinkingControls();
}

void GameStateMachine::SetHintThinkingActive(const bool active) {
    runtime::SetHintThinkingActive(m_renderState, active);
    RefreshThinkingControls();
}

void GameStateMachine::CancelHintRefinement() {
    runtime::ClearHintRefinement(m_renderState);
    m_renderState.hintFeedbackElapsed = fxe::ElapsedTime{};
}

void GameStateMachine::ClearHintMove() {
    runtime::ClearHintMove(m_renderState);
    SyncHintRemovalCue();
    RefreshThinkingControls();
}

void GameStateMachine::SetHintMoveSquares(const uint8_t fromSquare, const uint8_t toSquare) {
    const bool allowRefinement = m_chessInteractionModel.phase.tag == ChessInteractionPhase::Idle &&
        !m_placementModeActive && !m_textOverlayActive && !m_kingQueenSwapPromptActive;
    runtime::SetHintMoveSquares(m_renderState, fromSquare, toSquare, allowRefinement);
    SyncHintRemovalCue();
    RefreshThinkingControls();
}

void GameStateMachine::ClearHintDuckMove() {
    runtime::ClearHintDuckMove(m_renderState);
}

void GameStateMachine::SetHintDuckMoveSquares(
    const uint8_t fromSquare,
    const uint8_t toSquare) {
    runtime::SetHintDuckMoveSquares(m_renderState, fromSquare, toSquare);
}

bool GameStateMachine::TryGetHintDuckMoveSquares(
    uint8_t& outFromSquare,
    uint8_t& outToSquare) const {
    if (m_hintPresentationObserver == nullptr)
        return runtime::TryGetHintDuckMoveSquares(m_renderState, outFromSquare, outToSquare);
    const auto shown = GetVisibleHintPresentation();
    outFromSquare = shown.duckFromSquare;
    outToSquare = shown.duckToSquare;
    return outFromSquare < 64U && outToSquare < 64U;
}

void GameStateMachine::SetHintMoverIsBlack(const bool moverIsBlack) {
    runtime::SetHintMoverIsBlack(m_renderState, moverIsBlack);
}

void GameStateMachine::ClearHintMoverSide() {
    runtime::ClearHintMoverSide(m_renderState);
}

bool GameStateMachine::ConsumeHintCancelRequested() {
    return runtime::ConsumeHintCancelRequested(m_renderState);
}

bool GameStateMachine::IsIllegalFlashOn() const {
    const uint32_t period = (m_config.illegalFlashMs == 0U) ? 1U : m_config.illegalFlashMs;
    const uint32_t elapsedMs = GetIllegalFlashElapsedMs();
    return ((elapsedMs / period) % 2U) == 0U;
}

bool GameStateMachine::IsHumanOConfirmSettleActive() const {
    return m_humanSettleActive && !m_cpuExecution.moveSettleActive &&
           m_activeHumanSettleRequiresO;
}

#ifndef ARDUINO
void GameStateMachine::DebugSetPhysicalOccupancyMask(const uint64_t occ64) {
    const uint32_t nowMs = fxe::Millis();
    m_chessInteractionModel.board.valid = true;
    m_chessInteractionModel.board.changedMask = 0ULL;
    m_chessInteractionModel.board.occupiedMask = occ64;
    m_chessInteractionModel.board.timestampMs = nowMs;
    m_chessInteractionModel.board.stableSinceMs = nowMs;
}
#endif

} // namespace runtime
