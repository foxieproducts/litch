#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hardware {

enum class LedDeliveryOutcome : uint8_t {
    FlashDenied,
    BusWaitTimedOut,
    Unchanged,
    Unconfirmed,
    Confirmed,
};

struct LedDeliveryGap {
    uint32_t atMs = 0U;
    uint32_t gapMs = 0U;
    bool confirmation = false;
};

struct LedDeliverySnapshot {
    uint32_t attempts = 0U;
    std::array<uint32_t, 5> outcomes{};
    uint32_t submissions = 0U;
    uint32_t confirmations = 0U;
    uint32_t lastSubmissionMs = 0U;
    uint32_t lastConfirmationMs = 0U;
    uint32_t maxSubmissionGapMs = 0U;
    uint32_t maxConfirmationGapMs = 0U;
    uint32_t maxPreSubmitMs = 0U;
    uint32_t maxShowMs = 0U;
    // Gap buckets: <=40, <=60, <=100, <=250, >250 ms. First TX has no gap.
    std::array<uint32_t, 5> submissionGaps{};
    std::array<uint32_t, 5> confirmationGaps{};
    std::array<LedDeliveryGap, 8> recentLongGaps{};
    uint8_t recentLongGapCount = 0U;
};

// Owned by the cooperative output driver. No logging, allocation or engine
// access in Show; snapshot collection happens only on a diagnostic request.
class LedDeliveryTelemetry final {
  public:
    void OnAttempt(const uint32_t nowMs) {
        ++m_snapshot.attempts;
        m_attemptMs = nowMs;
    }

    void OnSubmission(const uint32_t nowMs) {
        if (m_haveSubmission) {
            RecordGap(nowMs, nowMs - m_snapshot.lastSubmissionMs, false);
        }
        m_haveSubmission = true;
        ++m_snapshot.submissions;
        m_snapshot.lastSubmissionMs = nowMs;
        Max(m_snapshot.maxPreSubmitMs, nowMs - m_attemptMs);
    }

    void OnOutcome(const LedDeliveryOutcome outcome, const uint32_t nowMs) {
        ++m_snapshot.outcomes[static_cast<size_t>(outcome)];
        Max(m_snapshot.maxShowMs, nowMs - m_attemptMs);
        // An unchanged clean buffer is acknowledged, but sends no new frame.
        if (outcome != LedDeliveryOutcome::Confirmed) {
            return;
        }
        if (m_haveConfirmation) {
            RecordGap(nowMs, nowMs - m_snapshot.lastConfirmationMs, true);
        }
        m_haveConfirmation = true;
        ++m_snapshot.confirmations;
        m_snapshot.lastConfirmationMs = nowMs;
    }

    LedDeliverySnapshot GetSnapshot() const {
        auto result = m_snapshot;
        const size_t count = result.recentLongGapCount;
        const size_t begin = count == result.recentLongGaps.size()
                                 ? m_nextLongGap : 0U;
        for (size_t i = 0U; i < count; ++i) {
            result.recentLongGaps[i] = m_snapshot.recentLongGaps[
                (begin + i) % result.recentLongGaps.size()];
        }
        return result;
    }

  private:
    static void Max(uint32_t& maximum, const uint32_t value) {
        if (value > maximum) maximum = value;
    }

    void RecordGap(const uint32_t nowMs, const uint32_t gapMs,
                   const bool confirmation) {
        auto& buckets = confirmation ? m_snapshot.confirmationGaps
                                     : m_snapshot.submissionGaps;
        ++buckets[gapMs <= 40U ? 0U : gapMs <= 60U ? 1U :
                  gapMs <= 100U ? 2U : gapMs <= 250U ? 3U : 4U];
        Max(confirmation ? m_snapshot.maxConfirmationGapMs
                         : m_snapshot.maxSubmissionGapMs, gapMs);
        if (gapMs <= 60U) return;
        m_snapshot.recentLongGaps[m_nextLongGap] = {nowMs, gapMs, confirmation};
        m_nextLongGap = (m_nextLongGap + 1U) % m_snapshot.recentLongGaps.size();
        if (m_snapshot.recentLongGapCount < m_snapshot.recentLongGaps.size()) {
            ++m_snapshot.recentLongGapCount;
        }
    }

    LedDeliverySnapshot m_snapshot{};
    uint32_t m_attemptMs = 0U;
    size_t m_nextLongGap = 0U;
    bool m_haveSubmission = false;
    bool m_haveConfirmation = false;
};

} // namespace hardware
