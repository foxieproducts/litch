#include "modules/game_persistence_coordinator.hpp"

#include <cstring>

#include "logger.hpp"
#include "modules/game_persistence_engine_helpers.hpp"
#include "runtime/chess_interaction_persistence.hpp"

#if defined(LITCH_ESP32S3)
// GCC sets the numeric threshold for the whole file. Enforce it only on the
// small timeout boundary below, not on the separate bounded snapshot body.
#pragma GCC diagnostic ignored "-Wframe-larger-than=128"
#endif

namespace modules {
namespace {

constexpr uint32_t DEFAULT_RNG_SEED = 1U;

class SnapshotScratchLease final {
  public:
    explicit SnapshotScratchLease(std::atomic<bool>& busy)
        : m_busy(busy), m_acquired(!busy.exchange(true, std::memory_order_acquire)) {}
    ~SnapshotScratchLease() {
        if (m_acquired) m_busy.store(false, std::memory_order_release);
    }
    explicit operator bool() const { return m_acquired; }
  private:
    std::atomic<bool>& m_busy;
    bool m_acquired;
};

uint32_t NormalizeRngSeed(const uint32_t seed) {
    return (seed == 0U) ? DEFAULT_RNG_SEED : seed;
}

bool SafeGetFen(IEngine& engine, FenString& outFen) {
    return game_persistence_engine_helpers::SafeGetFen(
        engine, outFen, true);
}

} // namespace

bool GamePersistenceCoordinator::LoadSettingsForRecovery() {
    SnapshotScratchLease lease(m_snapshotScratchBusy);
    if (!lease) return false;
    const bool loaded = m_settings.Load(&m_snapshotDocument);
    m_snapshotDocument.clear();
    return loaded;
}

void GamePersistenceCoordinator::SetRngSeed(const uint32_t rngSeed) {
    m_vsCpuRngSeed = NormalizeRngSeed(rngSeed);
}

bool GamePersistenceCoordinator::TryGetLastHintThinkStats(UciMove& outBestUci,
                                                          uint32_t& outElapsedMs,
                                                          uint32_t& outNodeCount,
                                                          uint8_t& outDifficulty) const {
    if (!m_lastHintThinkStatsValid) {
        outBestUci.Clear();
        outElapsedMs = 0;
        outNodeCount = 0;
        outDifficulty = 0;
        return false;
    }

    outBestUci = m_lastHintThinkBestUci;
    outElapsedMs = m_lastHintThinkElapsedMs;
    outNodeCount = m_lastHintThinkNodeCount;
    outDifficulty = m_lastHintThinkDifficulty;
    return true;
}

bool GamePersistenceCoordinator::TryGetThinkDiagnostics(
    const ThinkRequestKind kind, ThinkDiagnostics& out) const {
    if (kind != ThinkRequestKind::Cpu && kind != ThinkRequestKind::Hint) {
        out = ThinkDiagnostics{};
        return false;
    }
    out = kind == ThinkRequestKind::Cpu ? m_cpuDiagnostics : m_hintDiagnostics;
    if (kind == ThinkRequestKind::Unavailable || out.requestId == 0U ||
        !m_diagnosticFen.Equals(m_cachedFen.View()) ||
        m_diagnosticVariant != m_gameVariant || out.isDuck != IsDuckGame() ||
        (out.isDuck && out.duckSquare != m_duckSquare)) {
        out = ThinkDiagnostics{};
        return false;
    }
    // A prior kind's confirmed quiescent result is immutable history, not
    // a substitute for current activity. It survives the other kind's work.
    if (out.requestId != m_diagnosticRequestId && out.quiescentValid && !out.active) return true;
    ThinkDiagnostics live;
    if (!ThinkWorker().TryGetDiagnostics(live) || live.requestId != out.requestId || live.kind != kind) {
        out = ThinkDiagnostics{};
        return false;
    }
    live.firstAcceptedHintValid = out.firstAcceptedHintValid;
    live.firstAcceptedHintAtMs = out.firstAcceptedHintAtMs;
    live.retired = out.retired;
    live.configuredCpuLevel = out.configuredCpuLevel;
    out = live;
    return true;
}

void GamePersistenceCoordinator::RefreshThinkDiagnostics() {
    ThinkDiagnostics live;
    if (!ThinkWorker().TryGetDiagnostics(live) || live.requestId != m_diagnosticRequestId) return;
    if (live.kind == ThinkRequestKind::Benchmark) {
        m_benchmarkStatus.diagnostics = live;
        return;
    }
    if (live.kind != ThinkRequestKind::Cpu && live.kind != ThinkRequestKind::Hint) return;
    auto& cache = live.kind == ThinkRequestKind::Cpu ? m_cpuDiagnostics : m_hintDiagnostics;
    live.firstAcceptedHintValid = cache.firstAcceptedHintValid;
    live.firstAcceptedHintAtMs = cache.firstAcceptedHintAtMs;
    live.retired = cache.retired;
    live.configuredCpuLevel = cache.configuredCpuLevel;
    cache = live;
}

void GamePersistenceCoordinator::BeginThinkDiagnostics(
    const ThinkConfig& config, const ThinkRequestKind kind) {
    RefreshThinkDiagnostics();
    if (!m_diagnosticFen.Equals(m_cachedFen.View()) || m_diagnosticVariant != m_gameVariant) {
        m_diagnosticVariant = m_gameVariant;
        m_cpuDiagnostics = ThinkDiagnostics{};
        m_hintDiagnostics = ThinkDiagnostics{};
        m_diagnosticFen = m_cachedFen;
    }
    m_diagnosticRequestId = m_diagnosticRequestId == UINT32_MAX ? 1U : m_diagnosticRequestId + 1U;
    ThinkWorker().PrepareDiagnostics(m_diagnosticRequestId, kind, config,
        m_engineDerived.sideToMove, IsDuckGame(), IsDuckGame() ? m_duckSquare : INVALID_DUCK_SQUARE);
    auto& cache = kind == ThinkRequestKind::Cpu ? m_cpuDiagnostics : m_hintDiagnostics;
    cache = ThinkDiagnostics{};
    cache.configuredCpuLevel = kind == ThinkRequestKind::Cpu ? static_cast<uint8_t>(ReadVsCpuLevelSetting()) : 0U;
    RefreshThinkDiagnostics();
}

void GamePersistenceCoordinator::RetireThinkDiagnostics(const ThinkRequestKind kind) {
    RefreshThinkDiagnostics();
    auto& cache = kind == ThinkRequestKind::Cpu ? m_cpuDiagnostics : m_hintDiagnostics;
    if (cache.requestId != 0U) cache.retired = true;
}

void GamePersistenceCoordinator::RecordAcceptedHintDiagnostic() {
    RefreshThinkDiagnostics();
    if (m_hintDiagnostics.requestId == 0U || m_hintDiagnostics.retired ||
        !m_diagnosticFen.Equals(m_cachedFen.View()) ||
        m_hintDiagnostics.firstAcceptedHintValid) return;
    m_hintDiagnostics.firstAcceptedHintValid = true;
    m_hintDiagnostics.firstAcceptedHintAtMs = fxe::Millis();
}

uint32_t GamePersistenceCoordinator::NextClockIdentity(
    const uint32_t current) {
    return current == UINT32_MAX ? 1U : current + 1U;
}

GameClockPersistenceState
GamePersistenceCoordinator::CaptureGameClockPersistenceState(
    const bool newSessionBoundary,
    const runtime::GameClockState& clockState) {
    const uint32_t timerSetting = GetClockTimerSetting();
    const runtime::TimerPreset preset =
        runtime::TimerPresetForTimerSetting(timerSetting);

    GameClockPersistenceState state;
    state.sessionId = newSessionBoundary
        ? NextClockIdentity(m_clockSessionId)
        : (m_clockSessionId == 0U ? 1U : m_clockSessionId);
    state.durableEpoch = NextClockIdentity(m_clockDurableEpoch);
    state.timerSetting = timerSetting;
    state.baseMs = preset.baseSeconds * 1000U;
    state.started = m_clockSessionStarted;
    state.clock = clockState;
    state.terminalDuckChessMove = m_clockTerminalDuckChessMove;
    return state;
}

#if defined(LITCH_ESP32S3)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wframe-larger-than=128"
#endif
[[gnu::noinline]] bool GamePersistenceCoordinator::PersistSnapshot(
    const bool clearUndo, const bool scheduleSave) {
    // Capture the clock exactly once at the semantic boundary. CaptureState()
    // performs current-time accounting and can be the operation that reaches
    // zero, so promote that result before capturing any dependent game state.
    const runtime::GameClockState clockState = m_gameClock.CaptureState();
    (void)ConsumePendingClockTimeout();
    if (m_terminalAbandonPending) {
        return false;
    }
    return PersistSnapshotState(clockState, clearUndo, scheduleSave);
}
#if defined(LITCH_ESP32S3)
#pragma GCC diagnostic pop
#endif

// Timeout cleanup above may acknowledge a position and render its guidance.
// Reserve the large game/history snapshot only after that call chain unwinds.
[[gnu::noinline]] bool GamePersistenceCoordinator::PersistSnapshotState(
    const runtime::GameClockState& clockState,
    const bool clearUndo, const bool scheduleSave) {
    SnapshotScratchLease lease(m_snapshotScratchBusy);
    if (!lease) return false;
    const bool clockTerminal = HasClockTerminalOutcome() || HasResignationOutcome();

    if (m_duckUndoRelocationPending || m_duckRedoRelocationPending) {
        logger::Trace(
            "persist: snapshot deferred (duck undo relocation active)");
        return false;
    }

    auto& doc = m_settings.Doc();

    if (!clockTerminal && !clearUndo && m_stateMachine.IsUndoPromptActive()) {
        logger::Trace("persist: snapshot deferred (undo prompt active)");
        return false;
    }
    if (!clockTerminal && !runtime::CanSafelyPersistChessInteractionState(
            m_stateMachine.GetChessInteractionModel())) {
        logger::Trace("persist: snapshot deferred (interaction action in flight)");
        return false;
    }

    GamePersistenceState s;
    s.fen = m_cachedFen;
    s.hasAcceptedMove = m_gameHasAcceptedMove;

    const uint64_t movedMask = m_stateMachine.GetMovedSquaresMask();
    s.movedSquaresCount =
        MaskToSquareListAscending(movedMask, s.movedSquares, sizeof(s.movedSquares));

    s.undoBaseFen = m_undoBaseFen;
    s.undoBaseMovedSquaresCount = MaskToSquareListAscending(m_undoBaseMovedSquaresMask,
                                                            s.undoBaseMovedSquares,
                                                            sizeof(s.undoBaseMovedSquares));

    if (clearUndo) {
        s.undoHistoryCount = 0;
    } else {
        // Persisting the full in-RAM undo history can exceed the SettingsStore
        // JSON capacity (which also holds many non-game settings). When this
        // happens we would previously fail persistence entirely, leaving a stale
        // snapshot that then replays incorrectly after reboot.
        //
        // Persist only a bounded tail window of moves, and persist a matching
        // base FEN from our recent FEN ring so replay is consistent.
        static constexpr uint8_t PERSISTED_UNDO_MAX_MOVES = 128;
        static constexpr uint16_t STRIDE = UNDO_HISTORY_FIELDS_PER_MOVE;

        const uint16_t totalFields = m_undoHistoryFieldCount;
        const uint16_t totalMoves = static_cast<uint16_t>(totalFields / STRIDE);
        const uint16_t keepMoves =
            (totalMoves > static_cast<uint16_t>(PERSISTED_UNDO_MAX_MOVES))
                ? static_cast<uint16_t>(PERSISTED_UNDO_MAX_MOVES)
                : totalMoves;
        const uint16_t keepFields = static_cast<uint16_t>(keepMoves * STRIDE);

        if (keepFields > static_cast<uint16_t>(sizeof(s.undoHistory))) {
            return false;
        }

        if (keepFields != 0U) {
            const uint16_t start = static_cast<uint16_t>(totalFields - keepFields);
            std::memcpy(s.undoHistory, &m_undoHistoryFields[start], keepFields);
            s.undoHistoryCount = keepFields;

            // Base FEN should be the position immediately before the oldest
            // persisted move. Fen history stores ply-boundary engine FENs; the
            // current position is the most recent entry.
            if (m_fenHistorySize >= static_cast<uint8_t>(keepMoves + 1U)) {
                const uint8_t idxFromStart = static_cast<uint8_t>(m_fenHistorySize - (keepMoves + 1U));
                const uint8_t ringIdx =
                    static_cast<uint8_t>((m_fenHistoryStart + idxFromStart) % FEN_HISTORY_CAPACITY);
                FenString ringBase;
                if (ReadFenHistory(ringIdx, ringBase)) {
                    CopyFenWithCommittedFullmove(ringBase, totalMoves - keepMoves, s.undoBaseFen);
                    // Base-moved metadata is only used for LED overlays; when
                    // we change the base, discard the metadata rather than
                    // risk mismatched overlays after reboot.
                    s.undoBaseMovedSquaresCount = 0;
                }
            }
        } else {
            s.undoHistoryCount = 0;
            // No history: ensure base is self-consistent with the snapshot.
            CopyFenWithCommittedFullmove(s.fen, 0U, s.undoBaseFen);
            s.undoBaseMovedSquaresCount = 0;
        }
    }

    {
        uint8_t fromSquare = 0;
        uint8_t toSquare = 0;
        bool captureSquareValid = false;
        uint8_t captureSquare = 0;
        if (!clockTerminal && m_stateMachine.TryGetQueuedCpuMove(fromSquare, toSquare, captureSquareValid, captureSquare)) {
            s.cpuMoveValid = true;
            s.cpuMoveFrom = fromSquare;
            s.cpuMoveTo = toSquare;
            s.cpuMoveCaptureSquareValid = captureSquareValid;
            s.cpuMoveCaptureSquare = captureSquare;

            char promoSuffix = '\0';
            if (m_stateMachine.TryGetQueuedCpuPromotionSuffix(promoSuffix)) {
                s.cpuMovePromotionSuffixLower = promoSuffix;
            }
        } else if (!clockTerminal && m_pendingCpuMoveUciValid) {
            s.cpuMoveValid = true;
            s.cpuMoveFrom = m_pendingCpuMoveFrom;
            s.cpuMoveTo = m_pendingCpuMoveTo;
            s.cpuMoveCaptureSquareValid = m_pendingCpuMoveCaptureSquareValid;
            s.cpuMoveCaptureSquare = m_pendingCpuMoveCaptureSquareValid ? m_pendingCpuMoveCaptureSquare : 0U;

            const char* pendingUci = m_pendingCpuMoveUci.CStr();
            if (pendingUci != nullptr && pendingUci[0] != '\0' && pendingUci[1] != '\0' &&
                pendingUci[2] != '\0' && pendingUci[3] != '\0' && pendingUci[4] != '\0') {
                s.cpuMovePromotionSuffixLower = pendingUci[4];
            }
        }
        if (s.cpuMoveValid && IsDuckGame() &&
            m_pendingCpuDuckDestinationValid) {
            s.cpuMoveDuckDestinationValid = true;
            s.cpuMoveDuckDestination = m_pendingCpuDuckDestination;
        }
    }

    if (!clockTerminal && runtime::HasDurableChessInteractionState(
            m_stateMachine.GetChessInteractionModel())) {
        char encoded[128] = {};
        if (!runtime::EncodeChessInteractionDurableState(
                m_stateMachine.GetChessInteractionModel(),
                encoded,
                sizeof(encoded)) ||
            !s.interactionState.Assign(encoded)) {
            return false;
        }
    }

    if (!EncodeDuckGameState(s)) {
        return false;
    }
    const uint16_t persistedDuckMoves = clearUndo
        ? 0U
        : static_cast<uint16_t>(
              s.undoHistoryCount / UNDO_HISTORY_FIELDS_PER_MOVE);
    const uint16_t duckHistoryStart =
        m_duckUndoMoveCount > persistedDuckMoves
            ? static_cast<uint16_t>(
                  m_duckUndoMoveCount - persistedDuckMoves)
            : 0U;
    if (!EncodeDuckUndoState(
            s, duckHistoryStart, persistedDuckMoves)) {
        return false;
    }

    s.clockStatePresent = true;
    s.clockState =
        CaptureGameClockPersistenceState(clearUndo, clockState);
    CaptureGameHistoryPersistenceState(s);

    // Prepare in reused RAM before publishing: failed encoding must preserve
    // newest preferences and the prior coherent game/undo record. Copying only
    // retained keys also reclaims ArduinoJson's dead monotonic pool allocations.
    if (!PrepareGamePersistenceDocument(m_snapshotDocument, doc, s)) {
        logger::Warn(
            "persist: snapshot preparation failed: overflow=%u candidate_mem=%u/%u json_bytes=%u clearUndo=%u",
            m_snapshotDocument.overflowed() ? 1U : 0U,
            static_cast<unsigned>(m_snapshotDocument.memoryUsage()),
            static_cast<unsigned>(m_snapshotDocument.capacity()),
            static_cast<unsigned>(measureJson(m_snapshotDocument)),
            clearUndo ? 1U : 0U);
        return false;
    }

    // Both documents use SettingsStore's capacity. The candidate owns copied
    // retained strings, so committing cannot refer into the replaced document.
    (void)doc.set(m_snapshotDocument);

    m_clockSessionId = s.clockState.sessionId;
    m_clockDurableEpoch = s.clockState.durableEpoch;
    if (scheduleSave) m_settings.MarkMustSaveDirty();
    else m_settings.NotifyDocChanged();
    return true;
}

bool GamePersistenceCoordinator::PersistSnapshotDeferredWithRetries(
    const char* reason,
    const bool clearUndo) {
    static constexpr uint8_t MAX_ATTEMPTS = 3;
    for (uint8_t attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        if (PersistSnapshot(clearUndo)) {
            ClearSnapshotRetry();
            DispatchInteractionAdapterResult(
                runtime::AppEventType::PersistenceResult,
                "persist",
                true);
            return true;
        }

        if (m_settings.IsDisabled()) {
            logger::Warn("persist: snapshot preparation skipped (settings disabled) reason=%s",
                         (reason != nullptr) ? reason : "?");
            ClearSnapshotRetry();
            DispatchInteractionAdapterResult(
                runtime::AppEventType::PersistenceResult,
                "persist",
                false);
            return false;
        }

        const modules::SettingsStoreError err = m_settings.GetLastError();
        if (err == modules::SettingsStoreError::None) {
            const auto& doc = m_settings.Doc();
            logger::Warn(
                "persist: snapshot preparation failed: attempt=%u/%u err=%s reason=%s doc_overflow=%u doc_mem=%u/%u json_bytes=%u",
                static_cast<unsigned>(attempt + 1U),
                static_cast<unsigned>(MAX_ATTEMPTS),
                modules::SettingsStoreErrorToString(err),
                (reason != nullptr) ? reason : "?",
                doc.overflowed() ? 1U : 0U,
                static_cast<unsigned>(doc.memoryUsage()),
                static_cast<unsigned>(doc.capacity()),
                static_cast<unsigned>(measureJson(doc)));
        } else {
            logger::Warn("persist: snapshot preparation failed: attempt=%u/%u err=%s reason=%s",
                         static_cast<unsigned>(attempt + 1U),
                         static_cast<unsigned>(MAX_ATTEMPTS),
                         modules::SettingsStoreErrorToString(err),
                         (reason != nullptr) ? reason : "?");
        }
    }

    ScheduleSnapshotRetry(reason, clearUndo);
    DispatchInteractionAdapterResult(
        runtime::AppEventType::PersistenceResult,
        "persist",
        false);
    return false;
}

void GamePersistenceCoordinator::ScheduleSnapshotRetry(
    const char* reason,
    const bool clearUndo) {
    m_snapshotRetryPending = true;
    m_snapshotRetryClearUndo = clearUndo;
    if (m_snapshotRetryFailures != UINT8_MAX) {
        ++m_snapshotRetryFailures;
    }
    m_snapshotRetryTime.Reset();
    (void)m_snapshotRetryReason.Assign((reason != nullptr) ? reason : "?");
}

void GamePersistenceCoordinator::ClearSnapshotRetry() {
    m_snapshotRetryPending = false;
    m_snapshotRetryClearUndo = false;
    m_snapshotRetryFailures = 0;
    m_snapshotRetryTime.Reset();
    m_snapshotRetryReason.Clear();
}

} // namespace modules
