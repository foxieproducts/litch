#pragma once

#include <cstdint>

#include "modules/board_led_frame.hpp"

namespace modules {

// Physical board coordinates: x increases to the right, y toward the P1 end.
// Values describe apparent gravity (opposite accelerometer specific force).
struct WaterSloshMotion {
    float downXG = 0.0F;
    float downYG = 0.0F;
    float normalG = 1.0F;
};

struct WaterSloshMetrics {
    float volume = 0.0F;
    float centerX = 0.0F;
    float centerY = 0.0F;
    float surfaceVariance = 0.0F;
    float speedSquared = 0.0F;
    float minimumDepth = 0.0F;
    float maximumDepth = 0.0F;
    float normalG = 1.0F;
    float foam = 0.0F;
    float ambientGain = 1.0F;
};

// Compact liquid illusion: two damped displacement modes and two faster crest
// modes, evaluated at LED positions. No spatial grid, allocation or randomness.
// The injected elapsed time is accumulated into 8 ms steps; long gaps are
// deliberately discarded rather than replayed on the cooperative render task.
class WaterSlosh final {
  public:
    static constexpr uint8_t RESOLUTION = 16U;
    static constexpr uint16_t CELL_COUNT = RESOLUTION * RESOLUTION;
    static constexpr float REST_DEPTH = 0.21F;
    static constexpr uint32_t STEP_MS = 8U;
    static constexpr uint32_t MAX_ADVANCE_MS = 64U;

    WaterSlosh() = default;
    void Reset();
    void Advance(const WaterSloshMotion& motion, uint32_t elapsedMs);
    void ExciteFoam(float changeG);
    void SetAmbientLight(float lux, bool available);
    WaterSloshMetrics Measure() const;
    // Output stays in physical coordinates. Caller must cancel any chess
    // orientation at the semantic output boundary, never rotate the liquid.
    BoardSquareLeds ComposeSquare(uint8_t physicalSquare) const;
    Rgb ComposePixel(uint32_t physicalIndex) const;
    Rgb ComposeStatusPixel(uint8_t physicalIndex) const;
    uint16_t RiderScale256(uint8_t physicalSquare) const;
    void Compose(BoardLedFrame& frame, uint8_t opacity255 = 255U) const;

  private:
    void Step(const WaterSloshMotion& motion);
    __attribute__((always_inline)) float SampleDepth(float x, float y) const;
    __attribute__((always_inline)) Rgb SurfaceColor(float x, float y, uint32_t physicalIndex) const;

    struct Mode {
        float displacement = 0.0F;
        float velocity = 0.0F;
        float crest = 0.0F;
        float crestVelocity = 0.0F;
        void Step(float force, float normal);
        float Profile(float position) const;
    };
    Mode m_x{};
    Mode m_y{};
    float m_normalG = 1.0F;
    float m_foam = 0.0F;
    uint32_t m_remainderMs = 0U;
    uint8_t m_light255 = 128U;
};

} // namespace modules
