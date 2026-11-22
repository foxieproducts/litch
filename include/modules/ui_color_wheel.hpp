#pragma once

#include <cstdint>

#include "modules/rgb.hpp"
#include "modules/settings_store.hpp"

namespace modules::ui_color_wheel {

static constexpr uint8_t MIN_SEPARATION = 32;

constexpr uint8_t AbsDiffU8(const uint8_t a, const uint8_t b) {
    return (a >= b) ? static_cast<uint8_t>(a - b) : static_cast<uint8_t>(b - a);
}

// Minimum distance on a 0..255 ring (0..128).
constexpr uint8_t CircularDistance(const uint8_t a, const uint8_t b) {
    const uint8_t d = AbsDiffU8(a, b);
    const uint8_t wrap = static_cast<uint8_t>(256U - d);
    return (d < wrap) ? d : wrap;
}

constexpr bool IsTooClose(const uint8_t a, const uint8_t b, const uint8_t minSeparation) {
    return CircularDistance(a, b) < minSeparation;
}

// Deterministic RGB wheel (255 positions). This matches the common Arduino
// "color wheel" semantics: R->G->B->R.
constexpr modules::Rgb WheelToRgb(const uint8_t wheel) {
    const uint8_t pos = static_cast<uint8_t>(255U - wheel);

    if (pos < 85U) {
        return modules::Rgb{static_cast<uint8_t>(255U - pos * 3U), 0, static_cast<uint8_t>(pos * 3U)};
    }

    if (pos < 170U) {
        const uint8_t p = static_cast<uint8_t>(pos - 85U);
        return modules::Rgb{0, static_cast<uint8_t>(p * 3U), static_cast<uint8_t>(255U - p * 3U)};
    }

    const uint8_t p = static_cast<uint8_t>(pos - 170U);
    return modules::Rgb{static_cast<uint8_t>(p * 3U), static_cast<uint8_t>(255U - p * 3U), 0};
}

inline uint8_t ClampWheelValue01To255(const uint32_t raw) {
    return static_cast<uint8_t>((raw > 255U) ? 255U : raw);
}

inline uint8_t ReadWheelValue01To255(const fxe::SettingsStore& settings,
                                     const char* key,
                                     const uint8_t defaultValue) {
    const uint32_t raw = settings.ReadValue(key, static_cast<uint32_t>(defaultValue));
    return ClampWheelValue01To255(raw);
}

// Step by +1/-1 on the ring, skipping over the forbidden zone around `other`.
inline uint8_t StepWithExclusion(const uint8_t current,
                                 const uint8_t other,
                                 const int delta,
                                 const uint8_t minSeparation = MIN_SEPARATION) {
    if (delta == 0) {
        return current;
    }

    const int step = (delta > 0) ? 1 : -1;
    uint8_t next = current;

    // Worst-case: skip the entire forbidden band plus land on the first allowed value.
    for (uint16_t i = 0; i < 256U; ++i) {
        next = static_cast<uint8_t>(static_cast<uint16_t>(next) + static_cast<uint16_t>(step));
        if (!IsTooClose(next, other, minSeparation)) {
            return next;
        }
    }

    return current;
}

// Ensure the pair is separated by at least minSeparation. If not, push P2 to
// the opposite side of the wheel.
inline void EnsureSeparated(uint8_t& ioP1,
                            uint8_t& ioP2,
                            const uint8_t minSeparation = MIN_SEPARATION) {
    if (!IsTooClose(ioP1, ioP2, minSeparation)) {
        return;
    }

    ioP2 = static_cast<uint8_t>(ioP1 + 128U);
}

} // namespace modules::ui_color_wheel
