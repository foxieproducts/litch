#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include <litch_max/duck_chess_rules.hpp>
#include <litch_max/duck_chess_search.hpp>
#include "modules/game_persistence_coordinator_update_internal.hpp"
#include "modules/game_persistence_engine_helpers.hpp"
#include "modules/vs_cpu_rng.hpp"
#include "trace/trace_recorder.hpp"

namespace modules {
namespace {

bool SafeGetFen(IEngine& engine, FenString& outFen) {
    return game_persistence_engine_helpers::SafeGetFen(
        engine, outFen, true);
}

GameWinner WinnerForSide(const SideToMove side) {
    return side == SideToMove::White ? GameWinner::White
                                     : GameWinner::Black;
}

void ResetOrthodoxDerivedState(EngineDerivedState& state,
                               const SideToMove side) {
    state.Reset();
    state.sideToMove = side;
}

bool IsHintedDuckDestinationValid(
    const litch_max::DuckChessMoveResult& applied,
    const uint8_t duckFrom,
    const uint8_t duckTo) {
    char piece = '\0';
    return duckTo < 64U && duckTo != duckFrom &&
           !game_persistence_update_internal::TryGetFenPieceAtSquare(
               applied.resultingFen.CStr(), duckTo, piece);
}

} // namespace

void GamePersistenceCoordinator::UpdateInitialDuckObstructionRelocation() {
    if (!m_initialDuckObstructionRelocationPending) {
        return;
    }
    if (!IsDuckGame() || !IsInitialVariantSelectionReversible() ||
        HasClockTerminalOutcome()) {
        m_initialDuckObstructionRelocationPending = false;
        m_initialDuckObstructedFromSquare = INVALID_DUCK_SQUARE;
        m_externalizedInitialDuckRelocationActionPending = false;
        return;
    }
    if (m_stateMachine.IsDuckRelocationActive()) {
        return;
    }
    const auto phase =
        m_stateMachine.GetChessInteractionModel().phase.tag;
    if (phase != runtime::ChessInteractionPhase::Idle ||
        m_stateMachine.HasHumanIllegal() ||
        !m_stateMachine.IsExpectedOccupancyMatched()) {
        return;
    }
    if (!QuiesceAllThinking()) {
        m_cancelAllThinkingRequested = true;
        return;
    }
    ClearCachedHintResults();
    ClearCachedLegalDestinations();
    m_stateMachine.SetHintDisplayRequested(false);
    m_stateMachine.ClearHintMove();
    (void)m_stateMachine.BeginDuckRelocation(
        m_duckSquare,
        runtime::CHESS_INTERACTION_INVALID_SQUARE,
        runtime::PhysicalTransitionSource::Human,
        runtime::DirectedMoveOrigin::Cpu);
}

bool GamePersistenceCoordinator::StageDuckChessMove(
    const runtime::PhysicalTransitionPlan& move,
    const char promotionSuffix) {
    if (!IsDuckGame() || m_duckTurnPhase != DuckTurnPhase::ChessMove ||
        m_pendingDuckTurn.valid || m_variantOutcome.IsTerminal()) {
        DispatchPendingInteractionActionResult(false);
        return false;
    }

    UciMove uci;
    if (!MakeUciMove(move.moveFrom, move.moveTo,
                     promotionSuffix, uci)) {
        DispatchPendingInteractionActionResult(false);
        return false;
    }

    litch_max::DuckChessMoveResult applied;
    if (!litch_max::TryApplyDuckMove(m_cachedFen.CStr(), m_duckSquare,
                          uci.CStr(), applied)) {
        logger::Warn("duck: rejected chess move %s", uci.CStr());
        DispatchPendingInteractionActionResult(false);
        return false;
    }

    PendingDuckChessTurn pending;
    pending.valid = true;
    pending.uci = uci;
    pending.afterFen = applied.resultingFen;
    pending.capturedKing = applied.capturedKing;
    pending.winner = WinnerForSide(m_engine.GetSideToMove());
    pending.source = move.source;
    pending.directedOrigin = move.directedOrigin;
    // Continue the displayed compound suggestion, including its promotion.
    const DuckThinkResult* hintedTurn =
        move.source == runtime::PhysicalTransitionSource::Human &&
        m_stateMachine.IsHintDisplayRequested() &&
        m_displayedDuckHintTurn.chessMove.Equals(uci.View())
            ? &m_displayedDuckHintTurn : &m_duckHintTurn;
    if (!applied.capturedKing) {
        if (runtime::IsCpuDirectedTransition(move) &&
            m_pendingCpuDuckDestinationValid) {
            pending.requiredDuckDestination =
                m_pendingCpuDuckDestination;
        } else if (move.source ==
                       runtime::PhysicalTransitionSource::Human &&
                   m_duckHintTurnValid &&
                   m_duckHintFen.Equals(m_cachedFen.View()) &&
                   m_duckHintSquare == m_duckSquare &&
                   hintedTurn != nullptr &&
                   hintedTurn->chessMove.Equals(uci.View()) &&
                   IsHintedDuckDestinationValid(
                       applied,
                       m_duckSquare,
                       hintedTurn->duckDestination)) {
            pending.requiredDuckDestination =
                hintedTurn->duckDestination;
        } else {
            litch_max::DuckChessSearchConfig config;
            config.maxNodes = 128U;
            config.rngSeed = 1U;
            config.difficulty = 11U;
            litch_max::DuckChessSearchStats stats;
            (void)litch_max::FindBestDuckPlacement(
                applied.resultingFen.CStr(), m_duckSquare, config,
                pending.requiredDuckDestination, stats);
        }
    }
    game_persistence_update_internal::BuildUndoPersistFieldsFromFen(
        m_cachedFen.CStr(), m_engine.GetSideToMove(),
        move.moveFrom, move.moveTo,
        applied.captureSquareValid, applied.captureSquare,
        static_cast<uint8_t>(promotionSuffix),
        pending.undoFields);
    pending.nextHalfmoveClockPlies = NextHalfmoveClockPlies(
        m_cachedFen.CStr(), m_halfmoveClockPlies,
        move.moveFrom, move.moveTo,
        applied.captureSquareValid, applied.captureSquare);
    if (move.source == runtime::PhysicalTransitionSource::Human) {
        PrepareTakebackGrace(move);
    }
    m_pendingDuckTurn = pending;
    if (runtime::IsCpuDirectedTransition(pending.source,
                                         pending.directedOrigin)) {
        // The staged owner now contains the complete choice. Do not persist
        // an obsolete queued chess move alongside its fixed Duck relocation.
        ClearPendingCpuMoveSelection();
    }

    // The physical chess choreography is now complete. Acknowledge it so the
    // reducer owns the post-chess physical projection before replacing the
    // plan with the mandatory duck relocation.
    DispatchPendingInteractionActionResult(true);
    UpdateLastMovePulseFromUndoHistory();
    if (m_interactionEventsExternalized) {
        m_externalizedDuckChessActionPending = true;
        return true;
    }
    return ContinueStagedDuckTurnAfterChessAction();
}

bool GamePersistenceCoordinator::ContinueStagedDuckTurnAfterChessAction() {
    if (!m_pendingDuckTurn.valid) {
        return false;
    }
    if (m_pendingDuckTurn.capturedKing) {
        return CommitStagedDuckTurn(
            m_duckSquare, m_duckSquare, false);
    }

    m_duckTurnPhase = DuckTurnPhase::DuckRelocation;
    const bool cpuRelocation = runtime::IsCpuDirectedTransition(
        m_pendingDuckTurn.source, m_pendingDuckTurn.directedOrigin);
    const uint8_t requiredDestination = cpuRelocation
                                            ? m_pendingDuckTurn.requiredDuckDestination
                                            : runtime::CHESS_INTERACTION_INVALID_SQUARE;
    if (!m_stateMachine.BeginDuckRelocation(
            m_duckSquare, requiredDestination,
            m_pendingDuckTurn.source,
            m_pendingDuckTurn.directedOrigin)) {
        logger::Warn("duck: failed to start relocation from=%u",
                     static_cast<unsigned>(m_duckSquare));
        m_pendingDuckTurn = PendingDuckChessTurn{};
        m_duckTurnPhase = DuckTurnPhase::ChessMove;
        m_stateMachine.ClearHintMove();
        ClearTakebackGrace();
        UpdateLastMovePulseFromUndoHistory();
        ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
        return false;
    }

    if (!cpuRelocation &&
        m_pendingDuckTurn.requiredDuckDestination < 64U) {
        m_stateMachine.SetDuckRelocationHint(
            m_duckSquare,
            m_pendingDuckTurn.requiredDuckDestination);
    } else {
        m_stateMachine.ClearHintMove();
    }

    (void)PersistSnapshotDeferredWithRetries(
        "duck_relocation_pending", false);
    RecordVariantTraceCheckpoint();
    return true;
}

bool GamePersistenceCoordinator::CommitDuckRelocation(
    const runtime::PhysicalTransitionPlan& relocation) {
    if (m_initialDuckObstructionRelocationPending &&
        !m_pendingDuckTurn.valid) {
        return CommitInitialDuckObstructionRelocation(relocation);
    }
    if (!IsDuckGame() || !m_pendingDuckTurn.valid ||
        m_duckTurnPhase != DuckTurnPhase::DuckRelocation ||
        relocation.kind != runtime::PhysicalTransitionKind::DuckRelocation ||
        relocation.moveFrom != m_duckSquare ||
        relocation.moveTo >= 64U ||
        relocation.moveTo == relocation.moveFrom) {
        DispatchPendingInteractionActionResult(false);
        return false;
    }
    if (m_interactionEventsExternalized) {
        m_externalizedDuckRelocationActionPending = true;
        m_externalizedDuckFrom = relocation.moveFrom;
        m_externalizedDuckTo = relocation.moveTo;
        DispatchPendingInteractionActionResult(true);
        return true;
    }
    DispatchPendingInteractionActionResult(true);
    return CommitStagedDuckTurn(
        relocation.moveFrom, relocation.moveTo, true);
}

bool GamePersistenceCoordinator::CommitInitialDuckObstructionRelocation(
    const runtime::PhysicalTransitionPlan& relocation) {
    if (!m_initialDuckObstructionRelocationPending ||
        !IsInitialVariantSelectionReversible() ||
        relocation.kind != runtime::PhysicalTransitionKind::DuckRelocation ||
        relocation.moveFrom != m_duckSquare ||
        relocation.moveTo >= 64U ||
        relocation.moveTo == relocation.moveFrom) {
        DispatchPendingInteractionActionResult(false);
        return false;
    }
    if (m_interactionEventsExternalized) {
        m_externalizedInitialDuckRelocationActionPending = true;
        m_externalizedDuckFrom = relocation.moveFrom;
        m_externalizedDuckTo = relocation.moveTo;
        return true;
    }
    DispatchPendingInteractionActionResult(true);
    return FinalizeInitialDuckObstructionRelocation(
        relocation.moveFrom, relocation.moveTo);
}

bool GamePersistenceCoordinator::FinalizeInitialDuckObstructionRelocation(
    const uint8_t fromSquare,
    const uint8_t toSquare) {
    if (!m_initialDuckObstructionRelocationPending ||
        !IsInitialVariantSelectionReversible() ||
        fromSquare != m_duckSquare || toSquare >= 64U ||
        toSquare == fromSquare) {
        return false;
    }
    m_duckSquare = toSquare;
    m_initialDuckObstructionRelocationPending = false;
    m_initialDuckObstructedFromSquare = INVALID_DUCK_SQUARE;
    ClearLegalDestinationsMask();
    ClearCachedLegalDestinations();
    ClearCachedHintResults();
    m_stateMachine.SetHintDisplayRequested(false);
    m_stateMachine.ClearHintMove();
    ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
    RecordVariantTraceCheckpoint();
    logger::Trace("duck: free opening relocation from=%u to=%u",
                 static_cast<unsigned>(fromSquare),
                 static_cast<unsigned>(toSquare));
    return PersistSnapshotDeferredWithRetries(
        "initial_duck_obstruction_relocated", false);
}

bool GamePersistenceCoordinator::ResumeExternalizedDuckActionAfterCheckpoint() {
    if (!m_interactionEventsExternalized) {
        return true;
    }
    if (m_externalizedInitialDuckRelocationActionPending) {
        if (m_stateMachine.GetChessInteractionModel().phase.tag !=
            runtime::ChessInteractionPhase::Idle) {
            return true;
        }
        const uint8_t duckFrom = m_externalizedDuckFrom;
        const uint8_t duckTo = m_externalizedDuckTo;
        m_externalizedInitialDuckRelocationActionPending = false;
        m_externalizedDuckFrom = INVALID_DUCK_SQUARE;
        m_externalizedDuckTo = INVALID_DUCK_SQUARE;
        return FinalizeInitialDuckObstructionRelocation(
            duckFrom, duckTo);
    }
    // Hardware traces include a pre-action checkpoint while the reducer still
    // awaits the recorded result. Usually the result reaches Idle. An early
    // human duck pickup legitimately leaves the acknowledged chess plan as an
    // ExecutingPlan bridge so the following Duck plan can inherit that lift.
    if (m_externalizedDuckChessActionPending) {
        const runtime::TaggedInteractionState& phase =
            m_stateMachine.GetChessInteractionModel().phase;
        const bool acknowledgedBridge =
            phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
            phase.plan.source ==
                runtime::PhysicalTransitionSource::Human &&
            phase.plan.progress.actionAcknowledged;
        if (phase.tag != runtime::ChessInteractionPhase::Idle &&
            !acknowledgedBridge) {
            return true;
        }
        m_externalizedDuckChessActionPending = false;
        return ContinueStagedDuckTurnAfterChessAction();
    }
    if (!m_externalizedDuckRelocationActionPending) {
        if (!m_externalizedDuckUndoActionPending &&
            !m_externalizedDuckRedoActionPending) {
            return true;
        }
        if (m_stateMachine.GetChessInteractionModel().phase.tag !=
            runtime::ChessInteractionPhase::Idle) {
            return true;
        }
        const bool valid = m_externalizedDuckFixedActionValid;
        const uint8_t duckTo = m_externalizedDuckTo;
        const bool undo = m_externalizedDuckUndoActionPending;
        m_externalizedDuckUndoActionPending = false;
        m_externalizedDuckRedoActionPending = false;
        m_externalizedDuckFixedActionValid = false;
        m_externalizedDuckFrom = INVALID_DUCK_SQUARE;
        m_externalizedDuckTo = INVALID_DUCK_SQUARE;
        if (undo) {
            ContinueDuckUndoRelocationAction(valid);
        } else {
            ContinueDuckRedoRelocationAction(valid, duckTo);
        }
        return true;
    }
    if (m_stateMachine.GetChessInteractionModel().phase.tag !=
        runtime::ChessInteractionPhase::Idle) {
        return true;
    }
    const uint8_t duckFrom = m_externalizedDuckFrom;
    const uint8_t duckTo = m_externalizedDuckTo;
    m_externalizedDuckRelocationActionPending = false;
    m_externalizedDuckFrom = INVALID_DUCK_SQUARE;
    m_externalizedDuckTo = INVALID_DUCK_SQUARE;
    return CommitStagedDuckTurn(duckFrom, duckTo, true);
}

bool GamePersistenceCoordinator::CommitStagedDuckTurn(
    const uint8_t duckFrom,
    const uint8_t duckTo,
    const bool duckRelocated) {
    if (!m_pendingDuckTurn.valid ||
        (duckRelocated &&
         (duckFrom >= 64U || duckTo >= 64U || duckFrom == duckTo))) {
        ClearTakebackGrace();
        return false;
    }
    if (!CanAppendUndoHistoryEntry()) {
        ClearTakebackGrace();
        logger::Warn("duck: undo history rollover failed");
        return false;
    }

    const PendingDuckChessTurn pending = m_pendingDuckTurn;
    const FenString previousFen = m_cachedFen;
    if (!m_engine.SetFen(pending.afterFen.CStr())) {
        ClearTakebackGrace();
        logger::Warn("duck: engine rejected committed FEN");
        return false;
    }
    if (!SafeGetFen(m_engine, m_cachedFen)) {
        m_cachedFen = pending.afterFen;
    }

    if (!AppendUndoHistoryEntry(
            pending.undoFields[0], pending.undoFields[1],
            pending.undoFields[2], pending.undoFields[3],
            pending.undoFields[4], pending.undoFields[5],
            pending.undoFields[6])) {
        ClearTakebackGrace();
        logger::Warn("duck: undo history append failed");
        if (!m_engine.SetFen(previousFen.CStr())) {
            logger::Warn("duck: engine rollback failed after history error");
        }
        m_cachedFen = previousFen;
        return false;
    }
    if (m_duckUndoMoveCount != 0U) {
        const uint16_t last =
            static_cast<uint16_t>(m_duckUndoMoveCount - 1U);
        m_duckUndoFrom[last] = duckRelocated
                                   ? duckFrom
                                   : INVALID_DUCK_SQUARE;
        m_duckUndoTo[last] = duckRelocated
                                 ? duckTo
                                 : INVALID_DUCK_SQUARE;
    }

    m_halfmoveClockPlies = pending.nextHalfmoveClockPlies;
    m_vsCpuRngSeed = NextVsCpuSeedAfterMove(
        m_vsCpuRngSeed, pending.uci.CStr());
    if (duckRelocated) {
        m_duckSquare = duckTo;
    }
    m_duckTurnPhase = DuckTurnPhase::ChessMove;
    m_pendingDuckTurn = PendingDuckChessTurn{};
    ClearCachedHintResults();
    m_stateMachine.SetHintDisplayRequested(false);
    m_stateMachine.ClearHintMove();

    if (pending.capturedKing) {
        m_variantOutcome = VariantOutcome{
            VariantOutcomeReason::KingCaptured, pending.winner};
    } else if (!litch_max::HasAnyDuckChessMove(
                   m_cachedFen.CStr(), m_duckSquare)) {
        // Duck Chess fowling awards the win to the player who begins a turn
        // with no pseudo-legal chess move.
        m_variantOutcome = VariantOutcome{
            VariantOutcomeReason::Fowled,
            WinnerForSide(m_engine.GetSideToMove())};
    }

    if (m_variantOutcome.IsTerminal()) m_variantTerminalStartMs = fxe::Millis();

    (void)PushFenHistory(
        m_cachedFen.CStr(), m_halfmoveClockPlies);
    ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());
    UpdateLastMovePulseFromUndoHistory();
    ResetOrthodoxDerivedState(
        m_engineDerived, m_engine.GetSideToMove());
    RefreshDrawStateFromTracking();

    if (m_traceRecorder != nullptr && m_traceRecorder->IsActive()) {
        m_traceRecorder->RecordCheckpointEngine(m_cachedFen.CStr());
    }
    RecordVariantTraceCheckpoint();
    RefreshPreparedTakebackGraceClockSnapshot();
    CommitGameClockMove(ToClockSide(m_engineDerived.sideToMove));
    UpdateGameClock();
    ArmPreparedTakebackGrace(
        !pending.capturedKing && duckRelocated,
        duckFrom,
        duckTo);
    logger::Trace("duck: turn=%s duck=%u outcome=%s",
                 pending.uci.CStr(),
                 static_cast<unsigned>(m_duckSquare),
                 VariantOutcomeReasonToString(m_variantOutcome.reason));
    return PersistSnapshotDeferredWithRetries(
        pending.capturedKing ? "duck_king_capture"
                             : "duck_turn_finalized",
        false);
}

} // namespace modules
