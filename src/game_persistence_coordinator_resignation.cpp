#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include "modules/chess_strings.hpp"

namespace modules {

void GamePersistenceCoordinator::ClearKingResignation() {
    m_kingResignationOwner = UiOwner::None;
    m_kingResignationSquare = 255U;
    m_kingResignationStartedAtMs = 0U;
    m_kingResignationPendingOwner = UiOwner::None;
}

void GamePersistenceCoordinator::UpdateKingResignation() {
    const auto& interaction = m_stateMachine.GetChessInteractionModel();
    const bool ordinaryLift =
        interaction.phase.tag == runtime::ChessInteractionPhase::Idle ||
        interaction.phase.tag == runtime::ChessInteractionPhase::PieceLifted ||
        interaction.phase.tag == runtime::ChessInteractionPhase::Correction ||
        (runtime::IsCpuDirectedTransition(interaction.phase.plan) &&
         interaction.phase.plan.kind == runtime::PhysicalTransitionKind::QuietMove);
    if (!m_gameHasAcceptedMove || !interaction.board.valid || !ordinaryLift ||
        IsGameOverVariantAware() || m_litchcraft.IsActive() ||
        m_boardSettingsActive || !m_runtimeHealthClocksAllowed ||
        m_stateMachine.IsPlacementModeActive() ||
        m_stateMachine.IsKingQueenSwapPromptActive() ||
        m_stateMachine.IsUndoPromptActive() || IsUndoActionPending() ||
        m_stateMachine.IsPromotionActive() || m_stateMachine.IsTextOverlayActive() ||
        m_pendingDuckTurn.valid || m_duckTurnPhase != DuckTurnPhase::ChessMove) {
        ClearKingResignation();
        return;
    }
    uint64_t expected = 0ULL;
    const uint64_t physical = m_stateMachine.GetPhysicalOccupancyMask();
    if (!FenToOccupancyMask(m_cachedFen.CStr(), expected)) {
        ClearKingResignation();
        return;
    }
    if (IsDuckGame() && m_duckSquare < 64U) expected |= UINT64_C(1) << m_duckSquare;
    const uint64_t missing = expected & ~physical;
    if ((physical & ~expected) != 0ULL || missing == 0ULL ||
        (missing & (missing - 1ULL)) != 0ULL) {
        ClearKingResignation();
        return;
    }
    uint8_t square = 0U;
    while ((missing & (UINT64_C(1) << square)) == 0ULL) ++square;
    char piece = '\0';
    // Published FEN is safe even while an async worker owns the mutable engine.
    if (!TryGetFenPieceAtSquare(m_cachedFen.CStr(), square, piece) ||
        (piece != 'K' && piece != 'k')) {
        ClearKingResignation();
        return;
    }
    const UiOwner owner = piece == 'K' ? UiOwner::P1 : UiOwner::P2;
    if (m_kingResignationSquare != square || m_kingResignationOwner != owner) {
        m_kingResignationSquare = square;
        m_kingResignationOwner = owner;
        m_kingResignationStartedAtMs = fxe::Millis();
    }
}

bool GamePersistenceCoordinator::HandleKingResignationButton(
    const ButtonEvent& event, const uint32_t eventTimeMs) {
    if (!IsOButton(event.button)) return false;
    const auto owner = ResolveButtonEventOwner(event);
    if (owner == UiOwner::None) return false;
    const uint8_t index = owner == UiOwner::P2 ? 1U : 0U;
    const uint8_t bit = static_cast<uint8_t>(1U << index);
    const bool wasHeld = m_kingResignationCircleHeld[index];
    if (event.event == fxe::Button::Event::Press) m_kingResignationCircleHeld[index] = true;
    if (event.event == fxe::Button::Event::Release) m_kingResignationCircleHeld[index] = false;
    if ((m_kingResignationConsumedReleaseMask & bit) != 0U) {
        if (event.event == fxe::Button::Event::Release)
            m_kingResignationConsumedReleaseMask &= static_cast<uint8_t>(~bit);
        return true;
    }
    // Recheck occupancy/modals before accepting a queued or chord-replayed edge.
    UpdateGameClock();
    UpdateKingResignation();
    if (!IsKingResignationArmed()) return false;
    const uint32_t sinceLift = static_cast<uint32_t>(
        eventTimeMs - m_kingResignationStartedAtMs);
    const uint32_t sincePress = static_cast<uint32_t>(fxe::Millis() - eventTimeMs);
    if (owner == m_kingResignationOwner && !wasHeld &&
        event.event == fxe::Button::Event::Press &&
        sinceLift >= KING_RESIGNATION_HOLD_MS &&
        sincePress <= GetKingResignationElapsedMs()) {
        m_kingResignationConsumedReleaseMask |= bit;
        // Buttons precede Hall polling in production. Only a subsequent fresh,
        // admitted full scan may commit this one-shot intent.
        m_kingResignationPendingOwner = owner;
        m_kingResignationPendingSquare = m_kingResignationSquare;
        m_kingResignationPendingLiftMs = m_kingResignationStartedAtMs;
    }
    // Other Circle events cannot double as Hint or move confirmation while armed.
    return true;
}

void GamePersistenceCoordinator::ResetKingResignationInput() {
    ClearKingResignation();
    m_kingResignationCircleHeld.fill(false);
    m_kingResignationConsumedReleaseMask = 0U;
}

void GamePersistenceCoordinator::ObserveFreshAcceptedBoardScan(
    const uint64_t logicalMask) {
    const UiOwner owner = m_kingResignationPendingOwner;
    const uint8_t square = m_kingResignationPendingSquare;
    const uint32_t liftMs = m_kingResignationPendingLiftMs;
    m_kingResignationPendingOwner = UiOwner::None;
    UpdateGameClock();
    if (logicalMask != m_stateMachine.GetPhysicalOccupancyMask()) {
        ClearKingResignation();
        return;
    }
    // Every accepted full scan owns continuous-lift cancellation, even when
    // no Circle intent exists and the slower engine tick has not run yet.
    UpdateKingResignation();
    if (owner != UiOwner::None &&
        IsKingResignationArmed() && m_kingResignationOwner == owner &&
        m_kingResignationSquare == square &&
        m_kingResignationStartedAtMs == liftMs) {
        ConfirmKingResignation();
    }
}

void GamePersistenceCoordinator::ConfirmKingResignation() {
    const GameWinner winner = m_kingResignationOwner == UiOwner::P1
        ? GameWinner::Black : GameWinner::White;
    m_gameClock.Stop();
    if (ConsumePendingClockTimeout() && HasClockTerminalOutcome()) return;
    m_variantOutcome = {VariantOutcomeReason::Resigned, winner};
    m_variantTerminalStartMs = fxe::Millis();
    ClearKingResignation();
    m_terminalAbandonPending = !AbandonStagedWorkForTerminalOutcome();
    RecordVariantTraceCheckpoint();
    logger::Info("game: resignation winner=%s", GameWinnerToString(winner));
    if (!m_terminalAbandonPending)
        (void)PersistSnapshotDeferredWithRetries("resignation", false);
}

} // namespace modules
