#pragma once

#include <cstdint>

namespace modules {

// Presentation only: physical validation and recovery remain immediate.
enum class CorrectionFeedback : uint8_t { Quiet, Gentle, Strong };

inline constexpr uint32_t CORRECTION_QUIET_MS = 300U;
inline constexpr uint32_t CORRECTION_STRONG_MS = 1500U;

constexpr CorrectionFeedback CorrectionFeedbackForElapsed(
    const uint32_t elapsedMs) {
    if (elapsedMs < CORRECTION_QUIET_MS) return CorrectionFeedback::Quiet;
    if (elapsedMs < CORRECTION_STRONG_MS) return CorrectionFeedback::Gentle;
    return CorrectionFeedback::Strong;
}

} // namespace modules
