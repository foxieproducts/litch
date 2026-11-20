#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include "modules/game_history_protocol.hpp"
#include "modules/game_persistence_engine_helpers.hpp"
#include "trace/trace_recorder.hpp"

namespace modules {
namespace {

bool SafeGetExternalFen(IEngine& engine, FenString& outFen) {
    return game_persistence_engine_helpers::SafeGetFen(
        engine, outFen, true);
}

bool IsValidExternalFen(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    size_t length = 0U;
    while (length < litch_max::FEN_BUFFER_CAPACITY && value[length] != '\0') {
        const unsigned char character =
            static_cast<unsigned char>(value[length]);
        if (character < 32U || character > 126U) {
            return false;
        }
        ++length;
    }
    if (length == litch_max::FEN_BUFFER_CAPACITY) {
        return false;
    }
    game_history::Position position;
    return game_history::EncodePosition(
        value, game_history::Variant::Classic,
        game_history::INVALID_SQUARE, position);
}

} // namespace

bool GamePersistenceCoordinator::LoadExternalFen(const char* fen) {
    // Validate the entire bounded input before requesting cancellation or
    // changing any queued move, Hint, history or persistence state.
    if (!IsValidExternalFen(fen)) {
        return false;
    }

    const auto admission = CheckReplacementAdmission(ReplacementKind::ExternalFen);
    if (admission != ControlCoordinatorResult::Applied) {
        logger::Warn("engine: external FEN rejected: %s",
            ControlMutationReasonToString(
                ControlMutationReasonFromCoordinatorResult(admission)));
        return false;
    }

    CancelPonder(true);
    // Preserve the complete current game while the shared engine lane still
    // owns PollThinkProgress. The caller receives a truthful rejection and can
    // retry after cancellation completes.
    if (!QuiesceAllThinking()) {
        return false;
    }
    if (!m_engine.SetFen(fen)) {
        return false;
    }
    ClearPendingCpuMoveSelection();
    m_hintPendingStart = false;
    m_cancelAllThinkingRequested = false;
    m_stateMachine.SetHintDisplayRequested(false);
    m_stateMachine.ClearHintMove();
    ClearCachedHintResults();
    m_hintLastSideToMoveValid = false;
    m_hintPendingStart = false;

    if (!SafeGetExternalFen(m_engine, m_cachedFen)) {
        logger::Warn(
            "engine: GetFen failed after SetFen(external); using provided FEN");
        (void)m_cachedFen.Assign(fen);
    }

    m_gameVariant = GameVariant::Classic;
    m_duckSquare = INVALID_DUCK_SQUARE;
    m_duckTurnPhase = DuckTurnPhase::ChessMove;
    m_variantOutcome = VariantOutcome{};
    m_pendingDuckTurn = PendingDuckChessTurn{};
    m_stagedDuckChessUndoPending = false;
    m_externalizedDuckChessActionPending = false;
    m_externalizedDuckRelocationActionPending = false;
    m_externalizedDuckUndoActionPending = false;
    m_externalizedDuckRedoActionPending = false;
    m_externalizedDuckFixedActionValid = false;
    m_externalizedDuckRedoStartPending = false;
    m_externalizedDuckFrom = INVALID_DUCK_SQUARE;
    m_externalizedDuckTo = INVALID_DUCK_SQUARE;
    m_duckUndoRelocationPending = false;
    m_duckRedoRelocationPending = false;
    m_duckUndoRequestedCount = 0U;
    m_variantDetectionArmed = false;

    m_promotionPending = false;
    m_restoredPromotionRequiresChoice = false;
    m_stateMachine.ResetForExternalEngineMutation();
    m_stateMachine.ClearMovedSquares();
    ClearUndoHistory();
    ClearFenHistory();
    (void)BeginNewGameHistory(true);
    m_halfmoveClockPlies = 0U;
    (void)m_undoBaseFen.Assign(fen);
    m_undoBaseMovedSquaresMask = 0ULL;
    ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());

    if (m_traceRecorder != nullptr && m_traceRecorder->IsActive()) {
        m_traceRecorder->RecordCheckpointEngine(m_cachedFen.CStr());
    }
    (void)m_stateMachine.TryEnterPlacementMode();
    m_engineDerived.RefreshFromEngine(m_engine);
    SeedFenHistoryFromCurrentFen();
    RefreshDrawStateFromTracking();
    ClearClockTerminalOutcome();
    m_cachedTimerSetting = UINT32_MAX;
    UpdateGameClock();
    RecordVariantTraceCheckpoint();
    return PersistSnapshotDeferredWithRetries(
        "load_external_fen", true);
}

} // namespace modules
