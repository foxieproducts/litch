#pragma once

#include <cstdint>

#include "foxie_embedded/millis.hpp"

namespace modules {

struct CpuThinkingPresentation {
    char side = '\0';
    uint32_t elapsedHalfSeconds = 0U;
    uint16_t remainingSeconds = 0U; // Zero means unknown, never completed.

    bool operator==(const CpuThinkingPresentation& other) const {
        return side == other.side &&
               elapsedHalfSeconds == other.elapsedHalfSeconds &&
               remainingSeconds == other.remainingSeconds;
    }
};

// Main-loop estimate of the current job's node/time bounds, not a completion promise.
// No engine reads, allocations, or influence on the actual search budget.
class CpuThinkingEstimate {
  public:
    void Clear() { *this = CpuThinkingEstimate{}; }

    void Start(const char side, const uint32_t budgetNodes,
               const uint32_t maxThinkMs = 0U) {
        Clear();
        if (side != 'w' && side != 'b') return;
        m_presentation.side = side;
        m_budgetNodes = budgetNodes;
        m_maxThinkMs = maxThinkMs;
        m_startMs = fxe::Millis();
        m_lastAdvanceMs = m_startMs;
    }

    void Update(const uint32_t nodes) {
        if (m_presentation.side == '\0') return;
        constexpr uint32_t PUBLISH_INTERVAL_MS = 500U;
        constexpr uint32_t STALE_SAMPLE_MS = 2000U;
        constexpr uint32_t CUSHION_SECONDS = 2U;
        const uint32_t nowMs = fxe::Millis();
        const uint32_t elapsedMs = nowMs - m_startMs;
        if (nodes > m_nodes) {
            m_nodes = nodes;
            m_lastAdvanceMs = nowMs;
            if (m_samples < 2U) ++m_samples;
        }
        const uint32_t halfSeconds = elapsedMs / PUBLISH_INTERVAL_MS;
        const bool publishTick = halfSeconds != m_presentation.elapsedHalfSeconds;
        m_presentation.elapsedHalfSeconds = halfSeconds;
        if (m_samples < 2U || elapsedMs < PUBLISH_INTERVAL_MS || nodes == 0U ||
            nodes < m_nodes || nodes >= m_budgetNodes ||
            (m_maxThinkMs != 0U && elapsedMs >= m_maxThinkMs) ||
            nowMs - m_lastAdvanceMs >= STALE_SAMPLE_MS) {
            m_presentation.remainingSeconds = 0U;
            return;
        }
        if (!publishTick) return;
        if (m_estimateNodes != nodes) {
            m_estimateNodes = nodes;
            const uint64_t numerator =
                static_cast<uint64_t>(m_budgetNodes - nodes) *
                static_cast<uint32_t>(m_lastAdvanceMs - m_startMs);
            const uint64_t denominator = static_cast<uint64_t>(nodes) * 1000U;
            m_estimateSeconds = numerator / denominator +
                (numerator % denominator != 0U ? 1U : 0U) + CUSHION_SECONDS;
            m_estimateAtMs = m_lastAdvanceMs;
        }
        // A stalled sample never pushes its old estimate further into the
        // future. New measured progress can replace it; expiry is unknown.
        const uint32_t ageSeconds = (nowMs - m_estimateAtMs) / 1000U;
        uint64_t remainingSeconds = ageSeconds < m_estimateSeconds
            ? m_estimateSeconds - ageSeconds : 0U;
        if (m_maxThinkMs != 0U) {
            const uint32_t remainingMs = m_maxThinkMs - elapsedMs;
            const uint64_t timeBoundSeconds = remainingMs / 1000U +
                (remainingMs % 1000U != 0U ? 1U : 0U) + CUSHION_SECONDS;
            if (remainingSeconds > timeBoundSeconds) remainingSeconds = timeBoundSeconds;
        }
        m_presentation.remainingSeconds = remainingSeconds <= UINT16_MAX
            ? static_cast<uint16_t>(remainingSeconds) : 0U;
    }

    CpuThinkingPresentation Snapshot() const { return m_presentation; }

  private:
    CpuThinkingPresentation m_presentation{};
    uint32_t m_budgetNodes = 0U;
    uint32_t m_maxThinkMs = 0U;
    uint32_t m_startMs = 0U;
    uint32_t m_lastAdvanceMs = 0U;
    uint32_t m_nodes = 0U;
    uint32_t m_estimateNodes = 0U;
    uint32_t m_estimateAtMs = 0U;
    uint64_t m_estimateSeconds = 0U;
    uint8_t m_samples = 0U;
};

} // namespace modules
