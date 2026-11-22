#include "modules/edge_led_hint_feedback.hpp"

namespace modules {
namespace {

constexpr uint32_t RESULT_PULSE_MS = 240U;
constexpr uint32_t RESULT_GAP_MS = 120U;
constexpr uint32_t RESULT_SECOND_PULSE_MS = RESULT_PULSE_MS + RESULT_GAP_MS;
constexpr uint32_t RESULT_FADE_MS = 120U;
constexpr uint32_t RESULT_END_MS = RESULT_SECOND_PULSE_MS + RESULT_PULSE_MS + RESULT_FADE_MS;
constexpr uint32_t UNAVAILABLE_MS = 720U;
} // namespace

void ApplyHintCornerFeedback(const EdgeLedState& state,
                             std::array<Rgb, EDGE_LED_COUNT>& frame) {
    if (state.minimalAnimations || state.hintFeedbackOwner == UiOwner::None ||
        (state.mode != EdgeUiMode::IdleTurn && state.mode != EdgeUiMode::Placement)) {
        return;
    }
    const uint32_t elapsed = state.hintFeedbackElapsedMs;
    const bool pending = state.hintFeedbackPending && !state.hintFeedbackHasMove;
    if (pending) return;
    uint16_t scale = 0U;
    uint8_t blend = 255U;
    if (!pending && state.hintFeedbackHasMove) {
        if (elapsed >= RESULT_END_MS ||
            (elapsed >= RESULT_PULSE_MS && elapsed < RESULT_SECOND_PULSE_MS)) {
            return;
        }
        // Two bounded white pulses mean "look at the current suggestion".
        // The ordinary corners remain visible in the gap, so a changed tuple
        // cannot look like one long thinking indication.
        const uint32_t pulseAge = elapsed < RESULT_PULSE_MS
            ? elapsed : elapsed - RESULT_SECOND_PULSE_MS;
        const uint32_t riseMs = RESULT_PULSE_MS / 2U;
        scale = pulseAge < riseMs
            ? static_cast<uint16_t>(64U + pulseAge * 128U / riseMs)
            : pulseAge < RESULT_PULSE_MS
                ? static_cast<uint16_t>(192U - (pulseAge - riseMs) * 128U / riseMs)
                : 64U;
        if (pulseAge >= RESULT_PULSE_MS) {
            blend = static_cast<uint8_t>((RESULT_END_MS - elapsed) * 255U / RESULT_FADE_MS);
        }
    } else if (!pending) {
        // An exhausted request gets a restrained acknowledgement, then ends.
        if (elapsed >= UNAVAILABLE_MS) {
            return;
        }
        scale = 64U;
        blend = static_cast<uint8_t>((UNAVAILABLE_MS - elapsed) * 255U / UNAVAILABLE_MS);
    }
    const auto indices = PlayerCornerLedIndices(state.hintFeedbackOwner);
    for (uint8_t index = 0U; index < indices.size(); ++index) {
        const Rgb white = ScaleRgbSaturatingBy256(RGB_WHITE,
            scale);
        frame[indices[index]] = AlphaBlendRgb(frame[indices[index]], white, blend);
    }
}

} // namespace modules
