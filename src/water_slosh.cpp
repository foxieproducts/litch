#include "modules/water_slosh.hpp"

#include <algorithm>
#include <cmath>

#if defined(LITCH_ESP32S3)
#pragma GCC diagnostic error "-Wframe-larger-than=128"
#endif

namespace modules {
namespace {

// Run the tiny modal liquid model 1.5x faster than wall time. This changes the
// character of the slosh without raising render cadence, sample traffic, RAM,
// or the number of integration steps.
constexpr float DT = float(WaterSlosh::STEP_MS) * 1.5F / 1000.0F;
constexpr float DEAD_ZONE_G = 0.018F;
// Exact second/fourth moments of 16 equally spaced observation points in [-1,1].
// These normalize optical depth, not a stored or stepped spatial grid.
constexpr float MOMENT_2 = 17.0F / 45.0F;
constexpr float MOMENT_4 = 12937.0F / 50625.0F;

float BoundedForce(const float value) {
    const float magnitude = std::max(0.0F, std::fabs(value) - DEAD_ZONE_G);
    // std::max's first argument wins for NaN; infinities saturate safely.
    return std::copysign(std::min(magnitude, 1.5F), value);
}

uint8_t Channel(const float value) {
    // Bounded palette interpolation supplies nonnegative components <= 175.
    return static_cast<uint8_t>(value + 0.5F);
}

} // namespace

void WaterSlosh::Reset() {
    m_x = {};
    m_y = {};
    m_normalG = 1.0F;
    m_foam = 0.0F;
    m_remainderMs = 0U;
}

void WaterSlosh::ExciteFoam(const float changeG) {
    // A sample-to-sample high-pass of all three axes. The dead band rejects
    // measured rest offsets and gentle handling; repeated reversals froth.
    m_foam = std::min(1.0F, m_foam + std::max(0.0F, changeG - 0.24F) * 0.65F);
}

void WaterSlosh::SetAmbientLight(const float lux, const bool available) {
    m_light255 = available && std::isfinite(lux) && lux >= 0.0F
        ? static_cast<uint8_t>(255.0F * (lux / (lux + 200.0F))) : 128U;
}

void WaterSlosh::Advance(const WaterSloshMotion& motion,
                         const uint32_t elapsedMs) {
    m_remainderMs += std::min(elapsedMs, MAX_ADVANCE_MS);
    while (m_remainderMs >= STEP_MS) {
        Step(motion);
        m_remainderMs -= STEP_MS;
    }
}

void WaterSlosh::Mode::Step(const float force, const float normal) {
    // The slow mass lags the board. The faster even mode lifts/folds its crest,
    // including the reflected surge when the mass reaches a closed boundary.
    velocity += DT * (5.5F * force - 3.0F * normal * displacement -
                       1.6F * velocity);
    velocity = std::clamp(velocity, -3.0F, 3.0F);
    displacement += DT * velocity;
    if (std::fabs(displacement) > 1.1F) {
        displacement = std::copysign(1.1F, displacement);
        crestVelocity += std::fabs(velocity) * 0.7F;
        velocity *= -0.35F;
    }
    crestVelocity += DT * (2.0F * velocity - 28.0F * crest -
                            2.4F * crestVelocity);
    crestVelocity = std::clamp(crestVelocity, -3.0F, 3.0F);
    crest = std::clamp(crest + DT * crestVelocity, -0.45F, 0.45F);
}

void WaterSlosh::Step(const WaterSloshMotion& motion) {
    const float normal = std::isfinite(motion.normalG)
                             ? std::clamp(std::fabs(motion.normalG), 0.35F, 1.5F)
                             : 1.0F;
    m_x.Step(BoundedForce(motion.downXG), normal);
    m_y.Step(BoundedForce(motion.downYG), normal);
    m_normalG = normal;
    m_foam *= 0.992F; // ~0.69 s half-life, independent of render/sample cadence
    if (m_foam < 0.002F) { m_foam = 0.0F; }
}

float WaterSlosh::Mode::Profile(const float position) const {
    const float shape = 1.0F + displacement * position +
                        crest * (position * position - MOMENT_2);
    const float normalization = 1.0F + displacement * displacement * MOMENT_2 +
        crest * crest * (MOMENT_4 - MOMENT_2 * MOMENT_2);
    return shape * shape / normalization;
}

inline float WaterSlosh::SampleDepth(const float x, const float y) const {
    // Private callers supply normalized, already bounded physical coordinates.
    return REST_DEPTH * m_x.Profile(x) * m_y.Profile(y);
}

WaterSloshMetrics WaterSlosh::Measure() const {
    WaterSloshMetrics result;
    result.minimumDepth = SampleDepth(-1.0F, -1.0F);
    // On-demand diagnostics only; no 16x16 representation is retained or stepped.
    for (uint16_t i = 0U; i < CELL_COUNT; ++i) {
        const float depth = SampleDepth(float(i % RESOLUTION) * (1.0F / 7.5F) - 1.0F,
                                        float(i / RESOLUTION) * (1.0F / 7.5F) - 1.0F);
        result.volume += depth;
        result.centerX += depth * float(i % RESOLUTION);
        result.centerY += depth * float(i / RESOLUTION);
        const float departure = depth - REST_DEPTH;
        result.surfaceVariance += departure * departure;
        result.minimumDepth = std::min(result.minimumDepth, depth);
        result.maximumDepth = std::max(result.maximumDepth, depth);
    }
    result.speedSquared = m_x.velocity * m_x.velocity + m_y.velocity * m_y.velocity +
        m_x.crestVelocity * m_x.crestVelocity + m_y.crestVelocity * m_y.crestVelocity;
    result.centerX /= result.volume;
    result.centerY /= result.volume;
    result.normalG = m_normalG;
    result.foam = m_foam;
    result.ambientGain = 0.75F + float(m_light255) * (0.5F / 255.0F);
    return result;
}

inline Rgb WaterSlosh::SurfaceColor(const float x, const float y,
                                   const uint32_t physicalIndex) const {
    const float depth = SampleDepth(x, y);
    const float coverage = std::min(depth * (1.0F / 0.32F), 1.0F);
    const float body = std::min(depth * (1.0F / 1.8F), 1.0F);
    // Light follows displaced mass and velocity, never a free-running clock.
    const float lightGain = 0.75F + float(m_light255) * (0.5F / 255.0F);
    const float crest = std::min(depth *
        (std::fabs(m_x.velocity) + std::fabs(m_y.velocity)) * 0.3F * lightGain, 1.0F);
    // Normal specific force is pressure, not displacement or a clock: lifting
    // accelerates toward bright aqua; unloading deepens the same pool to indigo.
    const float pressure = m_normalG - 1.0F;
    const float red = coverage * (2.0F + 24.0F * crest);
    const float green = coverage * (18.0F + 24.0F * pressure + 45.0F * body + 65.0F * crest);
    const float blue = coverage * (45.0F + 12.0F * pressure + 70.0F * body + 35.0F * crest);
    // A stable physical-index pattern has no buffer, RNG or independent phase.
    // Its density and opacity fall with the same bounded foam reservoir.
    const uint32_t fleck = ((physicalIndex * 73U) ^ (physicalIndex >> 2U)) & 31U;
    const float visibleFoam = m_foam * 0.6F;
    const float foam = fleck < uint32_t(visibleFoam * 16.0F)
        ? visibleFoam * coverage : 0.0F;
    return {Channel(red + foam * (120.0F - red)),
            Channel(green + foam * (155.0F - green)),
            Channel(blue + foam * (175.0F - blue))};
}

BoardSquareLeds WaterSlosh::ComposeSquare(const uint8_t physicalSquare) const {
    if (physicalSquare >= 64U) { return {}; }
    const uint16_t base = physicalSquare * 4U;
    return {ComposePixel(base), ComposePixel(base + 1U),
            ComposePixel(base + 2U), ComposePixel(base + 3U)};
}

Rgb WaterSlosh::ComposeStatusPixel(const uint8_t physicalIndex) const {
    return ComposePixel(256U + physicalIndex);
}

uint16_t WaterSlosh::RiderScale256(const uint8_t physicalSquare) const {
    if (physicalSquare >= 64U) { return 256U; }
    const float depth = SampleDepth(float(physicalSquare % 8U) * (2.0F / 7.0F) - 1.0F,
                                   float(physicalSquare / 8U) * (2.0F / 7.0F) - 1.0F);
    return 160U + static_cast<uint16_t>(96.0F * std::min(depth / (2.0F * REST_DEPTH), 1.0F));
}

Rgb WaterSlosh::ComposePixel(const uint32_t physicalIndex) const {
    if (physicalIndex >= 288U) { return {}; }
    int32_t x;
    int32_t y;
    if (physicalIndex < 256U) {
        x = ((physicalIndex / 4U) % 8U) * 20U + 5U;
        y = (physicalIndex / 32U) * 20U + 5U;
        const int32_t offset = (physicalIndex % 4U) < 2U ? -7 : 7;
        if ((physicalIndex & 1U) == 0U) { x += offset; }
        else { y += offset; }
    } else {
        const int32_t position = (physicalIndex % 8U) * 20U + 5U;
        // Clockwise dedicated ring: top left→right, right top→bottom,
        // bottom right→left, left bottom→top. Continuous with outward slots.
        switch ((physicalIndex - 256U) / 8U) {
        case 0U: x = position; y = 0; break;
        case 1U: x = 150; y = position; break;
        case 2U: x = 150 - position; y = 150; break;
        default: x = 0; y = 150 - position; break;
        }
    }
    // Position arithmetic and boundary clamping stay integer; convert only
    // once at the modal evaluation boundary (coordinates in [-1,1]).
    return SurfaceColor(float(std::clamp<int32_t>(x, 0, 150)) * (1.0F / 75.0F) - 1.0F,
                        float(std::clamp<int32_t>(y, 0, 150)) * (1.0F / 75.0F) - 1.0F,
                        physicalIndex);
}

void WaterSlosh::Compose(BoardLedFrame& frame,
                         const uint8_t opacity255) const {
    const uint16_t scale256 = (uint16_t(opacity255) * 256U + 127U) / 255U;
    for (uint8_t index = 0U; index < 64U; ++index) {
        const auto source = ComposeSquare(index);
        frame[index] = {ScaleRgbSaturatingBy256(source.left, scale256),
                        ScaleRgbSaturatingBy256(source.top, scale256),
                        ScaleRgbSaturatingBy256(source.right, scale256),
                        ScaleRgbSaturatingBy256(source.bottom, scale256)};
    }
}

} // namespace modules
