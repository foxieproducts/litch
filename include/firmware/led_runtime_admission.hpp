#pragma once

#include <cstdint>

namespace firmware {

enum class LedRenderAttemptResult : uint8_t {
    NotAttempted = 0,
    Completed,
    Denied,
};

// The cooperative loop's render admission. OTA entry/exit renders immediately;
// transfer frames remain admitted at the caller's bounded refresh cadence.
class LedRuntimeAdmission final {
  public:
    bool Update(const bool otaPresentationActive,
                const bool cadenceDue,
                const uint32_t nowMs) {
        const bool changed = otaPresentationActive != m_otaPresentationActive;
        m_otaPresentationActive = otaPresentationActive;
        const bool retryDue = m_retryPending &&
                              nowMs - m_lastAttemptMs >= RETRY_INTERVAL_MS;
        return changed || cadenceDue || retryDue;
    }

    void NoteRenderOutcome(const LedRenderAttemptResult result,
                           const uint32_t nowMs) {
        if (result == LedRenderAttemptResult::NotAttempted) {
            m_retryPending = false;
            return;
        }
        m_retryPending = result == LedRenderAttemptResult::Denied;
        m_lastAttemptMs = nowMs;
    }

    // Include the rejected transfer's deadline in the cooperative sleep.
    // Eligibility alone cannot wake the loop before the normal cadences.
    uint32_t ConstrainNextWakeMs(const uint32_t cadenceRemainingMs,
                                 const uint32_t nowMs) const {
        if (!m_retryPending) return cadenceRemainingMs;
        const uint32_t elapsedMs = nowMs - m_lastAttemptMs;
        const uint32_t retryRemainingMs = elapsedMs < RETRY_INTERVAL_MS
            ? RETRY_INTERVAL_MS - elapsedMs : 0U;
        return retryRemainingMs < cadenceRemainingMs
            ? retryRemainingMs : cadenceRemainingMs;
    }

  private:
    static constexpr uint32_t RETRY_INTERVAL_MS = 1U;
    bool m_otaPresentationActive = false;
    bool m_retryPending = false;
    uint32_t m_lastAttemptMs = 0U;
};

} // namespace firmware
