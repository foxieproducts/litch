#include "modules/game_persistence_coordinator.hpp"

#include <cstring>

#include "logger.hpp"
#include <litch_max/duck_chess_rules.hpp>
#include <litch_max/fen_constants.hpp>
#include "modules/first_player_migration.hpp"
#include "modules/game_persistence_coordinator_update_internal.hpp"
#include "modules/game_persistence_engine_helpers.hpp"
#include "modules/initial_fen.hpp"
#include "runtime/chess_interaction_persistence.hpp"
#include "trace/trace_recorder.hpp"

namespace modules {
namespace {

bool SafeGetFen(IEngine& engine, FenString& outFen) {
    return game_persistence_engine_helpers::SafeGetFen(
        engine, outFen, true);
}

bool GetFirstPlayerIsP2FromDoc(const JsonDocument& doc) {
    return modules::first_player_migration::ReadFirstPlayerIsP2FromDoc(doc);
}

bool MigrateLegacyFirstPlayerSetting(modules::SettingsStore& settings) {
    return modules::first_player_migration::CanonicalizeAndCleanup(settings.Doc());
}

bool ValidateDuckCpuPayload(
    const char* fen,
    const uint8_t duckSquare,
    const UciMove& uci,
    const GamePersistenceState& persisted,
    bool& outCapturedKing) {
    litch_max::DuckChessMoveResult applied;
    if (!litch_max::TryApplyDuckMove(
            fen, duckSquare, uci.CStr(), applied)) {
        return false;
    }
    outCapturedKing = applied.capturedKing;
    if (applied.captureSquareValid !=
            persisted.cpuMoveCaptureSquareValid ||
        (applied.captureSquareValid &&
         applied.captureSquare != persisted.cpuMoveCaptureSquare)) {
        return false;
    }
    if (applied.capturedKing) {
        return !persisted.cpuMoveDuckDestinationValid;
    }
    char occupied = '\0';
    return persisted.cpuMoveDuckDestinationValid &&
           persisted.cpuMoveDuckDestination != duckSquare &&
           !game_persistence_update_internal::TryGetFenPieceAtSquare(
               applied.resultingFen.CStr(),
               persisted.cpuMoveDuckDestination, occupied);
}

} // namespace

bool GamePersistenceCoordinator::LoadOrInitialize() {
    bool persistSnapshot = false;
    bool clearUndoForSnapshot = false;
    if (!RestoreOrInitializeState(
            persistSnapshot, clearUndoForSnapshot)) {
        return false;
    }
    // RestoreOrInitializeState owns a large bounded decode frame. Let it
    // unwind before constructing the similarly large persistence snapshot;
    // nesting both overflows the ESP32 Arduino loop task on a fresh game.
    if (persistSnapshot) {
        (void)PersistSnapshot(clearUndoForSnapshot, false);
    }
    return true;
}

bool GamePersistenceCoordinator::RestoreOrInitializeState(
    bool& outPersistSnapshot,
    bool& outClearUndoForSnapshot) {
    outPersistSnapshot = false;
    outClearUndoForSnapshot = false;
    // This method may be called on a reused coordinator. Do not erase search
    // ownership or restore the engine until the prior worker has actually
    // stopped touching it.
    CancelPonder(true);
    if (!QuiesceAllThinking()) {
        return false;
    }

    auto& doc = m_settings.Doc();

    // Transient state must not survive across LoadOrInitialize() calls.
    // Tests and firmware may reuse a coordinator instance across resets.
    m_gameClock.ResetFromTimerSetting(0U, runtime::ClockSide::White);
    m_cachedTimerSetting = UINT32_MAX;
    m_clockSessionId = 0U;
    m_clockDurableEpoch = 0U;
    m_clockSessionStarted = false;
    m_pregameClockGraceArmed = false;
    m_pregameClockProgressSeen = false;
    m_pregameClockProgressStartedAtMs = 0U;
    m_pregameClockGraceElapsed.Reset();
    m_restoredClockResumeAuthorizationPending = false;
    ClearClockTerminalOutcome();
    m_vsCpuThinking = false;
    m_hintThinking = false;
    m_hintPendingStart = false;
    m_coachingHintActive = false;
    m_hintThinkAutomatic = false;
    m_cancelAllThinkingRequested = false;
    m_cancelCpuWorkRequested = false;
    m_cancelHintWorkRequested = false;

    ClearPendingCpuMoveSelection();
    m_lastCastlingRookPromptActive = false;
    ClearCachedHintResults();

    m_stateMachine.SetCpuThinkingActive(false);
    m_stateMachine.SetHintThinkingActive(false);
    m_stateMachine.SetHintDisplayRequested(false);
    m_stateMachine.ClearHintMove();
    m_stateMachine.CancelQueuedCpuMove();

    // LoadOrInitialize can be called multiple times on a reused coordinator
    // instance (tests, recovery flows). Ensure transient render-facing state
    // like last-move highlights never leaks across a reset/reinit.
    m_stateMachine.ClearLastMoveForSide(modules::PersistedSide::White);
    m_stateMachine.ClearLastMoveForSide(modules::PersistedSide::Black);

    GamePersistenceState persisted;
    if (!ReadGamePersistenceState(doc, persisted)) {
        DispatchInteractionAdapterResult(
            runtime::AppEventType::RestoreResult,
            "restore",
            false);
        return false;
    }
    const bool initializedNewGame = persisted.fen.Empty();

    bool firstPlayerIsP2 = GetFirstPlayerIsP2FromDoc(doc);

    // Migration: earlier builds experimented with a swapped-colors initial FEN.
    // That position blocks white pawns behind the back rank and is not compatible
    // with the current model (we keep a standard FEN and handle side ownership via
    // physical square mapping). If we detect that legacy start FEN, treat the
    // persisted game as invalid and reinitialize.
    if (!persisted.fen.Empty() &&
        std::strcmp(persisted.fen.CStr(), modules::LEGACY_INITIAL_FEN_P2_OWNS_WHITE_SWAPPED_COLORS) == 0) {
        ClearGameState(doc);
        ClearUndoState(doc);
        m_settings.NotifyDocChanged();
        persisted = GamePersistenceState{};
    }

    // Canonicalize first-player storage after boot/load if legacy keys or numeric
    // values are present.
    if (MigrateLegacyFirstPlayerSetting(m_settings)) {
        m_settings.NotifyDocChanged();
        firstPlayerIsP2 = m_settings.ReadValue("GAME_FIRST_PLAYER", static_cast<uint32_t>(1U)) == 2U;
    }

    if (!persisted.fen.Empty()) {
        if (!m_engine.SetFen(persisted.fen.CStr())) {
            DispatchInteractionAdapterResult(
                runtime::AppEventType::RestoreResult,
                "restore",
                false);
            return false;
        }
        // Canonicalize the cached FEN from the engine. Some engines normalize
        // FEN text on SetFen() (castling, en-passant, etc). If m_cachedFen
        // disagrees, Update() will mis-detect an "external mutation" and can
        // incorrectly enter placement mode.
        if (!SafeGetFen(m_engine, m_cachedFen)) {
            m_cachedFen = persisted.fen;
        }
    } else {
        const char* initialFen = modules::InitialFenForFirstPlayerIsP2(firstPlayerIsP2);

        m_engine.Reset();
        if (!m_engine.SetFen(initialFen)) {
            DispatchInteractionAdapterResult(
                runtime::AppEventType::RestoreResult,
                "restore",
                false);
            return false;
        }
        if (!SafeGetFen(m_engine, m_cachedFen)) {
            logger::Warn("engine: GetFen failed after SetFen(initial); using constant");
            (void)m_cachedFen.Assign(initialFen);
        }

        // Seed the base now; the complete initial snapshot is written after
        // variant detection and all runtime state have been initialized.
        (void)m_undoBaseFen.Assign(initialFen);
        m_undoBaseMovedSquaresMask = 0;
    }

    // Restore coordinator-owned undo history fields.
    ClearUndoHistory();
    if (persisted.undoHistoryCount != 0) {
        if (persisted.undoHistoryCount > static_cast<uint16_t>(sizeof(m_undoHistoryFields))) {
            DispatchInteractionAdapterResult(
                runtime::AppEventType::RestoreResult,
                "restore",
                false);
            return false;
        }
        std::memcpy(m_undoHistoryFields, persisted.undoHistory, persisted.undoHistoryCount);
        m_undoHistoryFieldCount = persisted.undoHistoryCount;
    }
    m_halfmoveClockPlies = ComputeHalfmoveClockPliesFromUndoHistory();

    // Restore moved squares.
    uint64_t movedMask = 0;
    for (uint8_t i = 0; i < persisted.movedSquaresCount; ++i) {
        const uint8_t sq = persisted.movedSquares[i];
        if (sq < 64) {
            movedMask |= (1ULL << sq);
        }
    }
    m_stateMachine.SetMovedSquaresMask(movedMask);

    const bool loadedInitialFen = modules::IsAnyInitialFen(m_cachedFen.CStr());
    const bool loadedFreshStart = (movedMask == 0ULL) &&
                                  (persisted.undoHistoryCount == 0) &&
                                  !persisted.cpuMoveValid &&
                                  persisted.interactionState.Empty();

    if (!RestoreDuckGameState(
            persisted, loadedInitialFen && loadedFreshStart)) {
        DispatchInteractionAdapterResult(
            runtime::AppEventType::RestoreResult,
            "restore",
            false);
        return false;
    }
    if (!RestoreDuckUndoState(persisted)) {
        DispatchInteractionAdapterResult(
            runtime::AppEventType::RestoreResult,
            "restore",
            false);
        return false;
    }

    // Restore undo base.
    if (!persisted.undoBaseFen.Empty()) {
        m_undoBaseFen = persisted.undoBaseFen;
    } else if (m_undoBaseFen.Empty()) {
        // Fallback: if no persisted base exists and we didn't seed one above,
        // treat the current engine state as the undo base.
        m_undoBaseFen = m_cachedFen;
    }

    uint64_t baseMovedMask = 0;
    for (uint8_t i = 0; i < persisted.undoBaseMovedSquaresCount; ++i) {
        const uint8_t sq = persisted.undoBaseMovedSquares[i];
        if (sq < 64) {
            baseMovedMask |= (1ULL << sq);
        }
    }
    m_undoBaseMovedSquaresMask = baseMovedMask;

    // Derive render-facing last-move pulses from restored undo history so the
    // indicator survives reboot.
    UpdateLastMovePulseFromUndoHistory();

    ApplyEngineFenToStateMachineExpected(
        m_pendingDuckTurn.valid
            ? m_pendingDuckTurn.afterFen.CStr()
            : m_cachedFen.CStr());

    bool restoredInteraction = false;
    if (!persisted.interactionState.Empty()) {
        runtime::ChessInteractionDurableState durable;
        restoredInteraction =
            runtime::DecodeChessInteractionDurableState(
                persisted.interactionState.CStr(), durable) &&
            m_stateMachine.RestoreInteractionDurableState(durable);
        if (!restoredInteraction) {
            DispatchInteractionAdapterResult(
                runtime::AppEventType::RestoreResult,
                "restore",
                false);
            return false;
        }
    }
    const bool restoredDuckRelocation =
        restoredInteraction &&
        m_stateMachine.IsDuckRelocationActive();
    if (m_pendingDuckTurn.valid != restoredDuckRelocation) {
        DispatchInteractionAdapterResult(
            runtime::AppEventType::RestoreResult,
            "restore",
            false);
        return false;
    }

    if (restoredDuckRelocation) {
        const auto& plan = m_stateMachine.GetChessInteractionModel().phase.plan;
        m_pendingDuckTurn.source = plan.source;
        m_pendingDuckTurn.directedOrigin = plan.directedOrigin;
        m_pendingDuckTurn.requiredDuckDestination =
            runtime::IsCpuDirectedTransition(plan)
                ? plan.moveTo : runtime::CHESS_INTERACTION_INVALID_SQUARE;
    }

    if (persisted.cpuMoveValid) {
        UciMove restoredUci;
        bool cpuPlanReady = MakeUciMove(
            persisted.cpuMoveFrom, persisted.cpuMoveTo,
            persisted.cpuMovePromotionSuffixLower, restoredUci);
        bool capturedKing = false;
        if (cpuPlanReady && IsDuckGame()) {
            cpuPlanReady = ValidateDuckCpuPayload(
                m_cachedFen.CStr(), m_duckSquare, restoredUci,
                persisted, capturedKing);
        }
        if (cpuPlanReady) {
            cpuPlanReady = m_stateMachine.QueueCpuMove(
                persisted.cpuMoveFrom,
                persisted.cpuMoveTo,
                persisted.cpuMoveCaptureSquareValid,
                persisted.cpuMoveCaptureSquare,
                persisted.cpuMovePromotionSuffixLower);
        }
        if (!cpuPlanReady) {
            DispatchInteractionAdapterResult(
                runtime::AppEventType::RestoreResult,
                "restore", false);
            return false;
        }

        m_pendingCpuMoveUci = restoredUci;
        m_pendingCpuMoveUciValid = true;
        m_pendingCpuMoveFrom = persisted.cpuMoveFrom;
        m_pendingCpuMoveTo = persisted.cpuMoveTo;
        m_pendingCpuMoveCaptureSquareValid =
            persisted.cpuMoveCaptureSquareValid;
        m_pendingCpuMoveCaptureSquare =
            persisted.cpuMoveCaptureSquareValid
                ? persisted.cpuMoveCaptureSquare
                : 0U;
        if (IsDuckGame()) {
            m_pendingCpuDuckDestinationValid = !capturedKing;
            m_pendingCpuDuckDestination = !capturedKing
                                              ? persisted.cpuMoveDuckDestination
                                              : INVALID_DUCK_SQUARE;
        }
    }

    m_lastCastlingRookPromptActive = m_stateMachine.IsCastlingRookPromptActive();

    // Fresh boot at an untouched initial position keeps the identity assist
    // available even when every Hall sensor already reports occupied.
    if (loadedInitialFen && loadedFreshStart) {
        m_stateMachine.TriggerInitialKingPlacementPrompt();
    }

    m_engineDerived.RefreshFromEngine(m_engine);

    // A present clock record is strict authority except for a full-time record
    // from a provably untouched initial setup whose TIMER setting changed.
    // A legacy missing record may use the configured preset, but remains
    // paused until fresh boot evidence.
    if (!RestoreGameClockFromPersistence(
            persisted,
            persisted.fen.Empty(),
            loadedInitialFen && loadedFreshStart)) {
        DispatchInteractionAdapterResult(
            runtime::AppEventType::RestoreResult,
            "restore",
            false);
        return false;
    }

    // The firmware's physical boot scan runs after persistence restore. Let
    // the ordinary action consumer restart a restored promotion modal only
    // after that scan confirms the completed physical plan.
    m_promotionPending = false;
    const auto& restoredPhase = m_stateMachine.GetChessInteractionModel().phase;
    m_restoredPromotionRequiresChoice =
        restoredPhase.plan.kind == runtime::PhysicalTransitionKind::Promotion &&
        restoredPhase.plan.source == runtime::PhysicalTransitionSource::Human;

    if (!RebuildFenHistoryFromUndoState()) {
        if (IsDuckGame() && m_undoHistoryFieldCount != 0U) {
            DispatchInteractionAdapterResult(
                runtime::AppEventType::RestoreResult,
                "restore",
                false);
            return false;
        }
        m_halfmoveClockPlies = ComputeHalfmoveClockPliesFromUndoHistory();
        SeedFenHistoryFromCurrentFen();
    }

    RestoreGameHistoryIdentity(persisted);
    const bool migratedHistoryIdentity = persisted.gameHistory.valid &&
        !persisted.gameHistory.present && m_gameHistoryPersistence.present;
    RefreshDrawStateFromTracking();

    // Treat initialization as the baseline for kq-swap prompt toggle detection.
    // Tests and firmware may reuse a coordinator instance across resets.
    m_kqPromptInitialized = false;
    m_kqPromptBaselineValid = false;
    m_kqPromptGeneration = 0;
    m_kqPromptLastStartedGeneration = 0;

    // Also treat initialization as the baseline for first-player toggle detection.
    // Prevent a spurious first-player change from being detected on the first
    // Update() after settings are reloaded.
    m_firstPlayerInitialized = false;

    // Hint pipeline: ensure we treat the first Update() after boot/load as a
    // "new turn" so the initial human-turn hint can be scheduled.
    m_hintLastSideToMoveValid = false;
    m_hintPendingStart = false;
    DispatchInteractionAdapterResult(
        runtime::AppEventType::RestoreResult,
        "restore",
        true);
    outPersistSnapshot = initializedNewGame || migratedHistoryIdentity;
    outClearUndoForSnapshot = initializedNewGame;
    return true;
}

} // namespace modules
