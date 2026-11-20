#include <cstdint>

#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"

namespace modules {
namespace {

constexpr uint32_t MIN_HINT_RETRY_DELAY_MS = 250U;
constexpr uint32_t MAX_HINT_RETRY_DELAY_MS = 4000U;
constexpr uint8_t MAX_HINT_RETRY_SHIFT = 4U;

} // namespace

void GamePersistenceCoordinator::ResetHintRetry() {
    m_hintRetryDelayActive = false;
    m_hintRetryFailureCount = 0U;
    m_hintRetryDelayMs = 0U;
}

void GamePersistenceCoordinator::ScheduleHintRetry(const char* reason) {
    if (m_hintRetryFailureCount != UINT8_MAX) {
        ++m_hintRetryFailureCount;
    }
    const uint8_t shift = (m_hintRetryFailureCount > MAX_HINT_RETRY_SHIFT)
                              ? MAX_HINT_RETRY_SHIFT
                              : static_cast<uint8_t>(m_hintRetryFailureCount - 1U);
    const uint32_t delayMs = MIN_HINT_RETRY_DELAY_MS * static_cast<uint32_t>(1U << shift);
    m_hintRetryDelayMs = (delayMs > MAX_HINT_RETRY_DELAY_MS) ? MAX_HINT_RETRY_DELAY_MS : delayMs;
    m_hintRetryElapsed.Reset();
    m_hintRetryDelayActive = true;
    m_hintPendingStart = true;
    logger::Warn("hint: retry scheduled (reason=%s delay_ms=%lu failures=%u)",
                 (reason == nullptr) ? "unknown" : reason,
                 static_cast<unsigned long>(m_hintRetryDelayMs),
                 static_cast<unsigned>(m_hintRetryFailureCount));
}

bool GamePersistenceCoordinator::IsHintRetryReady() {
    if (!m_hintRetryDelayActive) {
        return true;
    }
    if (!m_hintRetryElapsed.HasElapsed(m_hintRetryDelayMs)) {
        return false;
    }
    m_hintRetryDelayActive = false;
    return true;
}

bool GamePersistenceCoordinator::RecoverEngineAfterFailedThinkStart() {
    m_engine.CancelThink();
    if (UsesAsyncThinkWorker() && ThinkWorker().IsActive()) {
        return ThinkWorker().CancelAndJoin(m_engine);
    }
    uint8_t ignoredProgress = 0U;
    UciMove ignoredBest;
    (void)m_engine.PollThinkProgress(ignoredProgress, ignoredBest);
    return true;
}

} // namespace modules
