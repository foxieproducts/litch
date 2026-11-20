#pragma once

#include <cstdint>

namespace modules::board_ripple {

struct Recipe {
    uint32_t durationMs = 1U;
    uint32_t travelMs = 1U;
    uint32_t attackMs = 1U;
    uint32_t fadeStartMs = 0U;
    uint16_t maxRadiusQ8 = 0U;
    uint16_t halfWidthQ8 = 1U;
    uint8_t maxAlpha = 0U;
};

inline constexpr uint32_t PLACEMENT_DURATION_MS = 560U;
inline constexpr Recipe PLACEMENT_RECIPE{
    PLACEMENT_DURATION_MS,
    460U,
    80U,
    280U,
    640U,
    280U,
    62U,
};

inline uint16_t OctileDistanceQ8(const uint8_t fromSquare,
                                 const uint8_t toSquare) {
    const int fromRank = static_cast<int>(fromSquare / 8U);
    const int fromFile = static_cast<int>(fromSquare % 8U);
    const int toRank = static_cast<int>(toSquare / 8U);
    const int toFile = static_cast<int>(toSquare % 8U);
    const uint16_t rankDistance = static_cast<uint16_t>(
        (fromRank > toRank) ? fromRank - toRank : toRank - fromRank);
    const uint16_t fileDistance = static_cast<uint16_t>(
        (fromFile > toFile) ? fromFile - toFile : toFile - fromFile);
    const uint16_t maximum =
        (rankDistance > fileDistance) ? rankDistance : fileDistance;
    const uint16_t minimum =
        (rankDistance > fileDistance) ? fileDistance : rankDistance;
    // max + 0.414 * min approximates Euclidean distance without sqrt/float.
    return static_cast<uint16_t>(maximum * 256U + minimum * 106U);
}

inline uint8_t SampleAlpha(const uint8_t centerSquare,
                           const uint8_t square,
                           const uint32_t elapsedMs,
                           const Recipe& recipe) {
    if (centerSquare >= 64U || square >= 64U ||
        elapsedMs >= recipe.durationMs || recipe.durationMs == 0U ||
        recipe.travelMs == 0U || recipe.halfWidthQ8 == 0U ||
        recipe.maxAlpha == 0U) {
        return 0U;
    }

    const uint32_t travelElapsed =
        elapsedMs > recipe.travelMs ? recipe.travelMs : elapsedMs;
    const uint16_t radiusQ8 = static_cast<uint16_t>(
        (travelElapsed * recipe.maxRadiusQ8 + recipe.travelMs / 2U) /
        recipe.travelMs);
    const uint16_t distanceQ8 = OctileDistanceQ8(centerSquare, square);
    const uint16_t differenceQ8 =
        (distanceQ8 > radiusQ8)
            ? static_cast<uint16_t>(distanceQ8 - radiusQ8)
            : static_cast<uint16_t>(radiusQ8 - distanceQ8);
    if (differenceQ8 >= recipe.halfWidthQ8) {
        return 0U;
    }

    const uint16_t ringScale = static_cast<uint16_t>(
        ((recipe.halfWidthQ8 - differenceQ8) * 255U +
         recipe.halfWidthQ8 / 2U) /
        recipe.halfWidthQ8);
    const uint16_t attack = static_cast<uint16_t>(
        (recipe.attackMs == 0U || elapsedMs >= recipe.attackMs)
            ? 255U
            : (elapsedMs * 255U) / recipe.attackMs);
    uint16_t fade = 255U;
    if (elapsedMs > recipe.fadeStartMs) {
        const uint32_t fadeDuration =
            recipe.durationMs - recipe.fadeStartMs;
        fade = static_cast<uint16_t>(
            ((recipe.durationMs - elapsedMs) * 255U) /
            (fadeDuration == 0U ? 1U : fadeDuration));
    }
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(ringScale) * attack * fade *
             recipe.maxAlpha +
         8290687U) /
        16581375U);
}

} // namespace modules::board_ripple
