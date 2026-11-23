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

constexpr uint32_t DIRECTED_MIN_TRAVEL_MS = 900U;
constexpr uint32_t DIRECTED_SQUARE_TRAVEL_MS = 480U;
constexpr uint32_t DIRECTED_DESTINATION_DWELL_MS = 240U;
constexpr uint16_t DIRECTED_FLOOR_256 = 96U;

struct DirectedTravel {
    uint8_t source, target, alpha;
    uint32_t elapsed;
    int rankStep, fileStep;
    uint8_t distance;
    bool knight, duck, landingPending;
    modules::Rgb color;
};

// Logical positions are Left, Top, Right, Bottom; wiring rotation happens later.
modules::Rgb& DirectionalEmitter(modules::BoardSquareLeds& leds, uint8_t position) {
    switch (position) {
        case 0U: return leds.left;
        case 1U: return leds.top;
        case 2U: return leds.right;
        default: return leds.bottom;
    }
}

uint8_t RouteSquare(const DirectedTravel& cue, const uint8_t step) {
    const int ranks = int(cue.target / 8U) - int(cue.source / 8U);
    const int files = int(cue.target % 8U) - int(cue.source % 8U);
    if (cue.duck) {
        const int rankNumerator = (cue.source / 8U) * cue.distance + ranks * step;
        const int fileNumerator = (cue.source % 8U) * cue.distance + files * step;
        int rank = rankNumerator / cue.distance, file = fileNumerator / cue.distance;
        const int rankRemainder = rankNumerator % cue.distance;
        const int fileRemainder = fileNumerator % cue.distance;
        // Absolute nearest cells are reversible. Half-cell ties choose the
        // same checkerboard color, which also survives a 180-degree rotation.
        const bool oddCell = (rank + file) % 2 != 0;
        if (rankRemainder * 2 > cue.distance ||
            (rankRemainder * 2 == cue.distance && oddCell)) ++rank;
        if (fileRemainder * 2 > cue.distance ||
            (fileRemainder * 2 == cue.distance && oddCell)) ++file;
        return static_cast<uint8_t>(rank * 8 + file);
    }
    const uint8_t longSteps = std::min<uint8_t>(step, 2U);
    const uint8_t shortSteps = step > 2U ? 1U : 0U;
    const bool rankFirst = std::abs(ranks) == 2;
    const uint8_t rankSteps = cue.knight ? (rankFirst ? longSteps : shortSteps) : step;
    const uint8_t fileSteps = cue.knight ? (rankFirst ? shortSteps : longSteps) : step;
    return static_cast<uint8_t>((cue.source / 8U + cue.rankStep * rankSteps) * 8 +
                               cue.source % 8U + cue.fileStep * fileSteps);
}

// The geometry is logical. The hardware mapper rotates the finished frame,
// including the four positions within a square.
constexpr uint8_t SQUARE_EMITTERS = static_cast<uint8_t>(modules::BoardLedSlots::SquarePair);
constexpr uint8_t ALL_EMITTERS = static_cast<uint8_t>(modules::BoardLedSlots::All);

uint8_t EntryEmitters(const uint8_t from, const uint8_t to) {
    uint8_t mask = 0U;
    if (to / 8U > from / 8U) mask |= static_cast<uint8_t>(modules::BoardLedSlots::Top);
    if (to / 8U < from / 8U) mask |= static_cast<uint8_t>(modules::BoardLedSlots::Bottom);
    if (to % 8U > from % 8U) mask |= static_cast<uint8_t>(modules::BoardLedSlots::Left);
    if (to % 8U < from % 8U) mask |= static_cast<uint8_t>(modules::BoardLedSlots::Right);
    return mask;
}

struct TravelCell {
    uint8_t square;
    uint8_t entry;
    uint8_t middle;
    uint8_t exit;

    uint8_t PhaseCount() const { return middle == 0U ? 2U : 3U; }
};

// A piece owns its inner lights. Use its near/far outer light to carry
// motion on a rank-only segment when the path cannot use the piece LEDs.
uint8_t ProjectToRail(const uint8_t mask) {
    const uint8_t rail = mask & SQUARE_EMITTERS;
    if (rail != 0U) return rail;
    return mask == static_cast<uint8_t>(modules::BoardLedSlots::Top)
        ? static_cast<uint8_t>(modules::BoardLedSlots::Left)
        : static_cast<uint8_t>(modules::BoardLedSlots::Right);
}

void ApplyDirectedTravel(const ComposeContext& ctx, const DirectedTravel& cue,
                         modules::BoardLedFrame& board) {
    std::array<TravelCell, 8U> cells{};
    uint8_t nodes = 0U;
    uint8_t before = cue.source;
    uint8_t square = cue.source;
    for (uint8_t step = 0U; step <= cue.distance; ++step) {
        const uint8_t after = step < cue.distance ? RouteSquare(cue, step + 1U) : square;
        const uint8_t entry = step == 0U ? EntryEmitters(square, after) : EntryEmitters(before, square);
        const uint8_t exit = step == cue.distance ? EntryEmitters(square, before) : EntryEmitters(after, square);
        const uint8_t middle = ALL_EMITTERS & ~(entry | exit);
        cells[step] = {square, entry, middle, exit};
        nodes += cells[step].PhaseCount();
        before = square;
        square = after;
    }

    const uint32_t travelMs = std::max(DIRECTED_MIN_TRAVEL_MS,
        DIRECTED_SQUARE_TRAVEL_MS * cue.distance);
    const uint32_t phase = cue.elapsed % (travelMs + DIRECTED_DESTINATION_DWELL_MS);
    const uint32_t progress = std::min(phase, travelMs) * (nodes - 1U) * 256U / travelMs;
    const uint8_t head = static_cast<uint8_t>(progress / 256U);
    const uint16_t fraction = static_cast<uint16_t>(progress % 256U);
    const uint64_t removalMask = PhysicalRemovalSquareMask(ctx);
    uint8_t nextNode = 0U;
    for (uint8_t step = 0U; step <= cue.distance; ++step) {
        const TravelCell& cell = cells[step];
        const uint8_t phases = cell.PhaseCount();
        const uint8_t firstNode = nextNode;
        nextNode += phases;
        const uint64_t bit = UINT64_C(1) << cell.square;
        if ((removalMask & bit) != 0ULL ||
            (ctx.kingSafetyPromptActive &&
             (cell.square == ctx.kingSafetyKingSquare || cell.square == ctx.kingSafetyAttackerSquare))) continue;

        const bool occupied = (ctx.physicalOcc64 & bit) != 0ULL;
        const bool squareOnly = occupied || (step == cue.distance && cue.landingPending);
        uint8_t entry = cell.entry, middle = cell.middle, exit = cell.exit;
        if (squareOnly) {
            entry = ProjectToRail(entry);
            exit = ProjectToRail(exit);
            // An occupied elbow can project both directions onto the same
            // rail. Keep the exit direction and enter from the other rail.
            if (entry == exit) entry = SQUARE_EMITTERS ^ exit;
            middle = SQUARE_EMITTERS;
        }

        uint16_t cellWeight = 0U;
        std::array<uint16_t, 4U> emitterWeights{};
        uint8_t emitterMask = 0U;
        for (uint8_t local = 0U; local < phases; ++local) {
            const uint8_t node = firstNode + local;
            const uint16_t weight = node == head ? 256U - fraction
                : node == head + 1U ? fraction : 0U;
            if (occupied && weight == 0U) continue;
            const bool center = local != 0U && local + 1U != phases;
            const uint8_t group = local == 0U ? entry : center ? middle : exit;
            const uint16_t contribution = squareOnly && center ? weight / 2U : weight;
            for (uint8_t position = 0U; position < 4U; ++position) {
                if ((group & (1U << position)) != 0U) emitterWeights[position] += contribution;
            }
            emitterMask |= group;
            cellWeight = std::max(cellWeight, weight);
        }
        // Outside the moving head, the source, legal squares and destination
        // keep the exact indications already painted by their owners.
        if (cellWeight == 0U) continue;
        for (uint8_t position = 0U; position < 4U; ++position) {
            if ((emitterMask & (1U << position)) == 0U) continue;
            const bool pieceEmitter = position == 1U || position == 3U;
            auto& emitter = DirectionalEmitter(board[cell.square], position);
            modules::Rgb base = emitter;
            if (!pieceEmitter && (step == 0U || step == cue.distance || (ctx.validDestMask & bit) != 0ULL)) {
                base = ScaleRgbSaturating(cue.duck ? modules::RGB_YELLOW
                    : step == 0U ? ctx.activePromptRgb : ctx.availablePromptRgb, DIRECTED_FLOOR_256);
            }
            // Keep a softer moving footprint under the group that leads.
            const uint16_t halo = squareOnly ? 0U : cellWeight / 2U;
            const uint16_t weight = emitterWeights[position] +
                (256U - emitterWeights[position]) * halo / 256U;
            const auto lit = modules::AlphaBlendRgb(base, cue.color, static_cast<uint8_t>(weight * 255U / 256U));
            emitter = modules::AlphaBlendRgb(emitter, lit, cue.alpha);
        }
    }
}

void ApplyDirectedLanding(const ComposeContext& ctx, const DirectedTravel& cue,
                          const bool pieceTravel, modules::BoardLedFrame& board) {
    auto& landing = board[cue.target];
    const auto color = cue.duck ? modules::RGB_YELLOW : ctx.availablePromptRgb;
    landing.left = modules::AlphaBlendRgb(landing.left, color, cue.alpha);
    landing.right = modules::AlphaBlendRgb(landing.right, color, cue.alpha);
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
    if (snapshot.game.variant != modules::GameVariant::Duck ||
        interaction.plan.kind != runtime::PhysicalTransitionKind::DuckRelocation) return;
    runtime::DirectedPhysicalPrompt prompt;
    const bool fixed = runtime::TryGetDirectedPhysicalPrompt(interaction, prompt);
    if (!fixed && (!ctx.renderIntent.hintPromptVisible || !ctx.hintMoveValid ||
                   ctx.hintFromSquare != interaction.plan.moveFrom)) return;
    const uint8_t source = fixed ? prompt.fromSquare : ctx.hintFromSquare;
    const uint8_t target = fixed ? prompt.toSquare : ctx.hintToSquare;
    if (source >= 64U || target >= 64U || source == target ||
        (ctx.physicalOcc64 & (1ULL << target)) != 0ULL) return;
    const int dr = int(target / 8U) - source / 8U;
    const int df = int(target % 8U) - source % 8U;
    const DirectedTravel cue{source, target, fixed ? uint8_t{255U} : ctx.hintRefinementAlpha255,
        !fixed && snapshot.render.hintFeedbackTimingValid ? snapshot.render.hintFeedbackElapsedMs
            : snapshot.interaction.lastInteractionElapsedMs,
        (dr > 0) - (dr < 0), (df > 0) - (df < 0),
        static_cast<uint8_t>(std::max(std::abs(dr), std::abs(df))),
        false, true, false,
        modules::RGB_YELLOW};
    ApplyDirectedLanding(ctx, cue, !ctx.minimalAnimations, board);
    if (!ctx.minimalAnimations) ApplyDirectedTravel(ctx, cue, board);
}

runtime::PieceRelocation CaptureFirstRoute(const ComposeContext& ctx, uint32_t& elapsed) {
    const auto& snapshot = ctx.Snapshot();
    const auto& interaction = snapshot.interaction;
    const uint8_t victim = interaction.taggedState.selectedSquare;
    if (victim >= 64U || !snapshot.board.enginePiecesValid ||
        (ctx.physicalOcc64 & (UINT64_C(1) << victim)) != 0ULL) return {};

    // Give each legal source one complete pass. The list and its order come
    // from the current position; physical pickup immediately selects a plan.
    const uint64_t candidates = interaction.captureFirstCapturerMask & ctx.physicalOcc64;
    uint32_t cycleMs = 0U;
    for (uint8_t pass = 0U; pass < 2U; ++pass) {
        for (uint8_t from = 0U; from < 64U; ++from) {
            if ((candidates & (UINT64_C(1) << from)) == 0ULL) continue;
            const char piece = snapshot.board.enginePieces[from];
            const bool adjacentPawn = (piece == 'P' || piece == 'p') && from / 8U == victim / 8U;
            // The legal mask can include both an ordinary capture and a pawn
            // taking en passant. Only the latter uses the separate landing.
            const uint8_t target = adjacentPawn ? interaction.captureFirstEnPassantLanding : victim;
            if (target >= 64U) continue;
            const int ranks = std::abs(int(target / 8U) - int(from / 8U));
            const int files = std::abs(int(target % 8U) - int(from % 8U));
            const uint8_t distance = piece == 'N' || piece == 'n' ? 3U : std::max(ranks, files);
            const uint32_t duration = std::max(DIRECTED_MIN_TRAVEL_MS,
                DIRECTED_SQUARE_TRAVEL_MS * distance) + DIRECTED_DESTINATION_DWELL_MS;
            if (pass == 0U) cycleMs += duration;
            else if (elapsed < duration) return {from, target};
            else elapsed -= duration;
        }
        if (cycleMs == 0U) return {};
        elapsed %= cycleMs;
    }
    return {};
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

    if (interaction.taggedState.tag == runtime::ChessInteractionPhase::DuckRelocation) {
        ApplyDuckMoveTravel(ctx, board);
        return;
    }

    uint8_t source = 255U;
    uint8_t target = 255U;
    uint8_t alpha = 255U;
    uint32_t elapsed = ctx.undoPromptActive ? interaction.undoPromptElapsedMs
        : ctx.cpuTargetOccValid ? interaction.cpuMovePromptElapsedMs : interaction.lastInteractionElapsedMs;
    const bool humanPlacement =
        interaction.taggedState.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
        IsHumanPickupGuidanceActive(interaction);
    const auto& plan = interaction.taggedState.plan;
    const bool captureFirst = interaction.taggedState.tag == runtime::ChessInteractionPhase::CaptureStaged;
    if (captureFirst) {
        // Minimal already marks all legal capturers and the vacated square.
        // Cycling their destinations would animate the choice before pickup.
        if (ctx.minimalAnimations) return;
        const auto route = CaptureFirstRoute(ctx, elapsed);
        source = route.from;
        target = route.to;
    } else if (ctx.cpuTargetOccValid || ctx.partialCastlingTargetOccValid || humanPlacement || ctx.undoPromptActive) {
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
        IsHintCaptureRemovalSquare(ctx, ctx.hintCaptureSquare) ||
        (plan.removalRequired && !plan.progress.removalComplete && present(plan.removalSquare)) ||
        (ctx.pendingCaptureSquareValid && present(ctx.pendingCaptureSquare));
    if (source >= 64U || target >= 64U || alpha == 0U) {
        return;
    }
    // An occupied removal victim keeps its removal cue. Once the mover is placed,
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
    // Knights follow their long leg first. Only supported moves own a travel path.
    const bool canTravel = !ctx.minimalAnimations && piece != '\0' &&
        (knight ? knightVector : (dr == 0 || df == 0 || ranks == files)) &&
        (ranks != 0 || files != 0);
    const auto moverSide = piece >= 'a' && piece <= 'z'
        ? modules::LitchMaxEngine::PieceSide::Black : modules::LitchMaxEngine::PieceSide::White;
    const DirectedTravel cue{source, target, alpha, elapsed,
        (dr > 0) - (dr < 0), (df > 0) - (df < 0),
        static_cast<uint8_t>(knight ? 3 : ranks > files ? ranks : files),
        knight, false, victimPending || (captureFirst && target == interaction.taggedState.selectedSquare),
        PieceRgbForPieceSide(ctx, moverSide)};
    if (!cue.landingPending) ApplyDirectedLanding(ctx, cue, canTravel, board);
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
}

} // namespace internal
} // namespace modules::led_frame_composer
