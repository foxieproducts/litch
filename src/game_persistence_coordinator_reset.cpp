#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include "modules/first_player_migration.hpp"
#include "modules/game_persistence_engine_helpers.hpp"
#include "modules/initial_fen.hpp"
#include "trace/trace_recorder.hpp"

namespace modules {

bool GamePersistenceCoordinator::ResetEngine() {
    if (m_litchcraft.IsActive()) return false;
    ControlDispatchContext context{*this};
    const uint32_t sequence = m_eventIngress.Dispatch(
        runtime::AppEvent::MakeResetRequested(fxe::Millis()),
        &context,
        &GamePersistenceCoordinator::DispatchControlEvent);
    return sequence != 0U && context.result;
}

bool GamePersistenceCoordinator::ApplyResetRequest() {
    return ResetEngineInternal(true);
}

ControlCoordinatorResult GamePersistenceCoordinator::CheckReplacementAdmission(
    const ReplacementKind kind) const {
    // Standalone/native owners and explicitly volatile boot policy retain
    // their existing behavior. Enabled production storage always wires this.
    if (m_replacementAdmissionCallback == nullptr) {
        return ControlCoordinatorResult::Applied;
    }
    const auto admission = m_replacementAdmissionCallback(
        m_replacementAdmissionContext, m_gameHistoryPersistence, kind);
    if (admission == ControlCoordinatorResult::StorageUnavailable ||
        admission == ControlCoordinatorResult::Unsupported) {
        return admission;
    }
    if (IsUndoActionPending()) return ControlCoordinatorResult::Busy;
    return admission;
}

bool GamePersistenceCoordinator::ResetEngineInternal(const bool resetKqPromptTracking) {
    if (CheckReplacementAdmission(ReplacementKind::NewGame) !=
        ControlCoordinatorResult::Applied) {
        return false;
    }
    // New game must not reset VS-CPU difficulty. Preserve the setting across
    // the reset + forced persistence write.
    const uint32_t preservedVsCpuLevel = ReadVsCpuLevelSetting();

    CancelPonder(true);
    // New Game is synchronous from the control caller's perspective. Reject
    // it without disturbing the current game while worker cancellation is
    // still pending; a later explicit request can retry safely.
    if (!QuiesceAllThinking()) {
        return false;
    }
    ClearPendingCpuMoveSelection();
    m_hintPendingStart = false;
    m_cancelAllThinkingRequested = false;
    m_stateMachine.SetHintDisplayRequested(false);
    m_stateMachine.ClearHintMove();

    ClearCachedHintResults();

    // New game reset: restart the hint pipeline on the next Update() tick.
    m_hintLastSideToMoveValid = false;
    m_hintPendingStart = false;

    m_engine.Reset();
    const auto& doc = m_settings.Doc();
    const bool firstPlayerIsP2 = first_player_migration::ReadFirstPlayerIsP2FromDoc(doc);
    const char* initialFen = modules::InitialFenForFirstPlayerIsP2(firstPlayerIsP2);

    if (!m_engine.SetFen(initialFen)) {
        return false;
    }
    // New game must never inherit a stale cached FEN. Seed to INITIAL_FEN first so
    // any transient GetFen() failure after reset still yields the correct side-to-move.
    (void)m_cachedFen.Assign(initialFen);
    if (!game_persistence_engine_helpers::SafeGetFen(m_engine, m_cachedFen, true)) {
        logger::Warn("engine: GetFen failed after SetFen(initial); using constant");
    }

    m_promotionPending = false;
    m_restoredPromotionRequiresChoice = false;
    m_stateMachine.ResetForExternalEngineMutation();
    m_stateMachine.ClearMovedSquares();

    ResetGameVariantForNewGame();

    // Explicitly clear last-move overlays on a new game.
    m_stateMachine.ClearLastMoveForSide(modules::PersistedSide::White);
    m_stateMachine.ClearLastMoveForSide(modules::PersistedSide::Black);

    ClearUndoHistory();
    ClearFenHistory();
    (void)BeginNewGameHistory(false);

    m_halfmoveClockPlies = 0;

    (void)m_undoBaseFen.Assign(initialFen);
    m_undoBaseMovedSquaresMask = 0;

    ApplyEngineFenToStateMachineExpected(m_cachedFen.CStr());

    if (m_traceRecorder && m_traceRecorder->IsActive()) {
        m_traceRecorder->RecordCheckpointEngine(m_cachedFen.CStr());
    }
    if (!ShouldDeferInitialPlacementForVariantDetection(
            m_stateMachine.GetPhysicalOccupancyMask())) {
        (void)m_stateMachine.TryEnterPlacementMode();
    }
    m_stateMachine.TriggerInitialKingPlacementPrompt();
    m_engineDerived.RefreshFromEngine(m_engine);

    SeedFenHistoryFromCurrentFen();
    RefreshDrawStateFromTracking();

    // Reset is a new game; reset the clock to the preset.
    m_cachedTimerSetting = UINT32_MAX;
    UpdateGameClock();

    (void)PersistSnapshotDeferredWithRetries("reset_engine", true);

    if (preservedVsCpuLevel != 0U) {
        auto& doc = m_settings.Doc();
        const uint32_t cur = doc.containsKey("VS_CPU_LEVEL") ? (doc["VS_CPU_LEVEL"] | 0U) : 0U;
        if (cur != preservedVsCpuLevel) {
            doc["VS_CPU_LEVEL"] = preservedVsCpuLevel;
            m_settings.MarkDirty();
        }
    }

    if (resetKqPromptTracking) {
        // Reset prompt toggle tracking; this reset is an explicit new-game mutation.
        m_kqPromptInitialized = false;
        m_kqPromptBaselineValid = false;
        m_kqPromptGeneration = 0;
        m_kqPromptLastStartedGeneration = 0;
    }
    RecordVariantTraceCheckpoint();
    return true;
}

} // namespace modules
