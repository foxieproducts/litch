#include "runtime/progress_watchdog.hpp"
#include "modules/litch_max_engine.hpp"

#include <algorithm>

#include "logger.hpp"
#include <litch_max/duck_chess_search.hpp>
#include <litch_max/chess_notation.hpp>

namespace modules {

bool LitchMaxEngine::DuckSearchStopped(void* userdata) {
    runtime::FeedProgressWatchdog();
    auto* self = static_cast<LitchMaxEngine*>(userdata);
    if (self != nullptr) {
        self->PublishContinuousCandidate();
        constexpr uint32_t YIELD_INTERVAL_MS = 8U;
        const uint32_t now = fxe::Millis();
        if (static_cast<uint32_t>(now - self->m_duckLastYieldMs) >= YIELD_INTERVAL_MS) {
            runtime::DelayDiagnosticSlice(1U);
            self->m_duckLastYieldMs = fxe::Millis();
        }
    }
    return self == nullptr ||
           self->m_cancelRequested.load(std::memory_order_relaxed) ||
           self->IsThinkTimeUp();
}

bool LitchMaxEngine::StartDuckThink(const char* fen,
                                  const uint8_t duckSquare,
                                  const ThinkConfig& cfg) {
    if (m_thinking || fen == nullptr || fen[0] == '\0' ||
        duckSquare >= 64U || !m_duckThinkFen.Assign(fen)) {
        return false;
    }

    const bool sameAnalysisPosition = m_continuousAnalysisFen.Equals(fen) &&
                                      m_continuousAnalysisDuckSquare == duckSquare;
    if (!sameAnalysisPosition) {
        ClearContinuousAnalysis();
        if (cfg.continuousAnalysis) {
            if (!m_continuousAnalysisFen.Assign(fen)) return false;
            m_continuousAnalysisDuckSquare = duckSquare;
        }
    }

    litch_max::DuckChessTurn initial;
    litch_max::ParsedUciMove parsed;
    m_initialCandidate = ThinkCandidate{};
    if (litch_max::FindImmediateDuckChessTurn(fen, duckSquare, initial) &&
        litch_max::TryParseUciMove(initial.chessMove.CStr(), parsed)) {
        m_initialCandidate.fromSquare = parsed.fromSquare;
        m_initialCandidate.toSquare = parsed.toSquare;
        m_initialCandidate.promotion = parsed.hasPromotion ? parsed.promotionSuffixLower : '\0';
        m_initialCandidate.isDuck = true;
        m_initialCandidate.capturedKing = initial.capturedKing;
        m_initialCandidate.duckDestination = initial.duckDestination;
        m_initialCandidate.score = initial.score;
        m_initialCandidate.source = ThinkCandidateSource::Seed;
        m_initialCandidate.rootKind = ThinkRootKind::Seed;
        if (cfg.continuousAnalysis && !m_continuousCandidate.IsAvailable()) {
            m_continuousCandidate = m_initialCandidate;
        }
    }
    m_thinking = true;
    m_thinkMode = ThinkMode::Duck;
    m_cancelRequested.store(false, std::memory_order_relaxed);
    m_timeUp.store(false, std::memory_order_relaxed);
    m_lastThinkNodeCount = 0U;
    m_lastStopReason = ThinkStopReason::Unavailable;
    m_thinkElapsed.Reset();
    m_duckLastYieldMs = fxe::Millis();
    m_lastThinkCfg = cfg;
    m_hintCalculationElapsed = fxe::ElapsedTime{};
    m_lastCalculationMs = 0U;
    m_duckThinkSquare = duckSquare;
    m_lastClassicThinkResult = ClassicThinkResult{};
    m_lastClassicThinkResultValid = false;
    m_lastDuckThinkResult = DuckThinkResult{};
    m_lastDuckThinkResultValid = false;
    ThinkCandidate immediate = cfg.continuousAnalysis && m_continuousCandidate.IsAvailable()
        ? m_continuousCandidate : m_initialCandidate;
    immediate.replayed = cfg.continuousAnalysis && immediate.source != ThinkCandidateSource::Seed;
    if (cfg.continuousAnalysis) {
        m_continuousCandidatePending = immediate.source != ThinkCandidateSource::Seed;
        m_continuousCandidateReplayed = true;
        PublishContinuousCandidate();
    } else if (immediate.IsAvailable()) cfg.candidateObserver.Publish(immediate);
    return true;
}

void LitchMaxEngine::DuckCandidateCompletedCallback(
    void* userdata, const ThinkCandidate& candidate) {
    auto* self = static_cast<LitchMaxEngine*>(userdata);
    if (self == nullptr || self->m_cancelRequested.load(std::memory_order_relaxed)) return;
    ThinkCandidate completed = candidate;
    // An immediate king capture proves one complete turn before iterative search.
    if (completed.capturedKing && completed.completedDepth == 0U) {
        completed.completedDepth = 1U;
    }
    if (!completed.isDuck || completed.completedDepth == 0U ||
        completed.completedDepth < self->m_continuousCandidate.completedDepth) {
        return;
    }
    self->m_continuousCandidate = completed;
    self->m_continuousCandidatePending = true;
    self->m_continuousCandidateReplayed = false;
    self->PublishContinuousCandidate();
}

ThinkState LitchMaxEngine::PollDuckThinkProgress(
    uint8_t& outProgressPercent,
    UciMove& outBestMoveUci) {
    litch_max::DuckChessSearchConfig config;
    config.maxNodes = m_lastThinkCfg.maxNodes;
    config.rngSeed = m_lastThinkCfg.rngSeed;
    config.difficulty = m_lastThinkCfg.difficulty;
    config.progressObserver = m_lastThinkCfg.continuousAnalysis
        ? ThinkProgressObserver{&LitchMaxEngine::ContinuousProgress, this} : m_lastThinkCfg.progressObserver;
    if (m_lastThinkCfg.continuousAnalysis) {
        config.candidateObserver = {&LitchMaxEngine::DuckCandidateCompletedCallback, this,
            [](void* context, const ThinkCandidate& candidate) {
                static_cast<LitchMaxEngine*>(context)->m_lastThinkCfg.candidateObserver.RootCompleted(candidate);
            }};
    } else {
        config.candidateObserver = m_lastThinkCfg.candidateObserver;
    }
    config.continuousAnalysis = m_lastThinkCfg.continuousAnalysis;
    config.workspace = &m_searchWorkspace;
    config.maxDepth = static_cast<uint8_t>(std::min<uint32_t>(m_lastThinkCfg.maxDepth, 15U));
    ThinkProfile effective = ThinkProfile::Requested(m_lastThinkCfg);
    effective.maxDepth = config.maxDepth != 0U ? config.maxDepth :
        (config.continuousAnalysis ? 15U : (config.difficulty < 8U ? 1U : 2U));
    effective.minimumDepth = 1U;
    effective.firstRootNodeReserve = 0U;
    m_lastThinkCfg.candidateObserver.ProfileResolved(effective);
    litch_max::DuckChessTurn turn;
    litch_max::DuckChessSearchStats stats;
    const bool found = litch_max::FindBestDuckChessTurn(
        m_duckThinkFen.CStr(), m_duckThinkSquare, config,
        turn, stats, &LitchMaxEngine::DuckSearchStopped, this);
    m_lastThinkNodeCount = stats.nodesVisited;
    m_thinking = false;
    // A deadline is a budget stop: the search retains only complete compound
    // candidates and never promotes incompletely compared opponent replies.
    // Explicit cancellation still discards all output, including on a tie.
    if (m_cancelRequested.load(std::memory_order_relaxed) ||
        (stats.canceled && !m_timeUp.load(std::memory_order_relaxed))) {
        m_lastStopReason = ThinkStopReason::Canceled;
        return ThinkState::Canceled;
    }
    if (!found) {
        logger::Warn(
            "duck search failed nodes=%lu scratch=%lu canceled=%u "
            "depth=%u deadline=%u",
            static_cast<unsigned long>(stats.nodesVisited),
            static_cast<unsigned long>(stats.workspaceBytes),
            stats.canceled ? 1U : 0U,
            static_cast<unsigned>(stats.completedDepth),
            m_timeUp.load(std::memory_order_relaxed) ? 1U : 0U);
        const ThinkCandidate& retained = m_lastThinkCfg.continuousAnalysis
            ? m_continuousCandidate : m_initialCandidate;
        if (!retained.IsAvailable() ||
            !litch_max::TryFormatUciMove(retained.fromSquare, retained.toSquare,
                             retained.promotion, turn.chessMove)) return ThinkState::Error;
        turn.duckDestination = retained.duckDestination;
        turn.capturedKing = retained.capturedKing;
        turn.score = retained.score;
    }

    if (m_lastThinkCfg.continuousAnalysis && !turn.capturedKing &&
        stats.completedDepth < m_continuousCandidate.completedDepth) {
        const ThinkCandidate& retained = m_continuousCandidate;
        if (!litch_max::TryFormatUciMove(retained.fromSquare, retained.toSquare,
                             retained.promotion, turn.chessMove)) return ThinkState::Error;
        turn.duckDestination = retained.duckDestination;
        turn.capturedKing = retained.capturedKing;
        turn.score = retained.score;
    }

    m_lastDuckThinkResult.chessMove = turn.chessMove;
    m_lastDuckThinkResult.duckDestination = turn.duckDestination;
    m_lastDuckThinkResult.score = turn.score;
    m_lastDuckThinkResult.capturedKing = turn.capturedKing;
    m_lastDuckThinkResultValid = true;
    m_lastStopReason = m_timeUp.load(std::memory_order_relaxed) ? ThinkStopReason::Deadline :
        (m_lastThinkCfg.maxNodes != 0U && stats.nodesVisited >= m_lastThinkCfg.maxNodes
             ? ThinkStopReason::NodeCap : ThinkStopReason::Exhausted);
    outBestMoveUci = turn.chessMove;
    outProgressPercent = 100U;
    return ThinkState::Done;
}

bool LitchMaxEngine::GetLastDuckThinkResult(
    DuckThinkResult& outResult) const {
    if (!m_lastDuckThinkResultValid) {
        outResult = DuckThinkResult{};
        return false;
    }
    outResult = m_lastDuckThinkResult;
    return true;
}

} // namespace modules
