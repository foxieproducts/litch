#include "modules/litch_max_engine.hpp"
namespace modules {
bool LitchMaxEngine::StartBenchmark(const uint8_t caseId, const ThinkConfig& config) {
    const auto* item = GetEngineBenchmarkCase(caseId);
    if (m_thinking || item == nullptr || config.continuousAnalysis ||
        config.difficulty < 1U || config.difficulty > 11U) return false;
    m_benchmarkRoot = item->root;
    m_lastThinkCfg = config;
    m_lastThinkCfg.reversibleHalfmoves = item->reversibleHalfmoves;
    m_lastThinkCfg.openingPrefix = {};
    ThinkCandidate seed;
    if (litch_max::classic_chess::FindImmediateCandidate(m_benchmarkRoot, seed)) {
        m_lastThinkCfg.candidateObserver.Publish(seed);
    }
    m_thinkMode = ThinkMode::Benchmark;
    m_thinking = true;
    m_cancelRequested.store(false, std::memory_order_relaxed);
    m_timeUp.store(false, std::memory_order_relaxed);
    m_lastStopReason = ThinkStopReason::Unavailable;
    m_lastThinkNodeCount = 0U;
    m_thinkElapsed.Reset();
    return true;
}
} // namespace modules
