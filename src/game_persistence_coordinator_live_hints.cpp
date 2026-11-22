#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include <litch_max/chess_notation.hpp>

namespace modules {

void GamePersistenceCoordinator::ConsumeLiveHintCandidate() {
    if (!m_hintThinking || m_vsCpuThinking || m_cancelAllThinkingRequested ||
        m_cancelHintWorkRequested) {
        return;
    }
    // A revealed target owns physical guidance until stable Idle. An unseen
    // cached root must not freeze the older move before its first reveal.
    uint8_t renderedFrom = 64U;
    uint8_t renderedTo = 64U;
    if (!IsHintSearchStableIdle() && m_stateMachine.IsHintDisplayRequested() &&
        m_stateMachine.TryGetHintMoveSquares(renderedFrom, renderedTo)) {
        return;
    }
    ThinkCandidate candidate;
    if (!ThinkWorker().TryGetPublishedCandidate(candidate) ||
        candidate.isDuck != IsDuckGame() ||
        candidate.source == ThinkCandidateSource::Seed) {
        return;
    }
    if (candidate.isDuck) {
        if (CacheLiveDuckHint(candidate) &&
            !m_stateMachine.IsHintDisplayRequested() &&
            !CanPublishHint(candidate.fromSquare, candidate.toSquare)) {
            m_stateMachine.ClearHintMove();
        }
        return;
    }
    if (candidate.fromSquare >= 64U || candidate.toSquare >= 64U ||
        (m_cachedLegalComputedFromSquaresMask64 &
         (1ULL << candidate.fromSquare)) == 0ULL ||
        (m_cachedLegalDestByFromSquare[candidate.fromSquare] &
         (1ULL << candidate.toSquare)) == 0ULL) {
        return;
    }
    if (!m_hintCachedMoveValid ||
        !m_hintCachedFen.Equals(m_cachedFen.View()) ||
        m_hintCachedFromSquare != candidate.fromSquare ||
        m_hintCachedToSquare != candidate.toSquare) {
        CacheHintMoveForCurrentPosition(candidate.fromSquare, candidate.toSquare);
    }
    if (CanPublishHint(candidate.fromSquare, candidate.toSquare)) {
        m_stateMachine.SetHintMoveSquares(candidate.fromSquare, candidate.toSquare);
        RecordAcceptedHintDiagnostic();
    } else if (!m_stateMachine.IsHintDisplayRequested()) {
        // A newer global result can use another source. Retain that result,
        // but do not let the first request reveal a superseded hidden target.
        m_stateMachine.ClearHintMove();
    }
}

void GamePersistenceCoordinator::RecordHintSearchStats(
    const uint32_t elapsedMs, const uint32_t nodes) {
    m_hintCalculatedMs += elapsedMs;
    m_hintScannedNodes = nodes > UINT32_MAX - m_hintScannedNodes
        ? UINT32_MAX : m_hintScannedNodes + nodes;
}

void GamePersistenceCoordinator::LogRequestedHint() {
    if (!IsCurrentHintAnalysisPosition()) {
        logger::Trace("hint: requested (search_ms=0 nodes_observed=0)");
        return;
    }
    uint32_t searchMs = m_hintThinking ? m_hintSearchElapsed.ElapsedMs() : 0U;
    uint32_t liveNodes = m_hintThinking
        ? ThinkWorker().PublishedNodeCount() : 0U;
    // A held physical move may defer reaping, but completed work does not
    // accumulate more calculation time while its suggestion remains visible.
    if (m_hintThinking && UsesAsyncThinkWorker() &&
        ThinkWorker().IsDone() && !ThinkWorker().IsActive()) {
        uint8_t progress = 0U;
        UciMove best;
        (void)ThinkWorker().GetResult(progress, best, searchMs, liveNodes);
    }
    const uint32_t elapsedMs = m_hintCalculatedMs + searchMs;
    const uint32_t nodes = liveNodes > UINT32_MAX - m_hintScannedNodes
        ? UINT32_MAX : m_hintScannedNodes + liveNodes;
    logger::Trace("hint: requested (search_ms=%lu nodes_observed=%lu)",
                 static_cast<unsigned long>(elapsedMs),
                 static_cast<unsigned long>(nodes));
}

} // namespace modules
