#include "runtime/game_state_machine.hpp"

#include "foxie_embedded/elapsed_time.hpp"
#include "foxie_embedded/millis.hpp"

namespace runtime {

bool GameStateMachine::IsSettleFlashOn() const {
    // Settling flash accelerates over time and becomes solid at the end.
    // Use only state-machine-local timing to keep this deterministic.
    if (!IsMoveSettlingActive()) {
        return true;
    }

    // Test/debug harnesses may force Settling without starting the settle timer.
    // In that case, preserve the legacy fixed cadence behavior.
    if (!m_settleTimer.IsActive()) {
        const uint32_t p = (m_config.settleFlashMs == 0) ? 1U : m_config.settleFlashMs;
        return ((fxe::Millis() / p) % 2U) == 0U;
    }

    const uint32_t settleDurationMs = (m_activeSettleDurationMs == 0U)
                                          ? (m_cpuExecution.moveSettleActive ? m_config.settleDurationMs
                                                                             : m_humanSettleDurationMs)
                                          : m_activeSettleDurationMs;
    const uint32_t duration = (settleDurationMs == 0U) ? 1U : settleDurationMs;
    const uint32_t deadline = m_settleTimer.DeadlineMs();
    const uint32_t startMs = deadline - duration;
    const fxe::ElapsedTime sinceStart(startMs);
    const uint32_t elapsed = sinceStart.ElapsedMs();
    return SampleSettleFlashOn(elapsed, duration, m_config.settleFlashMs);
}

} // namespace runtime
