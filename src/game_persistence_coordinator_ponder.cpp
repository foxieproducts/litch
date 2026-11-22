#include "modules/game_persistence_coordinator.hpp"

#include <litch_max/chess_notation.hpp>
#include "logger.hpp"
#include "modules/draw_rules.hpp"
#include "modules/game_persistence_coordinator_update_internal.hpp"
#include "modules/game_persistence_coordinator_vs_cpu_internal.hpp"
#include "modules/initial_fen.hpp"
#include "modules/vs_cpu_difficulty.hpp"
#include "modules/vs_cpu_rng.hpp"
#include "modules/vs_cpu_turn.hpp"

namespace modules {
namespace {

bool SameProfile(const ThinkConfig& a, const ThinkConfig& b) {
    if (a.difficulty != b.difficulty || a.maxThinkMs != b.maxThinkMs ||
        a.maxNodes != b.maxNodes || a.maxDepth != b.maxDepth || a.rngSeed != b.rngSeed ||
        a.firstRootNodeReserve != b.firstRootNodeReserve || a.reversibleHalfmoves != b.reversibleHalfmoves ||
        a.openingPrefix.fromCanonicalStart != b.openingPrefix.fromCanonicalStart ||
        a.openingPrefix.moveCount != b.openingPrefix.moveCount) return false;
    for (uint8_t i = 0U; i < a.openingPrefix.moveCount; ++i) {
        const auto& x = a.openingPrefix.moves[i];
        const auto& y = b.openingPrefix.moves[i];
        if (x.fromSquare != y.fromSquare || x.toSquare != y.toSquare || x.promotion != y.promotion) return false;
    }
    return true;
}

bool Qualified(const ThinkCandidate& candidate) {
    return candidate.IsAvailable() && candidate.source == ThinkCandidateSource::Search &&
        candidate.scoreValid && !candidate.isDuck &&
        (candidate.rootKind == ThinkRootKind::ClassicFull ||
         candidate.rootKind == ThinkRootKind::ClassicPreliminary);
}

} // namespace

CpuPonderStatus GamePersistenceCoordinator::GetPonderStatus() const {
    auto result = m_ponderStatus;
    result.enabled = ReadVsCpuLevelSetting() == 11U && m_gameVariant == GameVariant::Classic;
    result.active = m_ponderThinking && ThinkWorker().IsActive();
    result.ready = m_ponderRootValid && m_ponderCompleted && Qualified(m_ponderFloor);
    if (m_ponderThinking) {
        ThinkDiagnostics diagnostic;
        if (ThinkWorker().TryGetDiagnostics(diagnostic) && diagnostic.requestId == m_ponderRequestId) {
            const auto& candidate = diagnostic.latestPublication;
            if (Qualified(candidate)) {
                result.completedDepth = candidate.completedDepth;
                result.fullRoot = candidate.rootKind == ThinkRootKind::ClassicFull;
                result.completedRootNodes = candidate.completedNodes;
                (void)litch_max::TryFormatUciMove(candidate.fromSquare, candidate.toSquare,
                                                candidate.promotion, result.reply);
            }
        }
        result.elapsedMs = m_ponderElapsed.ElapsedMs();
    }
    return result;
}

void GamePersistenceCoordinator::CollectPonderResult() {
    if (!m_ponderThinking) return;
    ThinkDiagnostics diagnostic;
    if (ThinkWorker().TryGetDiagnostics(diagnostic) && diagnostic.requestId == m_ponderRequestId &&
        Qualified(diagnostic.latestPublication)) {
        m_ponderFloor = diagnostic.latestPublication;
        m_ponderStatus.completedDepth = m_ponderFloor.completedDepth;
        m_ponderStatus.fullRoot = m_ponderFloor.rootKind == ThinkRootKind::ClassicFull;
        m_ponderStatus.completedRootNodes = m_ponderFloor.completedNodes;
        (void)litch_max::TryFormatUciMove(m_ponderFloor.fromSquare, m_ponderFloor.toSquare,
                                       m_ponderFloor.promotion, m_ponderStatus.reply);
    }
    m_ponderStatus.elapsedMs = m_ponderElapsed.ElapsedMs();
    if (!ThinkWorker().IsActive() && ThinkWorker().IsDone()) {
        uint8_t progress = 0U;
        uint32_t elapsed = 0U, nodes = 0U;
        UciMove result;
        const auto state = ThinkWorker().GetResult(progress, result, elapsed, nodes);
        m_ponderStatus.elapsedMs = elapsed;
        ThinkDiagnostics terminal;
        m_ponderStatus.nodesValid = ThinkWorker().TryGetDiagnostics(terminal) &&
            terminal.requestId == m_ponderRequestId && terminal.finalNodesValid;
        m_ponderStatus.nodes = m_ponderStatus.nodesValid ? nodes : 0U;
        if (!m_ponderCancelRequested && state == ThinkState::Done && Qualified(m_ponderFloor)) {
            m_ponderCompleted = true;
            m_ponderStatus.reply = result;
        }
    }
}

void GamePersistenceCoordinator::CancelPonder(const bool discard) {
    CollectPonderResult();
    if (m_ponderThinking && !m_ponderCancelRequested) {
        ++m_ponderStatus.cancels;
        m_ponderCancelRequested = true;
        m_engine.CancelThink();
        logger::Trace("ponder: cancel prediction=%s retained_depth=%u discard=%u",
            m_ponderStatus.predictedHumanMove.CStr(), unsigned(m_ponderFloor.completedDepth), unsigned(discard));
    }
    if (discard) {
        m_ponderRootValid = false;
        m_ponderCompleted = false;
        m_ponderFloor = {};
    }
}

bool GamePersistenceCoordinator::UpdatePonder(const uint32_t level, const bool firstPlayerIsP2,
                                              const bool cpuPlayerIsP2) {
    const bool humanTurn = !IsVsCpuTurn(m_engineDerived.sideToMove, level, firstPlayerIsP2, cpuPlayerIsP2);
    const bool contextOk = level == 11U && m_gameVariant == GameVariant::Classic &&
        !IsGameOverVariantAware() && !HasClockTerminalOutcome() && !HasResignationOutcome() &&
        !m_boardSettingsActive && !m_promotionPending && !m_litchcraft.IsActive() &&
        !m_coachingHintActive && !m_stateMachine.IsHintDisplayRequested();
    if (m_ponderThinking) {
        CollectPonderResult();
        if (!contextOk || m_cancelCpuWorkRequested || m_cancelHintWorkRequested || m_cancelAllThinkingRequested) {
            CancelPonder(true);
        } else if (!humanTurn || !IsHintSearchStableIdle() ||
                   m_undoHistoryRevision != m_ponderHistoryRevision ||
                   !m_cachedFen.Equals(m_ponderStatus.sourceFen.View())) {
            // Human handling gets priority. Retain completed evidence for an
            // exact subsequent commit, never wait for more speculative work.
            CancelPonder(false);
        }
        if (ThinkWorker().IsActive()) return true;
        CollectPonderResult();
        (void)ThinkWorker().CancelAndJoin(m_engine);
        m_ponderThinking = false;
        m_ponderStatus.active = false;
        return false;
    }
    if (!contextOk) {
        CancelPonder(true);
        return false;
    }
    // Existing Hint/Coaching owns all of its continuous-analysis window.
    // No new prediction while a visible manual/Coaching request is active.
    if (!humanTurn || m_ponderRootValid || m_hintThinking || m_vsCpuThinking ||
        m_benchmarkActive || ThinkWorker().IsActive() || !IsHintSearchStableIdle() ||
        m_coachingHintActive || m_stateMachine.IsHintDisplayRequested() ||
        !m_lastHintThinkStatsValid || m_hintCompletedDifficulty == 0U ||
        !IsCurrentHintAnalysisPosition() || !HasCachedHintForCurrentPosition() ||
        (m_ponderAttemptFen.Equals(m_cachedFen.View()) && m_ponderAttemptRevision == m_undoHistoryRevision)) return false;

    litch_max::ParsedUciMove predicted;
    if (!litch_max::TryParseUciMove(m_lastHintThinkBestUci.CStr(), predicted)) return false;
    bool capture = false;
    uint8_t captureSquare = predicted.toSquare;
    if (!game_persistence_update_internal::TryComputeCpuCaptureSquareAndIsCapture(
            m_cachedFen.CStr(), predicted.fromSquare, predicted.toSquare, captureSquare, capture)) return false;
    m_ponderHalfmoveClock = NextHalfmoveClockPlies(m_cachedFen.CStr(), m_halfmoveClockPlies,
        predicted.fromSquare, predicted.toSquare, capture, captureSquare);
    const bool completeHistory = m_gameHistoryPersistence.valid && m_gameHistoryPersistence.present &&
        !m_gameHistoryPersistence.captureIncomplete && m_undoBaseFen.Equals(INITIAL_FEN);
    const uint32_t nextTurns = completeHistory ? (GetUndoHistoryMoveCount() + 1U) / 2U : 2U;
    m_ponderConfig = MakeThinkConfigForVsCpuTurn(level, nextTurns,
        NextVsCpuSeedAfterMove(m_vsCpuRngSeed, m_lastHintThinkBestUci.CStr()), CpuSearchVariant::Classic);
    FillChessKnowledgeContext(m_ponderConfig);
    m_ponderConfig.reversibleHalfmoves = m_ponderConfig.reversibleHalfmoves == UINT16_MAX &&
        m_ponderHalfmoveClock != 0U ? UINT16_MAX : m_ponderHalfmoveClock;
    auto& prefix = m_ponderConfig.openingPrefix;
    if (prefix.fromCanonicalStart && !predicted.hasPromotion && prefix.moveCount < THINK_OPENING_MAX_PLIES) {
        prefix.moves[prefix.moveCount++] = {predicted.fromSquare, predicted.toSquare, '\0'};
    } else prefix = {};
    if (m_ponderConfig.maxThinkMs == 0U || m_ponderConfig.maxThinkMs > 60000U) return false;
    (void)ThinkWorker().CancelAndJoin(m_engine);
    m_diagnosticRequestId = m_diagnosticRequestId == UINT32_MAX ? 1U : m_diagnosticRequestId + 1U;
    m_ponderRequestId = m_diagnosticRequestId;
    ThinkWorker().PrepareDiagnostics(m_ponderRequestId, ThinkRequestKind::Ponder, m_ponderConfig,
        m_engineDerived.sideToMove == SideToMove::White ? SideToMove::Black : SideToMove::White, false, 255U);
    m_ponderConfig.candidateObserver = ThinkWorker().PrepareCandidateObserver();
    m_ponderConfig.progressObserver = ThinkWorker().PrepareProgressObserver();
    m_ponderAttemptFen = m_cachedFen;
    m_ponderAttemptRevision = m_undoHistoryRevision;
    if (!m_engine.StartPonder(m_lastHintThinkBestUci.CStr(), m_ponderConfig)) return false;
    m_ponderHistoryRevision = m_undoHistoryRevision;
    m_ponderHistoryFieldCount = m_undoHistoryFieldCount;
    m_ponderGameId = m_gameHistoryPersistence.gameId;
    m_ponderFloor = {};
    m_ponderCompleted = false;
    m_ponderCancelRequested = false;
    m_ponderRootValid = true;
    m_ponderThinking = true;
    m_ponderStatus.predictedHumanMove = m_lastHintThinkBestUci;
    m_ponderStatus.sourceFen = m_cachedFen;
    m_ponderStatus.reply.Clear();
    m_ponderStatus.completedDepth = 0U;
    m_ponderStatus.nodes = 0U;
    m_ponderStatus.nodesValid = false;
    m_ponderStatus.completedRootNodes = 0U;
    m_ponderStatus.elapsedMs = 0U;
    m_ponderStatus.fullRoot = false;
    m_ponderStatus.reusedFloor = false;
    m_ponderElapsed.Reset();
    ThinkWorker().RecordPreflight();
    if (!ThinkWorker().Start(m_engine)) {
        m_engine.CancelThink();
        uint8_t progress = 0U;
        UciMove ignored;
        (void)m_engine.PollThinkProgress(progress, ignored);
        m_ponderThinking = false;
        m_ponderRootValid = false;
        return false;
    }
    ++m_ponderStatus.starts;
    logger::Trace("ponder: start prediction=%s level=%u nodes=%u depth=%u deadline=%u",
        m_ponderStatus.predictedHumanMove.CStr(), unsigned(m_ponderConfig.difficulty),
        unsigned(m_ponderConfig.maxNodes), unsigned(m_ponderConfig.maxDepth), unsigned(m_ponderConfig.maxThinkMs));
    return true;
}

bool GamePersistenceCoordinator::TryUsePonder(const uint32_t selectedLevel, ThinkConfig& config) {
    // Lower levels keep one foreground work budget, including after a level change.
    if (selectedLevel != 11U) { CancelPonder(true); return false; }
    if (!m_ponderRootValid) return false;
    if (m_gameVariant != GameVariant::Classic) { CancelPonder(true); return false; }
    // The unchanged retained history plus exactly one committed human move
    // binds repetition context. Same-looking FEN reached by undo/replay is a miss.
    litch_max::ParsedUciMove predicted;
    const uint16_t field = m_ponderHistoryFieldCount;
    const bool historyMatches = field + UNDO_HISTORY_FIELDS_PER_MOVE <= UNDO_HISTORY_FIELD_CAPACITY &&
        m_undoHistoryRevision == m_ponderHistoryRevision + 1U &&
        m_undoHistoryFieldCount == field + UNDO_HISTORY_FIELDS_PER_MOVE &&
        m_gameHistoryPersistence.gameId == m_ponderGameId &&
        litch_max::TryParseUciMove(m_ponderStatus.predictedHumanMove.CStr(), predicted) &&
        m_undoHistoryFields[field + 5U] == predicted.fromSquare &&
        m_undoHistoryFields[field + 6U] == predicted.toSquare &&
        m_undoHistoryFields[field + 4U] == 0U; // Promotion follows its own cancellation boundary.
    const bool match = !m_ponderThinking && !ThinkWorker().IsActive() && historyMatches &&
        m_halfmoveClockPlies == m_ponderHalfmoveClock && SameProfile(config, m_ponderConfig) &&
        m_engine.PonderRootMatchesLive() && Qualified(m_ponderFloor);
    m_ponderRootValid = false;
    if (!match) {
        ++m_ponderStatus.misses;
        logger::Trace("ponder: miss prediction=%s", m_ponderStatus.predictedHumanMove.CStr());
        m_ponderFloor = {};
        return false;
    }
    litch_max::ParsedUciMove chosen;
    UciMove selected;
    if (m_ponderCompleted &&
        (!game_persistence_vs_cpu_internal::TryChooseClassicCpuMove(
             m_engine, m_ponderStatus.reply, chosen, selected) ||
         !selected.Equals(m_ponderStatus.reply.View()))) {
        ++m_ponderStatus.misses;
        m_ponderFloor = {};
        return false;
    }
    ++m_ponderStatus.hits;
    m_ponderFloor.replayed = true;
    config.completedRootFloor = m_ponderFloor;
    m_ponderStatus.reusedFloor = !m_ponderCompleted;
    logger::Trace("ponder: hit prediction=%s reply=%s full=%u depth=%u completed_profile=%u",
        m_ponderStatus.predictedHumanMove.CStr(), m_ponderStatus.reply.CStr(), unsigned(m_ponderStatus.fullRoot),
        unsigned(m_ponderFloor.completedDepth), unsigned(m_ponderCompleted));
    if (!m_ponderCompleted) return false; // Continue ordinary foreground budget with a qualified floor.
    bool capture = false;
    uint8_t captureSquare = chosen.toSquare;
    (void)game_persistence_update_internal::TryComputeCpuCaptureSquareAndIsCapture(
        m_cachedFen.CStr(), chosen.fromSquare, chosen.toSquare, captureSquare, capture);
    m_pendingCpuMoveUciValid = true;
    m_pendingCpuMoveUci = selected;
    m_pendingCpuMoveFrom = chosen.fromSquare;
    m_pendingCpuMoveTo = chosen.toSquare;
    m_pendingCpuMoveCaptureSquareValid = capture;
    m_pendingCpuMoveCaptureSquare = captureSquare;
    return true;
}

} // namespace modules
