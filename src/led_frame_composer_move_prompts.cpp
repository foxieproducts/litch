#include <algorithm>
#include <array>
#include <cstdlib>

#include "modules/led_frame_composer_internal.hpp"

#include "hardware/led_layout.hpp"
#include "modules/beginner_mode_policy.hpp"
#include "runtime/directed_physical_prompt.hpp"

namespace modules::led_frame_composer {
namespace internal {
namespace {

constexpr uint32_t HINT_PROMPT_PULSE_PERIOD_MS = 900U;
constexpr uint8_t DUCK_HINT_SOURCE_MIN_ALPHA = 80U;
constexpr uint8_t DUCK_HINT_SOURCE_MAX_ALPHA = 224U;
constexpr uint8_t DUCK_HINT_DESTINATION_ALPHA = 224U;
constexpr uint32_t DIRECTED_MIN_TRAVEL_MS = 900U;
constexpr uint32_t DIRECTED_SQUARE_TRAVEL_MS = 480U;
constexpr uint32_t DIRECTED_DESTINATION_DWELL_MS = 240U;
constexpr uint16_t DIRECTED_FLOOR_256 = 96U;

struct DirectedTravel {
    uint8_t source, target, alpha;
    uint32_t elapsed;
    int rankStep, fileStep;
    uint8_t distance, emitterCount;
    bool knight, duck, landingPending;
    std::array<uint8_t, 4U> order;
    modules::Rgb color;
};

bool ReservesChessLandingRails(const ComposeContext& ctx, const uint8_t square) {
    return ctx.hintDuckMoveValid &&
           ctx.Snapshot().interaction.taggedState.tag ==
               runtime::ChessInteractionPhase::PieceLifted &&
           ctx.settings.LegalMoves(ctx.activeOwner) &&
           (ctx.validDestMask & (1ULL << square)) != 0ULL;
}

// Logical positions are Left, Top, Right, Bottom; wiring rotation happens later.
modules::Rgb& DirectionalEmitter(modules::BoardSquareLeds& leds, uint8_t position) {
    switch (position) {
        case 0U: return leds.left;
        case 1U: return leds.top;
        case 2U: return leds.right;
        default: return leds.bottom;
    }
}

void ApplyDirectedTravel(const ComposeContext& ctx, const DirectedTravel& cue,
                         modules::BoardLedFrame& board) {
    const uint32_t travelMs = std::max(DIRECTED_MIN_TRAVEL_MS,
        DIRECTED_SQUARE_TRAVEL_MS * cue.distance);
    const uint32_t phase = cue.elapsed % (travelMs + DIRECTED_DESTINATION_DWELL_MS);
    const uint8_t nodes = static_cast<uint8_t>((cue.distance + 1U) * cue.emitterCount);
    const uint32_t progress = std::min(phase, travelMs) * (nodes - 1U) * 256U / travelMs;
    const uint8_t head = static_cast<uint8_t>(progress / 256U);
    const uint16_t fraction = static_cast<uint16_t>(progress % 256U);
    const int rankDelta = int(cue.target / 8U) - int(cue.source / 8U);
    const int fileDelta = int(cue.target % 8U) - int(cue.source % 8U);
    const bool rankFirst = rankDelta == 2 || rankDelta == -2;
    const uint64_t removalMask = PhysicalRemovalSquareMask(ctx);
    for (uint8_t step = 0U; step <= cue.distance; ++step) {
        const uint8_t longSteps = std::min<uint8_t>(step, 2U);
        const uint8_t shortSteps = step > 2U ? 1U : 0U;
        const uint8_t rankSteps = cue.knight ? (rankFirst ? longSteps : shortSteps)
            : cue.duck ? (std::abs(rankDelta) * step + cue.distance / 2U) / cue.distance : step;
        const uint8_t fileSteps = cue.knight ? (rankFirst ? shortSteps : longSteps)
            : cue.duck ? (std::abs(fileDelta) * step + cue.distance / 2U) / cue.distance : step;
        const uint8_t square = static_cast<uint8_t>(
            (cue.source / 8U + cue.rankStep * rankSteps) * 8 + cue.source % 8U + cue.fileStep * fileSteps);
        const bool occupied = (ctx.physicalOcc64 & (1ULL << square)) != 0ULL;
        const bool reserveRails = cue.duck && ReservesChessLandingRails(ctx, square);
        const bool squareOnly = occupied || (step == cue.distance && cue.landingPending);
        if ((removalMask & (1ULL << square)) != 0ULL ||
            (ctx.kingSafetyPromptActive &&
             (square == ctx.kingSafetyKingSquare || square == ctx.kingSafetyAttackerSquare)) ||
            (!cue.duck && ctx.hintDuckMoveValid &&
             (square == ctx.hintDuckFromSquare || square == ctx.hintDuckToSquare))) continue;
        uint16_t cellWeight = 0U;
        std::array<uint16_t, 4U> emitterWeights{};
        uint8_t emitterMask = 0U;
        for (uint8_t local = 0U; local < cue.emitterCount; ++local) {
            const uint8_t node = static_cast<uint8_t>(step * cue.emitterCount + local);
            const uint16_t weight = node == head ? 256U - fraction
                : node == head + 1U ? fraction : 0U;
            cellWeight = std::max(cellWeight, weight);
            if (occupied && weight == 0U) continue;
            // At the L elbow, enter on the long axis and leave on the short
            // axis. The same two-node crossfade owns both straight legs.
            const bool vertical = local == 0U
                ? (step < 3U ? rankFirst : !rankFirst)
                : (step < 2U ? rankFirst : !rankFirst);
            const uint8_t incoming = vertical ? (cue.rankStep > 0 ? 1U : 3U)
                : (cue.fileStep > 0 ? 0U : 2U);
            uint8_t position = cue.knight
                ? static_cast<uint8_t>((incoming + (local == 0U ? 0U : 2U)) % 4U)
                : cue.order[local];
            // Occupancy and pending capture landing own both Piece emitters.
            // Keep the route continuous on the square rails when jumping over
            // a piece, rather than asking for that unrelated piece to move.
            if (squareOnly && (position == 1U || position == 3U)) {
                position = position == 1U ? 0U : 2U;
            }
            // Several logical nodes can share an occupied square's rail. Keep
            // the strongest head rather than letting a weaker node repaint it.
            emitterWeights[position] = std::max(emitterWeights[position], weight);
            emitterMask |= static_cast<uint8_t>(1U << position);
        }
        for (uint8_t position = 0U; position < 4U; ++position) {
            if ((emitterMask & (1U << position)) == 0U) continue;
            const bool pieceEmitter = position == 1U || position == 3U;
            if (!pieceEmitter && reserveRails) continue;
            auto& emitter = DirectionalEmitter(board[square], position);
            modules::Rgb replacement = emitter;
            if (!pieceEmitter && (step == 0U || step == cue.distance ||
                    (ctx.validDestMask & (1ULL << square)) != 0ULL)) {
                replacement = ScaleRgbSaturating(cue.duck ? modules::RGB_YELLOW
                    : step == 0U ? ctx.activePromptRgb : ctx.availablePromptRgb, DIRECTED_FLOOR_256);
            }
            replacement = modules::AlphaBlendRgb(replacement, cue.color,
                static_cast<uint8_t>(emitterWeights[position] * 255U / 256U));
            emitter = modules::AlphaBlendRgb(emitter, replacement, cue.alpha);
        }
        // Empty route cells use all four LEDs. The shared half-strength head
        // envelope keeps the full directional entry/exit emitter readable.
        if (!squareOnly && cellWeight != 0U) {
            for (uint8_t position = 0U; position < 4U; ++position) {
                if (reserveRails && (position == 0U || position == 2U)) continue;
                auto& emitter = DirectionalEmitter(board[square], position);
                emitter = modules::AlphaBlendRgb(emitter, cue.color,
                    static_cast<uint8_t>(cellWeight * 127U / 256U * cue.alpha / 255U));
            }
        }
    }
}

void ApplyDirectedLanding(const ComposeContext& ctx, const DirectedTravel& cue,
                          const bool pieceTravel, modules::BoardLedFrame& board) {
    auto& landing = board[cue.target];
    const auto color = cue.duck ? modules::RGB_YELLOW : ctx.availablePromptRgb;
    if (!cue.duck || !ReservesChessLandingRails(ctx, cue.target)) {
        landing.left = modules::AlphaBlendRgb(landing.left, color, cue.alpha);
        landing.right = modules::AlphaBlendRgb(landing.right, color, cue.alpha);
    }
    const uint8_t triangle = modules::animation::TriangleWave255({cue.elapsed}, 1200U);
    const uint16_t gain = static_cast<uint16_t>(triangle) * 160U / 255U;
    landing.top = modules::AlphaBlendRgb(landing.top, ScaleRgbSaturating(color,
        ctx.minimalAnimations ? 256U : pieceTravel ? DIRECTED_FLOOR_256 : 96U + gain), cue.alpha);
    landing.bottom = modules::AlphaBlendRgb(landing.bottom, ScaleRgbSaturating(color,
        ctx.minimalAnimations ? 256U : pieceTravel ? DIRECTED_FLOOR_256 : 256U - gain), cue.alpha);
}

void ApplyDuckMoveTravel(const ComposeContext& ctx, modules::BoardLedFrame& board) {
    const auto& snapshot = ctx.Snapshot();
    const auto& interaction = snapshot.interaction.taggedState;
    const bool relocating = interaction.tag ==
        runtime::ChessInteractionPhase::DuckRelocation;
    runtime::DirectedPhysicalPrompt prompt;
    const bool fixed = relocating &&
        interaction.plan.kind == runtime::PhysicalTransitionKind::DuckRelocation &&
        runtime::TryGetDirectedPhysicalPrompt(interaction, prompt);
    const bool chessProgress = modules::IsMatchingDuckHintChessProgress(
        interaction, ctx.hintFromSquare, ctx.hintToSquare);
    if (snapshot.game.variant != modules::GameVariant::Duck ||
        (!fixed && (!ctx.renderIntent.hintPromptVisible ||
                    (!IsIdleInteraction(snapshot) && !relocating && !chessProgress)))) return;
    const uint8_t source = fixed ? prompt.fromSquare
        : relocating ? ctx.hintFromSquare : ctx.hintDuckFromSquare;
    const uint8_t target = fixed ? prompt.toSquare
        : relocating ? ctx.hintToSquare : ctx.hintDuckToSquare;
    if ((!fixed && !(relocating ? ctx.hintMoveValid : ctx.hintDuckMoveValid)) ||
        source >= 64U || target >= 64U || source == target ||
        (ctx.physicalOcc64 & (1ULL << target)) != 0ULL) return;
    const int dr = int(target / 8U) - source / 8U;
    const int df = int(target % 8U) - source % 8U;
    const uint8_t vertical = dr > 0 ? 1U : 3U;
    const uint8_t horizontal = df > 0 ? 0U : 2U;
    const std::array<uint8_t, 4U> order = dr == 0
        ? std::array<uint8_t, 4U>{horizontal, uint8_t((horizontal + 2U) % 4U), 0U, 0U}
        : df == 0 ? std::array<uint8_t, 4U>{vertical, uint8_t((vertical + 2U) % 4U), 0U, 0U}
        : std::array<uint8_t, 4U>{vertical, horizontal,
            uint8_t((vertical + 2U) % 4U), uint8_t((horizontal + 2U) % 4U)};
    const DirectedTravel cue{source, target, fixed ? uint8_t{255U} : ctx.hintRefinementAlpha255,
        !fixed && snapshot.render.hintFeedbackTimingValid ? snapshot.render.hintFeedbackElapsedMs
            : snapshot.interaction.lastInteractionElapsedMs,
        (dr > 0) - (dr < 0), (df > 0) - (df < 0),
        static_cast<uint8_t>(std::max(std::abs(dr), std::abs(df))),
        static_cast<uint8_t>(dr != 0 && df != 0 ? 4U : 2U), false, true, false, order,
        modules::RGB_YELLOW};
    ApplyDirectedLanding(ctx, cue, !ctx.minimalAnimations && dr != 0, board);
    if (!ctx.minimalAnimations) ApplyDirectedTravel(ctx, cue, board);
}

uint8_t HintPromptPulseAlpha(const BoardComposeRuntime& runtime) {
    const uint8_t tri = modules::animation::TriangleWave255(
        runtime.globalAnim, HINT_PROMPT_PULSE_PERIOD_MS);
    const uint16_t range =
        DUCK_HINT_SOURCE_MAX_ALPHA - DUCK_HINT_SOURCE_MIN_ALPHA;
    return static_cast<uint8_t>(
        DUCK_HINT_SOURCE_MIN_ALPHA +
        (static_cast<uint16_t>(tri) * range + 127U) / 255U);
}

} // namespace

void ApplyDirectedMoveLanding(const ComposeContext& ctx,
                              modules::BoardLedFrame& board) {
    const auto& snapshot = ctx.Snapshot();
    const auto& interaction = snapshot.interaction;
    if (ctx.renderIntent.HasCelebration() ||
        ctx.renderIntent.menuForegroundVisible ||
        snapshot.render.textOverlayActive ||
        (interaction.placementModeActive && !ctx.undoPromptActive) ||
        interaction.kingQueenSwapPromptActive ||
        ctx.invalidBoard.IsActive() || ctx.humanIllegalMask != 0ULL ||
        ctx.cpuIllegalMask != 0ULL || ctx.cpuTentativeCorrectionMask != 0ULL ||
        ctx.captureReturnMask != 0ULL ||
        runtime::IsRuntimeHealthPlayPaused(ctx.in->runtimeHealth) ||
        ctx.in->higherPriorityStatusOverlayActive || snapshot.system.safeModeActive ||
        snapshot.system.hardwareTestActive || snapshot.system.amptestActive ||
        snapshot.system.otaInProgress) {
        return;
    }

    ApplyDuckMoveTravel(ctx, board);

    uint8_t source = 255U;
    uint8_t target = 255U;
    uint8_t alpha = 255U;
    uint32_t elapsed = ctx.undoPromptActive ? interaction.undoPromptElapsedMs
        : ctx.cpuTargetOccValid ? interaction.cpuMovePromptElapsedMs : interaction.lastInteractionElapsedMs;
    const bool humanPlacement =
        interaction.taggedState.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
        IsHumanPickupGuidanceActive(interaction);
    const auto& plan = interaction.taggedState.plan;
    if (ctx.cpuTargetOccValid || ctx.partialCastlingTargetOccValid || humanPlacement || ctx.undoPromptActive) {
        runtime::DirectedPhysicalPrompt prompt;
        if (interaction.cpuPromotionValid ||
            plan.kind == runtime::PhysicalTransitionKind::Promotion ||
            plan.kind == runtime::PhysicalTransitionKind::DuckRelocation) {
            return;
        }
        if (runtime::TryGetDirectedPhysicalPrompt(interaction.taggedState, prompt)) {
            source = prompt.fromSquare;
            target = prompt.toSquare;
        } else if (interaction.taggedState.requiredAction == runtime::RequiredPhysicalAction::RemoveCapture &&
                   plan.removalRequired && !plan.progress.removalComplete && plan.relocationCount != 0U) {
            // Preview the owned relocation while removal remains the required
            // action; do not turn the shared physical helper into a move prompt.
            source = plan.relocations[0U].from;
            target = plan.relocations[0U].to;
        } else {
            return;
        }
    } else if (ctx.renderIntent.hintPromptVisible && ctx.hintMoveValid &&
               (IsIdleInteraction(snapshot) ||
                modules::IsMatchingHumanHintPickup(
                    interaction.taggedState, ctx.hintFromSquare))) {
        source = ctx.hintFromSquare;
        target = ctx.hintToSquare;
        alpha = ctx.hintRefinementAlpha255;
        elapsed = snapshot.render.hintFeedbackTimingValid
            ? snapshot.render.hintFeedbackElapsedMs : interaction.lastInteractionElapsedMs;
    }
    const auto present = [&](const uint8_t square) {
        return square < 64U && (ctx.physicalOcc64 & (1ULL << square)) != 0ULL;
    };
    const bool victimPending =
        (ctx.renderIntent.hintPromptVisible && present(ctx.hintCaptureSquare)) ||
        (plan.removalRequired && !plan.progress.removalComplete && present(plan.removalSquare)) ||
        (ctx.pendingCaptureSquareValid && present(ctx.pendingCaptureSquare));
    if (source >= 64U || target >= 64U || alpha == 0U) {
        return;
    }
    // An occupied removal victim keeps its white cue. Once the mover is placed,
    // no route may compete with settling or outstanding off-destination removal.
    if (present(target) && (PhysicalRemovalSquareMask(ctx) & (1ULL << target)) == 0ULL) {
        return;
    }

    const int dr = static_cast<int>(target / 8U) - source / 8U;
    const int df = static_cast<int>(target % 8U) - source % 8U;
    const int ranks = dr < 0 ? -dr : dr, files = df < 0 ? -df : df;
    char piece = snapshot.board.enginePiecesValid ? snapshot.board.enginePieces[source] : '\0';
    if (ctx.undoPromptActive && ctx.undoMoverPieceFenKnown) {
        // The engine projection can be before or after the inverse. The
        // physical history owner knows which piece is actually returning.
        piece = runtime::IsSecondaryPhysicalPrompt(interaction.taggedState)
            ? (ctx.undoMoverPieceFen >= 'a' && ctx.undoMoverPieceFen <= 'z' ? 'r' : 'R')
            : ctx.undoMoverPieceFen;
    }
    const bool knight = piece == 'N' || piece == 'n';
    const bool knightVector = (ranks == 2 && files == 1) || (ranks == 1 && files == 2);
    // Knights follow their long leg first; unknown or unsupported geometry
    // cannot invent a route. Landing shares travel only on a vertical last leg.
    const bool canTravel = !ctx.minimalAnimations && piece != '\0' &&
        (knight ? knightVector : (dr == 0 || df == 0 || ranks == files)) &&
        (ranks != 0 || files != 0);
    const auto moverSide = piece >= 'a' && piece <= 'z'
        ? modules::LitchMaxEngine::PieceSide::Black : modules::LitchMaxEngine::PieceSide::White;
    const uint8_t incomingVertical = dr > 0 ? 1U : 3U;
    const uint8_t incomingHorizontal = df > 0 ? 0U : 2U;
    const uint8_t outgoingVertical = dr > 0 ? 3U : 1U;
    const uint8_t outgoingHorizontal = df > 0 ? 2U : 0U;
    const std::array<uint8_t, 4U> order = dr == 0
        ? std::array<uint8_t, 4U>{incomingHorizontal, outgoingHorizontal, 0U, 0U}
        : df == 0 ? std::array<uint8_t, 4U>{incomingVertical, outgoingVertical, 0U, 0U}
        : std::array<uint8_t, 4U>{incomingVertical, incomingHorizontal, outgoingVertical, outgoingHorizontal};
    const DirectedTravel cue{source, target, alpha, elapsed,
        (dr > 0) - (dr < 0), (df > 0) - (df < 0),
        static_cast<uint8_t>(knight ? 3 : ranks > files ? ranks : files),
        static_cast<uint8_t>(!knight && dr != 0 && df != 0 ? 4U : 2U), knight, false, victimPending, order,
        PieceRgbForPieceSide(ctx, moverSide)};
    if (!victimPending) ApplyDirectedLanding(ctx, cue,
        canTravel && (knight ? files == 2 : dr != 0), board);
    if (canTravel) ApplyDirectedTravel(ctx, cue, board);
}

void ApplyHeldHintMovePrompt(const ComposeContext& ctx,
                            const BoardComposeRuntime&,
                            const uint8_t logicalSq,
                            modules::Rgb& ioFinalSquare) {
    if (ctx.renderIntent.hintPromptVisible && ctx.hintMoveValid &&
        logicalSq == ctx.hintToSquare &&
        !IsHintCaptureRemovalSquare(ctx, logicalSq) &&
        ((ctx.validDestMask >> logicalSq) & 1ULL) != 0ULL) {
        // The board-level landing owner adds the visible piece pair after
        // ordinary occupancy cleanup; legal side rails stay available.
        ioFinalSquare = ctx.availablePromptRgb;
    }
}

uint16_t UndoCandidatePiecePulseScale256(const BoardComposeRuntime& runtime) {
    const auto& animationTokens = modules::animation::LED_FRAME_TOKENS;
    const uint32_t periodMs = (animationTokens.moverPieceBlink.periodMs == 0U)
                                  ? 1U
                                  : animationTokens.moverPieceBlink.periodMs;
    const modules::animation::AnimationContext pulseAnim{
        runtime.undoAnim.elapsedMs + static_cast<uint32_t>(periodMs / 2U)};
    const uint8_t tri = modules::animation::TriangleWave255(pulseAnim, periodMs);
    if (tri >= 254U) {
        return 256U;
    }
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(tri) * 256U + 127U) / 255U);
}

void ApplyHintMovePrompt(const ComposeContext& ctx,
                         const BoardComposeRuntime& runtime,
                         const uint8_t logicalSq,
                         modules::Rgb& ioFinalSquare,
                         BoardSquarePromptState& ioPromptState) {
    if (logicalSq == ctx.hintFromSquare) {
        ioFinalSquare = modules::AlphaBlendRgb(
            ioFinalSquare, ctx.activePromptRgb, ctx.hintRefinementAlpha255);
        const uint16_t scale = runtime.moverPieceBlinkOn ? 256U : 0U;
        // Reuse the directed CPU mover cue. During a refinement handoff the
        // optional blink fades back to ordinary piece light at the midpoint.
        ioPromptState.pieceFinalScale256 = static_cast<uint16_t>(256U -
            (256U - scale) * ctx.hintRefinementAlpha255 / 255U);
    } else if (logicalSq == ctx.hintToSquare &&
               !IsHintCaptureRemovalSquare(ctx, logicalSq)) {
        // After the chess half, the selectable Duck suggestion uses the
        // ordinary Hint pair. Its destination remains neutral Duck yellow.
        const bool duckRelocation =
            ctx.Snapshot().game.variant == modules::GameVariant::Duck &&
            ctx.Snapshot().interaction.taggedState.tag ==
                runtime::ChessInteractionPhase::DuckRelocation;
        ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare,
            duckRelocation ? modules::RGB_YELLOW : ctx.availablePromptRgb,
            ctx.hintRefinementAlpha255);
    }

    if (!ctx.hintDuckMoveValid) {
        return;
    }
    if (logicalSq == ctx.hintDuckFromSquare) {
        ioFinalSquare = modules::AlphaBlendRgb(
            ioFinalSquare,
            modules::RGB_YELLOW,
            static_cast<uint8_t>((ctx.minimalAnimations ? DUCK_HINT_SOURCE_MAX_ALPHA : HintPromptPulseAlpha(runtime)) *
                ctx.hintRefinementAlpha255 / 255U));
    }
    if (logicalSq == ctx.hintDuckToSquare && !ReservesChessLandingRails(ctx, logicalSq)) {
        ioFinalSquare = modules::AlphaBlendRgb(
            ioFinalSquare,
            modules::RGB_YELLOW,
            static_cast<uint8_t>(DUCK_HINT_DESTINATION_ALPHA *
                ctx.hintRefinementAlpha255 / 255U));
    }
}

} // namespace internal
} // namespace modules::led_frame_composer
