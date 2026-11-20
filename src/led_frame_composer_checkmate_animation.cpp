#include <array>
#include <cstdint>

#include "modules/board_ripple.hpp"
#include "modules/led_frame_composer_internal.hpp"

namespace modules::led_frame_composer::internal {
namespace {

constexpr uint32_t FIRE_LOB_PERIOD_MS = 2000U;
constexpr uint32_t FIRE_LOB_TRAVEL_MS = 900U;
constexpr uint32_t FIRE_LOB_IMPACT_MS = 260U;
constexpr uint32_t FIRE_LOB_STAGGER_MS = 220U;
constexpr uint32_t RIPPLE_PERIOD_MS = 2200U;
constexpr uint32_t RIPPLE_VISIBLE_MS = 1800U;
constexpr uint32_t RIPPLE_TRAVEL_MS = 1300U;
constexpr uint32_t WINNER_SWEEP_PERIOD_MS = 2400U;
constexpr uint32_t WINNER_SWEEP_TRAVEL_MS = 1500U;
constexpr uint32_t CONSTELLATION_PERIOD_MS = 2400U;

struct FireLobFrame {
    const CheckmateBoardAnimationInputs* inputs = nullptr;
    modules::BoardLedFrame* ledFrame = nullptr;
    std::array<modules::Rgb, 64>* gridFrame = nullptr;
    uint8_t impactSquare = 255U;
};

void ApplySquareGlow(
    const uint8_t physicalSquare,
    const modules::Rgb color,
    const uint8_t alpha,
    modules::BoardLedFrame& ledFrame,
    std::array<modules::Rgb, 64>& gridFrame) {
    if (physicalSquare >= 64U || alpha == 0U) {
        return;
    }

    const modules::Rgb mixed = modules::AlphaBlendRgb(
        gridFrame[physicalSquare], color, alpha);
    gridFrame[physicalSquare] = mixed;
    ledFrame[physicalSquare].left = mixed;
    ledFrame[physicalSquare].right = mixed;
}

bool TryBuildLine(const uint8_t fromSquare,
                  const uint8_t toSquare,
                  std::array<uint8_t, 8>& outLine,
                  uint8_t& outCount) {
    outCount = 0U;
    if (fromSquare >= 64U || toSquare >= 64U || fromSquare == toSquare) {
        return false;
    }

    int file = static_cast<int>(fromSquare % 8U);
    int rank = static_cast<int>(fromSquare / 8U);
    const int targetFile = static_cast<int>(toSquare % 8U);
    const int targetRank = static_cast<int>(toSquare / 8U);
    const int fileDistance = (targetFile > file) ? targetFile - file : file - targetFile;
    const int rankDistance = (targetRank > rank) ? targetRank - rank : rank - targetRank;
    const int fileStep = (file < targetFile) ? 1 : -1;
    const int rankStep = (rank < targetRank) ? 1 : -1;
    int error = fileDistance - rankDistance;

    while (outCount < outLine.size()) {
        outLine[outCount++] = static_cast<uint8_t>(rank * 8 + file);
        if (file == targetFile && rank == targetRank) {
            return outCount >= 2U;
        }
        const int doubledError = error * 2;
        if (doubledError > -rankDistance) {
            error -= rankDistance;
            file += fileStep;
        }
        if (doubledError < fileDistance) {
            error += fileDistance;
            rank += rankStep;
        }
    }
    return false;
}

modules::Rgb FireColor(const uint32_t elapsedMs, const uint8_t salt) {
    static constexpr std::array<modules::Rgb, 3> PALETTE = {
        modules::RGB_YELLOW,
        modules::RGB_ORANGE,
        modules::RGB_RED,
    };
    return PALETTE[((elapsedMs / 80U) + salt) % PALETTE.size()];
}

void ApplyFireLobImpact(const FireLobFrame& frame, const uint8_t salt) {
    const uint8_t kingSquare = frame.impactSquare;
    modules::SetPiecePair((*frame.ledFrame)[kingSquare], modules::RGB_BLACK);

    const modules::Rgb burst = FireColor(frame.inputs->elapsedMs, salt);
    ApplySquareGlow(kingSquare, burst, 230U, *frame.ledFrame, *frame.gridFrame);
    const int kingRank = static_cast<int>(kingSquare / 8U);
    const int kingFile = static_cast<int>(kingSquare % 8U);
    for (int rankOffset = -1; rankOffset <= 1; ++rankOffset) {
        for (int fileOffset = -1; fileOffset <= 1; ++fileOffset) {
            if (rankOffset == 0 && fileOffset == 0) {
                continue;
            }
            const int rank = kingRank + rankOffset;
            const int file = kingFile + fileOffset;
            if (rank < 0 || rank >= 8 || file < 0 || file >= 8) {
                continue;
            }
            ApplySquareGlow(static_cast<uint8_t>(rank * 8 + file),
                            burst,
                            72U,
                            *frame.ledFrame,
                            *frame.gridFrame);
        }
    }
}

void ApplyFireLobTravel(const FireLobFrame& frame,
                        const uint8_t fromSquare,
                        const uint8_t salt,
                        const uint32_t travelMs) {
    std::array<uint8_t, 8> line{};
    uint8_t count = 0U;
    if (!TryBuildLine(fromSquare,
                      frame.impactSquare,
                      line,
                      count)) {
        return;
    }

    const uint32_t segments = count - 1U;
    const uint32_t positionQ8 =
        (travelMs * segments * 256U) / FIRE_LOB_TRAVEL_MS;
    const uint8_t headIndex = static_cast<uint8_t>(
        ((positionQ8 >> 8U) >= segments) ? segments : (positionQ8 >> 8U));
    const uint8_t fraction = static_cast<uint8_t>(positionQ8 & 0xFFU);
    const uint8_t nextIndex =
        (headIndex + 1U < count) ? static_cast<uint8_t>(headIndex + 1U) : headIndex;
    const modules::Rgb fire = FireColor(
        frame.inputs->elapsedMs + travelMs, salt);

    ApplySquareGlow(line[headIndex],
                    fire,
                    static_cast<uint8_t>(((255U - fraction) * 220U + 127U) / 255U),
                    *frame.ledFrame,
                    *frame.gridFrame);
    ApplySquareGlow(line[nextIndex],
                    fire,
                    static_cast<uint8_t>((fraction * 220U + 127U) / 255U),
                    *frame.ledFrame,
                    *frame.gridFrame);
    if (headIndex >= 1U) {
        ApplySquareGlow(line[headIndex - 1U],
                        modules::RGB_ORANGE,
                        110U,
                        *frame.ledFrame,
                        *frame.gridFrame);
    }
    if (headIndex >= 2U) {
        ApplySquareGlow(line[headIndex - 2U],
                        modules::RGB_RED,
                        60U,
                        *frame.ledFrame,
                        *frame.gridFrame);
    }
    auto& source = (*frame.ledFrame)[fromSquare];
    source.top = modules::AlphaBlendRgb(source.top, modules::RGB_ORANGE, 80U);
    source.bottom = modules::AlphaBlendRgb(source.bottom, modules::RGB_ORANGE, 80U);
}

void ApplyDuckCaptureFireLob(
    const CheckmateBoardAnimationInputs& inputs,
    modules::BoardLedFrame& ledFrame,
    std::array<modules::Rgb, 64>& gridFrame) {
    if (inputs.captureFromSquare >= 64U || inputs.captureToSquare >= 64U) {
        ApplyFullCheckmateFireworks(inputs, ledFrame, gridFrame);
        return;
    }

    const uint32_t localMs = inputs.elapsedMs % FIRE_LOB_PERIOD_MS;
    const FireLobFrame frame{&inputs, &ledFrame, &gridFrame, inputs.captureToSquare};
    if (localMs < FIRE_LOB_TRAVEL_MS) {
        ApplyFireLobTravel(frame, inputs.captureFromSquare, inputs.captureFromSquare, localMs);
        return;
    }

    const uint32_t impactMs = localMs - FIRE_LOB_TRAVEL_MS;
    static constexpr modules::board_ripple::Recipe EMBERS{
        610U, 480U, 48U, 270U, 640U, 210U, 194U,
    };
    if (impactMs >= EMBERS.durationMs) return;
    if (impactMs < FIRE_LOB_IMPACT_MS) {
        const uint8_t flare = static_cast<uint8_t>(
            230U * (FIRE_LOB_IMPACT_MS - impactMs) / FIRE_LOB_IMPACT_MS);
        ApplySquareGlow(inputs.captureToSquare, modules::RGB_YELLOW, flare, ledFrame, gridFrame);
    }
    for (uint8_t square = 0U; square < 64U; ++square) {
        ApplySquareGlow(square, modules::RGB_ORANGE,
                        modules::board_ripple::SampleAlpha(
                            inputs.captureToSquare, square, impactMs, EMBERS),
                        ledFrame, gridFrame);
    }
}

void ApplyFireLob(
    const CheckmateBoardAnimationInputs& inputs,
    modules::BoardLedFrame& ledFrame,
    std::array<modules::Rgb, 64>& gridFrame) {
    if (inputs.losingKingPhysicalSquare >= 64U) {
        return;
    }

    const uint64_t lobbers = (inputs.contributorPhysicalMask != 0ULL)
                                  ? inputs.contributorPhysicalMask
                                  : inputs.winnerPhysicalMask;
    const uint32_t localMs = inputs.elapsedMs % FIRE_LOB_PERIOD_MS;
    const FireLobFrame frame{&inputs, &ledFrame, &gridFrame, inputs.losingKingPhysicalSquare};
    uint8_t lobberIndex = 0U;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if (((lobbers >> square) & 1ULL) == 0ULL) {
            continue;
        }
        const uint32_t latestStart =
            FIRE_LOB_PERIOD_MS - FIRE_LOB_TRAVEL_MS - FIRE_LOB_IMPACT_MS;
        const uint32_t startMs =
            (static_cast<uint32_t>(lobberIndex) * FIRE_LOB_STAGGER_MS) %
            (latestStart + 1U);
        const uint8_t salt = static_cast<uint8_t>(square ^ (lobberIndex * 13U));
        if (localMs >= startMs && localMs < startMs + FIRE_LOB_TRAVEL_MS) {
            ApplyFireLobTravel(frame, square, salt, localMs - startMs);
        } else if (localMs >= startMs + FIRE_LOB_TRAVEL_MS &&
                   localMs < startMs + FIRE_LOB_TRAVEL_MS + FIRE_LOB_IMPACT_MS) {
            ApplyFireLobImpact(frame, salt);
        }
        ++lobberIndex;
    }
}

uint8_t TriangleEnvelope(const uint32_t elapsedMs,
                         const uint32_t durationMs,
                         const uint8_t maximum) {
    if (durationMs == 0U || elapsedMs >= durationMs) {
        return 0U;
    }
    const uint32_t half = durationMs / 2U;
    const uint32_t distance =
        (elapsedMs <= half) ? elapsedMs : durationMs - elapsedMs;
    return static_cast<uint8_t>(
        (distance * maximum + half / 2U) / ((half == 0U) ? 1U : half));
}

void ApplyRippleBloom(
    const CheckmateBoardAnimationInputs& inputs,
    modules::BoardLedFrame& ledFrame,
    std::array<modules::Rgb, 64>& gridFrame) {
    const uint32_t localMs = inputs.elapsedMs % RIPPLE_PERIOD_MS;
    if (localMs >= RIPPLE_VISIBLE_MS) {
        return;
    }

    const uint8_t center = inputs.losingKingPhysicalSquare < 64U
                               ? inputs.losingKingPhysicalSquare
                               : static_cast<uint8_t>(inputs.seed % 64U);
    static constexpr modules::board_ripple::Recipe RECIPE{
        RIPPLE_VISIBLE_MS,
        RIPPLE_TRAVEL_MS,
        140U,
        1150U,
        6U << 8U,
        352U,
        148U,
    };

    for (uint8_t square = 0U; square < 64U; ++square) {
        const uint8_t alpha = modules::board_ripple::SampleAlpha(
            center, square, localMs, RECIPE);
        ApplySquareGlow(
            square, inputs.winnerRgb, alpha, ledFrame, gridFrame);
    }
}

void ApplyWinnerSweep(
    const CheckmateBoardAnimationInputs& inputs,
    modules::BoardLedFrame& ledFrame,
    std::array<modules::Rgb, 64>& gridFrame) {
    const uint32_t localMs = inputs.elapsedMs % WINNER_SWEEP_PERIOD_MS;
    if (localMs >= WINNER_SWEEP_TRAVEL_MS) {
        return;
    }
    const uint32_t epoch = inputs.elapsedMs / WINNER_SWEEP_PERIOD_MS;
    const bool reverse = ((inputs.seed ^ epoch) & 1U) != 0U;
    const int32_t pathQ8 = 9 << 8;
    int32_t headQ8 = -256 + static_cast<int32_t>(
        (localMs * pathQ8) / WINNER_SWEEP_TRAVEL_MS);
    if (reverse) {
        headQ8 = (7 << 8) - headQ8;
    }
    static constexpr int32_t HALF_WIDTH_Q8 = 400;
    for (uint8_t square = 0U; square < 64U; ++square) {
        const int32_t squareQ8 = static_cast<int32_t>(square / 8U) << 8U;
        const int32_t signedDistance = squareQ8 - headQ8;
        const uint32_t distanceQ8 = static_cast<uint32_t>(
            signedDistance < 0 ? -signedDistance : signedDistance);
        if (distanceQ8 >= static_cast<uint32_t>(HALF_WIDTH_Q8)) {
            continue;
        }
        const uint8_t band = static_cast<uint8_t>(
            ((HALF_WIDTH_Q8 - distanceQ8) * 255U) / HALF_WIDTH_Q8);
        const uint32_t salt =
            static_cast<uint32_t>(square) * 17U + inputs.seed + epoch * 13U;
        const bool fleck = (salt % 5U) < 3U;
        const uint8_t alpha = static_cast<uint8_t>(
            (static_cast<uint16_t>(band) * (fleck ? 126U : 38U) + 127U) /
            255U);
        const modules::Rgb color = fleck
                                       ? modules::AlphaBlendRgb(
                                             inputs.winnerRgb,
                                             modules::RGB_YELLOW,
                                             80U)
                                       : inputs.winnerRgb;
        ApplySquareGlow(square, color, alpha, ledFrame, gridFrame);
    }
}

void ApplyWinnerConstellation(
    const CheckmateBoardAnimationInputs& inputs,
    modules::BoardLedFrame& ledFrame,
    std::array<modules::Rgb, 64>& gridFrame) {
    const uint32_t localMs = inputs.elapsedMs % CONSTELLATION_PERIOD_MS;
    const uint64_t anchors = inputs.winnerPhysicalMask != 0ULL
                                 ? inputs.winnerPhysicalMask
                                 : inputs.contributorPhysicalMask;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if (((anchors >> square) & 1ULL) == 0ULL) {
            continue;
        }
        const uint32_t phase =
            (static_cast<uint32_t>(square) * 97U + inputs.seed) % 1200U;
        const uint32_t age =
            (localMs + CONSTELLATION_PERIOD_MS - phase) %
            CONSTELLATION_PERIOD_MS;
        const uint8_t alpha = TriangleEnvelope(age, 720U, 112U);
        if (alpha == 0U) {
            continue;
        }
        ApplySquareGlow(square, inputs.winnerRgb, alpha, ledFrame, gridFrame);
        const int rank = static_cast<int>(square / 8U);
        const int file = static_cast<int>(square % 8U);
        static constexpr int8_t DR[4] = {-1, 0, 1, 0};
        static constexpr int8_t DF[4] = {0, 1, 0, -1};
        for (uint8_t i = 0U; i < 4U; ++i) {
            const int neighborRank = rank + DR[i];
            const int neighborFile = file + DF[i];
            if (neighborRank < 0 || neighborRank >= 8 ||
                neighborFile < 0 || neighborFile >= 8) {
                continue;
            }
            ApplySquareGlow(
                static_cast<uint8_t>(neighborRank * 8 + neighborFile),
                inputs.winnerRgb,
                static_cast<uint8_t>(alpha / 3U),
                ledFrame,
                gridFrame);
        }
    }

    if (inputs.losingKingPhysicalSquare < 64U && localMs >= 1500U) {
        const uint8_t kingAlpha =
            TriangleEnvelope(localMs - 1500U, 700U, 132U);
        ApplySquareGlow(inputs.losingKingPhysicalSquare,
                        modules::RGB_WHITE,
                        kingAlpha,
                        ledFrame,
                        gridFrame);
    }
}

} // namespace

void ApplyCheckmateBoardAnimation(
    const CheckmateBoardAnimationInputs& inputs,
    modules::BoardLedFrame& outLedFrame,
    std::array<modules::Rgb, 64>& outGridFrame) {
    switch (inputs.style) {
        case modules::CheckmateAnimationStyle::FireLob:
            if (inputs.duckKingCaptured) {
                ApplyDuckCaptureFireLob(inputs, outLedFrame, outGridFrame);
            } else {
                ApplyFireLob(inputs, outLedFrame, outGridFrame);
            }
            return;
        case modules::CheckmateAnimationStyle::RippleBloom:
            ApplyRippleBloom(inputs, outLedFrame, outGridFrame);
            return;
        case modules::CheckmateAnimationStyle::WinnerSweep:
            ApplyWinnerSweep(inputs, outLedFrame, outGridFrame);
            return;
        case modules::CheckmateAnimationStyle::WinnerConstellation:
            ApplyWinnerConstellation(inputs, outLedFrame, outGridFrame);
            return;
        case modules::CheckmateAnimationStyle::Fireworks:
            break;
    }
    ApplyFullCheckmateFireworks(inputs, outLedFrame, outGridFrame);
}

} // namespace modules::led_frame_composer::internal
