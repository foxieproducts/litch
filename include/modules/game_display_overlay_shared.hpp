#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/rgb.hpp"

namespace modules::game_display_overlay_shared {

struct Timeline {
    uint32_t fadeInMs = 0;
    uint32_t holdMs = 0;
    uint32_t fadeOutMs = 0;
};

template <size_t N>
struct TimedSnapshotOverlayState {
    bool active = false;
    fxe::ElapsedTime elapsed;
    std::array<Rgb, N> snapshot{};
};

inline constexpr uint8_t ScaleU8(const uint8_t value, const uint8_t scale255) {
    return static_cast<uint8_t>((static_cast<uint16_t>(value) * static_cast<uint16_t>(scale255) + 127U) / 255U);
}

inline constexpr Rgb ScaleRgb(const Rgb color, const uint8_t scale255) {
    return Rgb{ScaleU8(color.r, scale255), ScaleU8(color.g, scale255), ScaleU8(color.b, scale255)};
}

inline uint32_t TotalDurationMs(const Timeline& timeline) {
    return timeline.fadeInMs + timeline.holdMs + timeline.fadeOutMs;
}

inline uint8_t ComputeAlpha255(const Timeline& timeline, const uint32_t elapsedMs) {
    if (timeline.fadeInMs > 0U && elapsedMs < timeline.fadeInMs) {
        const uint32_t num = elapsedMs * 255U;
        return static_cast<uint8_t>((num + (timeline.fadeInMs / 2U)) / timeline.fadeInMs);
    }

    const uint32_t holdEnd = timeline.fadeInMs + timeline.holdMs;
    if (elapsedMs < holdEnd) {
        return 255U;
    }

    const uint32_t fadeOutStart = holdEnd;
    const uint32_t fadeOutEnd = fadeOutStart + timeline.fadeOutMs;
    if (timeline.fadeOutMs > 0U && elapsedMs < fadeOutEnd) {
        const uint32_t t = elapsedMs - fadeOutStart;
        const uint32_t num = (timeline.fadeOutMs - t) * 255U;
        return static_cast<uint8_t>((num + (timeline.fadeOutMs / 2U)) / timeline.fadeOutMs);
    }

    return 0U;
}

template <size_t N>
inline void ResetTimedSnapshotOverlay(TimedSnapshotOverlayState<N>& state) {
    state.active = false;
    state.elapsed = fxe::ElapsedTime();
}

template <size_t N>
inline void StartTimedSnapshotOverlay(TimedSnapshotOverlayState<N>& state,
                                      const std::array<Rgb, N>& currentFrame) {
    if (!state.active) {
        state.snapshot = currentFrame;
    }
    state.elapsed.Reset();
    state.active = true;
}

template <size_t N>
inline bool TryCopySnapshot(const TimedSnapshotOverlayState<N>& state,
                            std::array<Rgb, N>& outFrame) {
    if (!state.active) {
        return false;
    }
    outFrame = state.snapshot;
    return true;
}

template <size_t N>
inline bool IsTimedOverlayActive(const TimedSnapshotOverlayState<N>& state,
                                 const Timeline& timeline) {
    if (!state.active) {
        return false;
    }

    const uint32_t total = TotalDurationMs(timeline);
    if (total == 0U) {
        return false;
    }

    return state.elapsed.ElapsedMs() < total;
}

template <size_t N>
inline void ApplyBackgroundDim(std::array<Rgb, N>& frame, const uint8_t backgroundScale255) {
    for (auto& pixel : frame) {
        pixel = ScaleRgb(pixel, backgroundScale255);
    }
}

} // namespace modules::game_display_overlay_shared