#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/checkmate_animation_setting.hpp"
#include "modules/settle_animation_setting.hpp"

namespace modules::led_frame_composer { struct ComposeInputs; }

namespace modules {

enum class BoardStylePreviewKind : uint8_t { None, Placement, Checkmate };

// Immutable presentation input; it never claims a game result or physical move.
struct BoardStylePreviewFrame {
    BoardStylePreviewKind kind = BoardStylePreviewKind::None;
    uint32_t elapsedMs = 0U;
    uint8_t anchorSquare = 255U;
    bool white = true;
    SettleAnimationStyle placementStyle = SettleAnimationStyle::None;
    CheckmateAnimationStyle checkmateStyle = CheckmateAnimationStyle::FireLob;
};

inline uint32_t BoardStylePreviewDurationMs(const BoardStylePreviewFrame& frame,
                                           const bool minimalAnimations) {
    return frame.kind == BoardStylePreviewKind::Checkmate ? 2000U
        : settle_animation_setting::DurationMs(frame.placementStyle, minimalAnimations);
}

class BoardStylePreview final {
  public:
    void SettingAccepted(const SettingsStore& settings, const char* canonicalKey);
    void Cancel();
    BoardStylePreviewFrame Capture(const led_frame_composer::ComposeInputs& inputs);

  private:
    BoardStylePreviewFrame m_frame{};
    fxe::ElapsedTime m_elapsed;
    bool m_anchorPending = false;
};

} // namespace modules
