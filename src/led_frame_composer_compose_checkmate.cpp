#include "modules/led_frame_composer_internal.hpp"

#include "modules/animation_timing.hpp"
#include "runtime/square_mapping.hpp"

namespace modules::led_frame_composer {
namespace internal {

namespace {

struct CheckmateFrameStyle {
    uint8_t losingKingPhysicalSquare = 255U;
    uint64_t contributorPhysicalMask = 0ULL;
    modules::Rgb winnerRgb = modules::RGB_BLACK;
    modules::Rgb loserRgb = modules::RGB_BLACK;
    uint16_t fadeScale256 = 256U;
};

uint64_t BuildWinnerPhysicalMask(
    const runtime::InteractionModelSnapshot& snapshot,
    const modules::LitchMaxEngine::PieceSide winnerSide) {
    uint64_t mask = 0ULL;
    if (!snapshot.board.enginePiecesValid) {
        return mask;
    }
    for (uint8_t logicalSquare = 0U; logicalSquare < 64U; ++logicalSquare) {
        const char piece = snapshot.board.enginePieces[logicalSquare];
        const bool isWhite = piece >= 'A' && piece <= 'Z';
        const bool isBlack = piece >= 'a' && piece <= 'z';
        const bool belongsToWinner =
            (winnerSide == modules::LitchMaxEngine::PieceSide::White && isWhite) ||
            (winnerSide == modules::LitchMaxEngine::PieceSide::Black && isBlack);
        if (belongsToWinner) {
            mask |= 1ULL << logicalSquare;
        }
    }
    return mask;
}

uint32_t BuildAnimationSeed(const ComposeContext& ctx,
                            const uint64_t contributorMask,
                            const uint64_t winnerMask,
                            const uint8_t kingSquare) {
    uint32_t seed = 0x6B8B4567U;
    seed ^= static_cast<uint32_t>(contributorMask);
    seed ^= static_cast<uint32_t>(contributorMask >> 32U) * 0x9E3779B9U;
    seed ^= static_cast<uint32_t>(winnerMask) * 0x85EBCA6BU;
    seed ^= static_cast<uint32_t>(winnerMask >> 32U) * 0xC2B2AE35U;
    seed ^= static_cast<uint32_t>(kingSquare) << 24U;
    seed ^= static_cast<uint32_t>(ctx.p1Wheel) << 8U;
    seed ^= static_cast<uint32_t>(ctx.p2Wheel) << 16U;
    return (seed == 0U) ? 1U : seed;
}

uint16_t CheckmateFadeScale256(const uint32_t elapsedMs) {
    static constexpr uint32_t FADE_MS = 900U;
    static constexpr uint16_t TARGET_SCALE256 = 102U;
    const uint32_t clamped = (elapsedMs > FADE_MS) ? FADE_MS : elapsedMs;
    const uint16_t dimSpan = 256U - TARGET_SCALE256;
    const uint16_t dimStep = static_cast<uint16_t>(
        (static_cast<uint32_t>(dimSpan) * clamped) / FADE_MS);
    return static_cast<uint16_t>(256U - dimStep);
}

uint64_t BuildContributorsPhysicalMask(
    const runtime::InteractionModelSnapshot& snapshot) {
    uint64_t logicalMask = 0ULL;
    (void)TryComputeCheckmateContributorsMask(snapshot, logicalMask);
    uint64_t physicalMask = 0ULL;
    for (uint8_t logicalSquare = 0U; logicalSquare < 64U; ++logicalSquare) {
        if (((logicalMask >> logicalSquare) & 1ULL) == 0ULL) {
            continue;
        }
        physicalMask |= 1ULL << logicalSquare;
    }
    return physicalMask;
}

void ApplyCheckmateBaseFrame(
    const ComposeContext& ctx,
    const CheckmateFrameStyle& style,
    modules::BoardLedFrame& ledFrame,
    std::array<modules::Rgb, 64>& gridFrame) {
    for (uint8_t square = 0U; square < 64U; ++square) {

        gridFrame[square] = ScaleRgbSaturating(
            gridFrame[square], style.fadeScale256);
        ledFrame[square].left = gridFrame[square];
        ledFrame[square].right = gridFrame[square];
        const uint8_t logicalSquare = square;
        const bool occupied =
            (ctx.physicalOcc64 & (1ULL << logicalSquare)) != 0ULL;
        if (!occupied) {
            modules::SetPiecePair(ledFrame[square], modules::RGB_BLACK);
        } else if (square == style.losingKingPhysicalSquare) {
            modules::SetPiecePair(ledFrame[square], style.loserRgb);
        } else if (((style.contributorPhysicalMask >> square) & 1ULL) != 0ULL) {
            modules::SetPiecePair(ledFrame[square], style.winnerRgb);
        } else {
            ledFrame[square].top = ScaleRgbSaturating(
                ledFrame[square].top, style.fadeScale256);
            ledFrame[square].bottom = ScaleRgbSaturating(
                ledFrame[square].bottom, style.fadeScale256);
        }
    }
}

void ApplyCheckmateAnchorPulse(
    const CheckmateFrameStyle& style,
    const uint32_t elapsedMs,
    modules::BoardLedFrame& ledFrame,
    std::array<modules::Rgb, 64>& gridFrame) {
    const uint8_t pulse = modules::animation::SampleValue(
        {elapsedMs}, modules::animation::LED_FRAME_TOKENS.checkmatePulse);
    const uint8_t contributorAlpha = static_cast<uint8_t>(
        36U + (static_cast<uint16_t>(pulse) * 92U) / 255U);
    const uint8_t kingAlpha = static_cast<uint8_t>(
        64U + (static_cast<uint16_t>(pulse) * 112U) / 255U);

    for (uint8_t square = 0U; square < 64U; ++square) {
        const bool isKing = square == style.losingKingPhysicalSquare;
        const bool isContributor =
            ((style.contributorPhysicalMask >> square) & 1ULL) != 0ULL;
        if (!isKing && !isContributor) {
            continue;
        }
        const modules::Rgb accent = isKing ? style.loserRgb : style.winnerRgb;
        const uint8_t alpha = isKing ? kingAlpha : contributorAlpha;
        const modules::Rgb mixed = modules::AlphaBlendRgb(
            gridFrame[square], accent, alpha);
        gridFrame[square] = mixed;
        ledFrame[square].left = mixed;
        ledFrame[square].right = mixed;
    }
}

} // namespace

void ApplyCheckmateCelebration(const ComposeContext& ctx,
                               modules::BoardLedFrame& outLedFrame,
                               std::array<modules::Rgb, 64>& outGridFrame) {
    const auto& in = *ctx.in;
    const auto& snapshot = ctx.Snapshot();
    const auto& game = snapshot.game;

    const bool orthodoxMate =
        ctx.renderIntent.celebration == CelebrationIntent::Checkmate &&
        game.inCheckmate;
    const bool variantVictory =
        ctx.renderIntent.celebration == CelebrationIntent::VariantVictory &&
        game.variantOutcome.IsTerminal();
    const bool resignationVictory =
        variantVictory && game.variantOutcome.reason == modules::VariantOutcomeReason::Resigned;
    const bool clockVictory =
        ctx.renderIntent.celebration == CelebrationIntent::ClockVictory &&
        game.clockWinner != modules::GameWinner::None;
    const bool preview = ctx.boardStylePreview.kind == modules::BoardStylePreviewKind::Checkmate;
    if (!orthodoxMate && !variantVictory && !clockVictory && !preview) {
        return;
    }

    const uint32_t elapsedMs = preview ? ctx.boardStylePreview.elapsedMs
                                       : ctx.renderIntent.celebrationElapsedMs;
    const auto animationStyle = preview ? ctx.boardStylePreview.checkmateStyle
                                       : ctx.checkmateAnimationStyle;

    const modules::GameWinner recordedWinner = clockVictory
        ? game.clockWinner : game.variantOutcome.winner;
    const modules::LitchMaxEngine::PieceSide winnerSide = (variantVictory || clockVictory)
        ? (recordedWinner == modules::GameWinner::White
               ? modules::LitchMaxEngine::PieceSide::White
               : modules::LitchMaxEngine::PieceSide::Black)
        : (preview && !orthodoxMate
               ? (ctx.boardStylePreview.white ? modules::LitchMaxEngine::PieceSide::White
                                               : modules::LitchMaxEngine::PieceSide::Black)
               : (game.sideToMove == modules::SideToMove::White
                      ? modules::LitchMaxEngine::PieceSide::Black
                      : modules::LitchMaxEngine::PieceSide::White));
    const modules::LitchMaxEngine::PieceSide loserSide =
        winnerSide == modules::LitchMaxEngine::PieceSide::White
            ? modules::LitchMaxEngine::PieceSide::Black
            : modules::LitchMaxEngine::PieceSide::White;

    uint8_t losingKingPhysicalSq = 255U;
    if (preview && !orthodoxMate && !variantVictory && !clockVictory) {
        losingKingPhysicalSq = ctx.boardStylePreview.anchorSquare;
    } else if ((clockVictory || resignationVictory) && snapshot.board.enginePiecesValid) {
        const char losingKing = loserSide == modules::LitchMaxEngine::PieceSide::White
            ? 'K' : 'k';
        for (uint8_t square = 0U; square < 64U; ++square) {
            if (snapshot.board.enginePieces[square] == losingKing) {
                losingKingPhysicalSq = square;
                break;
            }
        }
    } else if (orthodoxMate && game.threatenedKingSquareValid && game.threatenedKingSquare < 64U) {
        losingKingPhysicalSq = game.threatenedKingSquare;
    }

    const uint64_t contributorsPhysicalMask64 =
        orthodoxMate ? BuildContributorsPhysicalMask(snapshot) : 0ULL;
    const uint64_t winnerPhysicalMask = BuildWinnerPhysicalMask(
        snapshot, winnerSide) & (preview ? ctx.physicalOcc64 : ~0ULL);
    const bool duckKingCaptureFireLob =
        variantVictory && game.variant == modules::GameVariant::Duck &&
        game.variantOutcome.reason == modules::VariantOutcomeReason::KingCaptured &&
        animationStyle == modules::CheckmateAnimationStyle::FireLob;
    const auto& render = snapshot.render;
    const bool whiteWon = winnerSide == modules::LitchMaxEngine::PieceSide::White;
    const auto winnerHistorySide = whiteWon
        ? modules::PersistedSide::White : modules::PersistedSide::Black;
    const uint8_t captureFrom = whiteWon
        ? render.whiteLastMoveFromSquare : render.blackLastMoveFromSquare;
    const uint8_t captureTo = whiteWon
        ? render.whiteLastMoveToSquare : render.blackLastMoveToSquare;
    const bool haveCaptureRoute = duckKingCaptureFireLob &&
        render.lastMoveMostRecentKnown && render.lastMoveMostRecentSide == winnerHistorySide &&
        (whiteWon ? render.whiteLastMoveValid : render.blackLastMoveValid) &&
        captureFrom < 64U && captureTo < 64U && captureFrom != captureTo &&
        (winnerPhysicalMask & (1ULL << captureTo)) != 0ULL;
    const uint64_t captureAccent = haveCaptureRoute ? 1ULL << captureTo : 0ULL;
    const CheckmateFrameStyle frameStyle{
        duckKingCaptureFireLob ? uint8_t{255U} : losingKingPhysicalSq,
        contributorsPhysicalMask64 | captureAccent,
        PieceRgbForPieceSide(ctx, winnerSide),
        PieceRgbForPieceSide(ctx, loserSide),
        ctx.minimalAnimations
            ? static_cast<uint16_t>(102U)
            : CheckmateFadeScale256(elapsedMs),
    };
    ApplyCheckmateBaseFrame(ctx, frameStyle, outLedFrame, outGridFrame);

    const CheckmateBoardAnimationInputs animationInputs{
        animationStyle,
        elapsedMs,
        BuildAnimationSeed(
            ctx,
            contributorsPhysicalMask64,
            winnerPhysicalMask,
            losingKingPhysicalSq),
        contributorsPhysicalMask64,
        winnerPhysicalMask,
        losingKingPhysicalSq,
        frameStyle.winnerRgb,
        duckKingCaptureFireLob,
        haveCaptureRoute ? captureFrom : uint8_t{255U},
        haveCaptureRoute ? captureTo : uint8_t{255U},
    };
    if (!ctx.minimalAnimations) {
        ApplyCheckmateBoardAnimation(
            animationInputs, outLedFrame, outGridFrame);
    }

    if (ctx.minimalAnimations ||
        animationStyle == modules::CheckmateAnimationStyle::FireLob) {
        return;
    }

    ApplyCheckmateAnchorPulse(
        frameStyle, elapsedMs, outLedFrame, outGridFrame);
}

} // namespace internal
} // namespace modules::led_frame_composer
