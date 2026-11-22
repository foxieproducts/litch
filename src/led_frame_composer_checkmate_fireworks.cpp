#include <cstdint>

#include "modules/led_frame_composer_internal.hpp"
#include "modules/ui_color_wheel.hpp"

namespace modules::led_frame_composer::internal {
namespace {

constexpr uint32_t FIREWORK_WINDOW_MS = 1000U;
constexpr uint8_t PARTICLE_COUNT = 8U;
constexpr uint8_t BASE_PARTICLE_ALPHA = 120U;

struct FireworkSpec {
    uint8_t centerSquare = 0U;
    uint8_t radius = 1U;
    uint8_t hue = 0U;
    uint32_t startOffsetMs = 0U;
    uint32_t durationMs = 1050U;
};

struct FireworkActivity {
    uint32_t localMs = 0U;
    uint8_t fadeAlpha = 0U;
};

struct OverlayGeometry {
    int rankDelta = 0;
    int fileDelta = 0;
    int rankDistance = 0;
    int fileDistance = 0;
    uint8_t maxDistance = 0U;
};

struct ColorAccumulator {
    uint32_t red = 0U;
    uint32_t green = 0U;
    uint32_t blue = 0U;
    uint32_t alpha = 0U;
};

uint32_t XorShift32(uint32_t& state) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

int AbsInt(const int value) {
    return (value < 0) ? -value : value;
}

int SignInt(const int value) {
    return (value > 0) ? 1 : ((value < 0) ? -1 : 0);
}

FireworkSpec BuildFireworkSpec(const uint32_t baseSeed, const uint32_t epoch) {
    uint32_t state = baseSeed ^ (epoch * 0x9E3779B9U) ^ 0x6D2B79F5U;
    if (state == 0U) {
        state = 1U;
    }

    FireworkSpec spec;
    spec.centerSquare = static_cast<uint8_t>(XorShift32(state) % 64U);
    spec.radius = static_cast<uint8_t>(1U + (XorShift32(state) % 2U));
    spec.durationMs = 1050U + (XorShift32(state) % 901U);
    spec.startOffsetMs = XorShift32(state) % FIREWORK_WINDOW_MS;
    spec.hue = static_cast<uint8_t>(XorShift32(state) & 0xFFU);
    return spec;
}

bool TryGetActivity(const FireworkSpec& spec,
                    const uint32_t epoch,
                    const uint32_t elapsedMs,
                    FireworkActivity& outActivity) {
    outActivity = FireworkActivity{};
    const uint32_t epochStartMs = epoch * FIREWORK_WINDOW_MS;
    if (elapsedMs < epochStartMs) {
        return false;
    }
    const uint32_t epochMs = elapsedMs - epochStartMs;
    if (epochMs < spec.startOffsetMs) {
        return false;
    }
    const uint32_t localMs = epochMs - spec.startOffsetMs;
    if (localMs >= spec.durationMs) {
        return false;
    }

    outActivity.localMs = localMs;
    const uint32_t holdMs = (spec.durationMs * 2U) / 3U;
    if (localMs <= holdMs) {
        outActivity.fadeAlpha = 255U;
        return true;
    }
    const uint32_t fadeMs = spec.durationMs - holdMs;
    const uint32_t fadeElapsed = localMs - holdMs;
    const uint32_t remainingMs =
        (fadeMs > fadeElapsed) ? (fadeMs - fadeElapsed) : 0U;
    outActivity.fadeAlpha = static_cast<uint8_t>(
        (remainingMs * 255U + fadeMs / 2U) / ((fadeMs == 0U) ? 1U : fadeMs));
    return true;
}

bool BuildOverlayGeometry(const FireworkSpec& spec,
                          const uint8_t square,
                          const uint32_t localMs,
                          OverlayGeometry& outGeometry) {
    if (square >= 64U || spec.centerSquare >= 64U ||
        localMs >= spec.durationMs) {
        return false;
    }
    const int centerRank = static_cast<int>(spec.centerSquare / 8U);
    const int centerFile = static_cast<int>(spec.centerSquare % 8U);
    const int squareRank = static_cast<int>(square / 8U);
    const int squareFile = static_cast<int>(square % 8U);
    outGeometry.rankDelta = squareRank - centerRank;
    outGeometry.fileDelta = squareFile - centerFile;
    outGeometry.rankDistance = AbsInt(outGeometry.rankDelta);
    outGeometry.fileDistance = AbsInt(outGeometry.fileDelta);
    outGeometry.maxDistance = static_cast<uint8_t>(spec.radius + 2U);
    const int distanceSquared =
        outGeometry.rankDistance * outGeometry.rankDistance +
        outGeometry.fileDistance * outGeometry.fileDistance;
    const int maxDistanceSquared =
        static_cast<int>(outGeometry.maxDistance) * outGeometry.maxDistance;
    return distanceSquared <= maxDistanceSquared;
}

bool TryGetStepFromCenter(const int directionFile,
                          const int directionRank,
                          const OverlayGeometry& geometry,
                          int& outStep) {
    if (directionFile == 0) {
        if (geometry.fileDelta != 0 ||
            (SignInt(geometry.rankDelta) != SignInt(directionRank) &&
             geometry.rankDelta != 0)) {
            return false;
        }
        outStep = geometry.rankDistance;
        return true;
    }
    if (directionRank == 0) {
        if (geometry.rankDelta != 0 ||
            (SignInt(geometry.fileDelta) != SignInt(directionFile) &&
             geometry.fileDelta != 0)) {
            return false;
        }
        outStep = geometry.fileDistance;
        return true;
    }
    if (geometry.rankDistance != geometry.fileDistance ||
        (SignInt(geometry.rankDelta) != SignInt(directionRank) &&
         geometry.rankDelta != 0) ||
        (SignInt(geometry.fileDelta) != SignInt(directionFile) &&
         geometry.fileDelta != 0)) {
        return false;
    }
    outStep = geometry.rankDistance;
    return true;
}

void AccumulateParticle(const FireworkSpec& spec,
                        const FireworkActivity& activity,
                        const OverlayGeometry& geometry,
                        const uint8_t particleIndex,
                        ColorAccumulator& accumulator) {
    static constexpr int8_t DIRECTION_FILE[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    static constexpr int8_t DIRECTION_RANK[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    uint32_t state =
        (static_cast<uint32_t>(spec.centerSquare) << 24U) ^
        (static_cast<uint32_t>(spec.radius) << 16U) ^
        (spec.durationMs << 1U) ^ static_cast<uint32_t>(spec.hue) ^
        (static_cast<uint32_t>(particleIndex) * 0x9E3779B9U) ^ 0xA5A5A5A5U;
    if (state == 0U) {
        state = 1U;
    }

    const uint8_t directionIndex = static_cast<uint8_t>(XorShift32(state) % 8U);
    const uint32_t stepMs = 60U + (XorShift32(state) % 81U);
    const uint32_t startDelayMs = XorShift32(state) % 320U;
    const uint8_t trailLength = static_cast<uint8_t>(2U + (XorShift32(state) % 4U));
    const uint8_t hue = static_cast<uint8_t>(
        spec.hue + (XorShift32(state) & 0xFFU) + particleIndex * 29U);
    const uint8_t alphaJitter = static_cast<uint8_t>(
        170U + (XorShift32(state) % 86U));
    if (activity.localMs < startDelayMs) {
        return;
    }

    const uint32_t particleMs = activity.localMs - startDelayMs;
    int stepFromCenter = 0;
    if (!TryGetStepFromCenter(DIRECTION_FILE[directionIndex],
                              DIRECTION_RANK[directionIndex],
                              geometry,
                              stepFromCenter)) {
        return;
    }

    // Keep particle motion in fixed-point. The former whole-step head shared a
    // second global expansion cap, so independent rays jumped together and
    // then froze. Q8 interpolation lets energy crossfade between adjacent
    // squares while retaining a deterministic, allocation-free renderer.
    // Let the head continue beyond the visible board. Its trailing energy then
    // exits naturally instead of freezing at the radius until the epoch fade.
    const uint32_t headQ8 = (particleMs << 8U) / stepMs;
    const uint32_t squareQ8 = static_cast<uint32_t>(stepFromCenter) << 8U;

    uint16_t motionScale = 0U;
    uint32_t tailDistanceQ8 = 0U;
    if (squareQ8 > headQ8) {
        const uint32_t aheadQ8 = squareQ8 - headQ8;
        if (aheadQ8 >= 256U) {
            return;
        }
        motionScale = static_cast<uint16_t>(
            ((256U - aheadQ8) * 255U + 128U) / 256U);
    } else {
        tailDistanceQ8 = headQ8 - squareQ8;
        const uint32_t tailSpanQ8 =
            static_cast<uint32_t>(trailLength + 1U) << 8U;
        if (tailDistanceQ8 >= tailSpanQ8) {
            return;
        }
        motionScale = static_cast<uint16_t>(
            ((tailSpanQ8 - tailDistanceQ8) * 255U + tailSpanQ8 / 2U) /
            tailSpanQ8);
    }

    const uint32_t attackMs = stepMs * 2U;
    const uint16_t attackScale = static_cast<uint16_t>(
        (particleMs >= attackMs)
            ? 255U
            : ((particleMs * 255U + attackMs / 2U) / attackMs));
    if (motionScale == 0U || attackScale == 0U) {
        return;
    }

    uint32_t alpha = BASE_PARTICLE_ALPHA;
    alpha = (alpha * motionScale + 127U) / 255U;
    alpha = (alpha * attackScale + 127U) / 255U;
    alpha = (alpha * alphaJitter + 127U) / 255U;
    if (alpha == 0U) {
        return;
    }

    const uint8_t wheel = static_cast<uint8_t>(
        hue + stepFromCenter * 19);
    const modules::Rgb color = modules::ui_color_wheel::WheelToRgb(wheel);
    accumulator.red += static_cast<uint32_t>(color.r) * alpha;
    accumulator.green += static_cast<uint32_t>(color.g) * alpha;
    accumulator.blue += static_cast<uint32_t>(color.b) * alpha;
    accumulator.alpha += static_cast<uint8_t>(alpha);
}

bool TryGetOverlay(const FireworkSpec& spec,
                   const FireworkActivity& activity,
                   const uint8_t square,
                   modules::Rgb& outColor,
                   uint8_t& outAlpha) {
    outColor = modules::RGB_BLACK;
    outAlpha = 0U;
    OverlayGeometry geometry;
    if (!BuildOverlayGeometry(spec, square, activity.localMs, geometry)) {
        return false;
    }

    ColorAccumulator accumulator;
    for (uint8_t particle = 0U; particle < PARTICLE_COUNT; ++particle) {
        AccumulateParticle(spec, activity, geometry, particle, accumulator);
    }
    if (accumulator.alpha == 0U) {
        return false;
    }
    const uint32_t denominator = accumulator.alpha;
    outColor = modules::Rgb{
        static_cast<uint8_t>((accumulator.red + denominator / 2U) / denominator),
        static_cast<uint8_t>((accumulator.green + denominator / 2U) / denominator),
        static_cast<uint8_t>((accumulator.blue + denominator / 2U) / denominator),
    };
    outAlpha = static_cast<uint8_t>(
        (accumulator.alpha > 255U) ? 255U : accumulator.alpha);
    return true;
}

void ApplyFireworkEpoch(
    const CheckmateBoardAnimationInputs& inputs,
    const uint32_t epoch,
    modules::BoardLedFrame& ledFrame,
    std::array<modules::Rgb, 64>& gridFrame) {
    const FireworkSpec spec = BuildFireworkSpec(inputs.seed, epoch);
    FireworkActivity activity;
    if (!TryGetActivity(spec, epoch, inputs.elapsedMs, activity) ||
        activity.fadeAlpha == 0U) {
        return;
    }

    for (uint8_t square = 0U; square < 64U; ++square) {
        modules::Rgb overlay = modules::RGB_BLACK;
        uint8_t overlayAlpha = 0U;
        if (!TryGetOverlay(spec, activity, square, overlay, overlayAlpha)) {
            continue;
        }
        const uint8_t alpha = static_cast<uint8_t>(
            (static_cast<uint16_t>(overlayAlpha) * activity.fadeAlpha + 127U) /
            255U);
        if (alpha == 0U) {
            continue;
        }
        const modules::Rgb mixed = modules::AlphaBlendRgb(
            gridFrame[square], overlay, alpha);
        gridFrame[square] = mixed;
        ledFrame[square].left = mixed;
        ledFrame[square].right = mixed;
    }
}

} // namespace

void ApplyFullCheckmateFireworks(
    const CheckmateBoardAnimationInputs& inputs,
    modules::BoardLedFrame& outLedFrame,
    std::array<modules::Rgb, 64>& outGridFrame) {
    const uint32_t epoch = inputs.elapsedMs / FIREWORK_WINDOW_MS;
    ApplyFireworkEpoch(inputs, epoch, outLedFrame, outGridFrame);
    if (epoch > 0U) {
        ApplyFireworkEpoch(inputs, epoch - 1U, outLedFrame, outGridFrame);
    }
    // A firework can start 999 ms into its epoch and live for 1950 ms, so a
    // second prior epoch may still be fading. Preserve it instead of dropping
    // a visible burst at an arbitrary one-second boundary.
    if (epoch > 1U) {
        ApplyFireworkEpoch(inputs, epoch - 2U, outLedFrame, outGridFrame);
    }
}

} // namespace modules::led_frame_composer::internal
