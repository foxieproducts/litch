#include "modules/led_frame_composer_internal.hpp"

#include <algorithm>

#include "modules/board_ripple.hpp"
#include "modules/promotion_piece_cue.hpp"
#include "modules/ui_color_wheel.hpp"

namespace modules::led_frame_composer::internal {
namespace {

constexpr uint32_t REVEAL_HOLD_MS = 300U;
constexpr uint32_t REVEAL_LAND_MS = 820U;

std::array<uint8_t, 64> PromotionRevealLevels(const runtime::PromotionConfirmation& reveal,
                                              const uint32_t elapsed) {
    std::array<uint8_t, 64> levels{};
    constexpr int CELL = 256;
    const int progress = static_cast<int>(
        std::clamp(elapsed, REVEAL_HOLD_MS, REVEAL_LAND_MS) - REVEAL_HOLD_MS) *
        CELL / (REVEAL_LAND_MS - REVEAL_HOLD_MS);
    const int targetX = (reveal.square % 8U) * progress;
    const int targetY = (reveal.square / 8U) * progress;
    const auto& icon = promotion_piece_cue::GetIcon(reveal.piece);
    for (uint8_t row = 0U; row < promotion_piece_cue::ICON_HEIGHT; ++row) {
        for (uint8_t column = 0U; column < promotion_piece_cue::ICON_WIDTH; ++column) {
            if (!promotion_piece_cue::IsIconPixelOn(icon, row, column)) continue;
            const int x = (column + 1) * (CELL - progress) + targetX;
            const int y = (row + 1) * (CELL - progress) + targetY;
            for (int oy = 0; oy < 2 && y / CELL + oy < 8; ++oy) {
                for (int ox = 0; ox < 2 && x / CELL + ox < 8; ++ox) {
                    const int wx = ox == 0 ? CELL - x % CELL : x % CELL;
                    const int wy = oy == 0 ? CELL - y % CELL : y % CELL;
                    auto& level = levels[(y / CELL + oy) * 8 + x / CELL + ox];
                    level = std::max(level, static_cast<uint8_t>(wx * wy * 255 / (CELL * CELL)));
                }
            }
        }
    }
    return levels;
}

constexpr uint32_t MINIMAL_ACK_MS = 220U;
constexpr uint32_t LANDING_FLASH_MS = 160U;
constexpr uint32_t RAINBOW_FADE_MS = 250U;
constexpr uint32_t RAINBOW_TRAIL_MS = 40U;

struct PlacementEffect {
    modules::SettleAnimationStyle style;
    uint8_t centerSquare;
    uint32_t elapsedMs;
    modules::Rgb color;
    uint8_t maximumLocalAlpha;
    bool minimalAnimations;
};

void ApplyLocalPlacement(const PlacementEffect& effect, const uint8_t square,
                         modules::Rgb& rail) {
    const auto [style, center, elapsed, color, maximumLocalAlpha, minimal] = effect;
    const uint32_t duration = settle_animation_setting::DurationMs(style, minimal);
    if (center >= 64U || elapsed >= duration) return;
    if (minimal) {
        if (square == center && elapsed < MINIMAL_ACK_MS)
            rail = modules::AlphaBlendRgb(rail, color, 96U);
        return;
    }
    if (style == modules::SettleAnimationStyle::LandingFlash) {
        if (square == center && elapsed < LANDING_FLASH_MS) {
            const uint8_t alpha = static_cast<uint8_t>(
                224U * (LANDING_FLASH_MS - elapsed) / LANDING_FLASH_MS);
            rail = modules::AlphaBlendRgb(rail, modules::RGB_WHITE, alpha);
        }
        return;
    }
    if (style == modules::SettleAnimationStyle::MiniOrbit ||
        style == modules::SettleAnimationStyle::RainbowOrbit) {
        // Keep a readable local acknowledgement underneath the angular head,
        // including the required king-to-rook handoff during castling.
        if (square == center && elapsed < duration) {
            const uint8_t local = static_cast<uint8_t>(
                96U * (duration - elapsed) / duration);
            rail = modules::AlphaBlendRgb(rail, color, local);
        }
        return;
    }
    if (style != modules::SettleAnimationStyle::Ripple) return;
    // Retain the accepted Ripple recipe and local decay byte for byte.
    const uint8_t alpha = modules::board_ripple::SampleAlpha(
        center, square, elapsed, modules::board_ripple::PLACEMENT_RECIPE);
    rail = modules::AlphaBlendRgb(rail, color, alpha);
    if (square == center && elapsed < 250U) {
        const uint32_t remaining = 250U - elapsed;
        const uint8_t local = static_cast<uint8_t>(
            maximumLocalAlpha * remaining * remaining / (250U * 250U));
        rail = modules::AlphaBlendRgb(rail, color, local);
    }
}

bool RequiredPresentationOwnsBoard(const ComposeContext& ctx) {
    const auto& snapshot = ctx.Snapshot();
    const auto phase = snapshot.interaction.taggedState.tag;
    return (phase != runtime::ChessInteractionPhase::Idle &&
            phase != runtime::ChessInteractionPhase::Settling &&
            phase != runtime::ChessInteractionPhase::AwaitingAction) ||
        ctx.renderIntent.HasCelebration() || ctx.renderIntent.menuForegroundVisible ||
        snapshot.render.textOverlayActive || snapshot.interaction.placementModeActive ||
        ctx.invalidBoard.IsActive() || ctx.humanIllegalMask != 0ULL ||
        ctx.cpuIllegalMask != 0ULL || ctx.cpuTentativeCorrectionMask != 0ULL ||
        ctx.captureReturnMask != 0ULL || ctx.kingSafetyPromptActive ||
        ctx.undoPromptActive || ctx.promoMoveValid || ctx.kingQueenSwapPromptActive ||
        ctx.initialKingPlacementPromptActive || ctx.allPiecesFlashActive ||
        PhysicalRemovalSquareMask(ctx) != 0ULL ||
        ctx.in->higherPriorityStatusOverlayActive || snapshot.system.safeModeActive ||
        snapshot.system.hardwareTestActive || snapshot.system.amptestActive ||
        snapshot.system.otaInProgress;
    // InputPaused owns the perimeter. Last-sound accepted board effects may
    // finish; an incoming preview is separately retired by its owner.
}

void ApplyOrbit(const ComposeContext& ctx, const PlacementEffect& effect,
                modules::BoardLedFrame& board) {
    using Style = modules::SettleAnimationStyle;
    const auto [style, center, elapsed, color, maximumLocalAlpha, minimal] = effect;
    const bool rainbow = style == Style::RainbowOrbit;
    const uint32_t duration = settle_animation_setting::DurationMs(style, minimal);
    if (minimal || (style != Style::MiniOrbit && !rainbow) ||
        center >= 64U || elapsed >= duration ||
        ((ctx.physicalOcc64 & (UINT64_C(1) << center)) == 0ULL &&
         !ctx.Snapshot().render.cpuMoveSettlingActive) ||
        RequiredPresentationOwnsBoard(ctx)) return;

    const uint8_t quadrant = static_cast<uint8_t>(elapsed * 4U / duration);
    const auto head = rainbow
        ? modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(elapsed * 256U / duration))
        : color;
    const uint32_t remaining = duration - elapsed;
    const uint32_t fadeMs = rainbow ? RAINBOW_FADE_MS : 40U;
    const uint8_t alpha = remaining < fadeMs
        ? static_cast<uint8_t>(remaining * 224U / fadeMs) : 224U;
    auto& landing = board[center];
    // Inward emitters retain the mover's hue and minimum nonzero brightness.
    const uint16_t floor = ctx.minimumBrightness ? 256U
        : static_cast<uint16_t>(256U - alpha / 2U);
    landing.top = ScaleRgbSaturating(landing.top, quadrant == 1U ? 256U : floor);
    landing.bottom = ScaleRgbSaturating(landing.bottom, quadrant == 3U ? 256U : floor);

    const auto applyQuadrant = [&](const uint8_t part, const uint8_t strength) {
        if (part == 0U) landing.left = modules::AlphaBlendRgb(landing.left, head, strength);
        if (part == 2U) landing.right = modules::AlphaBlendRgb(landing.right, head, strength);
        static constexpr int8_t OFFSETS[4][2]{{0, -1}, {-1, 0}, {0, 1}, {1, 0}};
        const int rank = static_cast<int>(center / 8U) + OFFSETS[part][0];
        const int file = static_cast<int>(center % 8U) + OFFSETS[part][1];
        if (rank < 0 || rank >= 8 || file < 0 || file >= 8) return;
        const uint8_t neighbor = static_cast<uint8_t>(rank * 8 + file);
        // A shown Hint retains its destination guidance above the halo.
        if (ctx.renderIntent.hintPromptVisible &&
            (neighbor == ctx.hintToSquare ||
             (ctx.validDestMask & (UINT64_C(1) << neighbor)) != 0ULL)) return;
        auto& halo = board[neighbor];
        if (part != 0U) halo.left = modules::AlphaBlendRgb(halo.left, head, strength);
        if (part != 2U) halo.right = modules::AlphaBlendRgb(halo.right, head, strength);
    };
    applyQuadrant(quadrant, alpha);
    const uint32_t quadrantPhase = elapsed * 4U % duration;
    if (rainbow && quadrant != 0U && quadrantPhase < RAINBOW_TRAIL_MS * 4U) {
        const uint8_t trailAlpha = static_cast<uint8_t>(
            alpha * (RAINBOW_TRAIL_MS * 4U - quadrantPhase) / (RAINBOW_TRAIL_MS * 8U));
        applyQuadrant(static_cast<uint8_t>(quadrant - 1U), trailAlpha);
    }
}

} // namespace

bool ApplyPromotionReveal(const ComposeContext& ctx, modules::BoardLedFrame& board,
                          std::array<modules::Rgb, 64>& grid) {
    const auto& snapshot = ctx.Snapshot();
    const auto& reveal = snapshot.render.promotionConfirmation;
    const uint32_t confirmationElapsed = ctx.nowMs - reveal.startedAtMs;
    const uint32_t arrivalElapsed = ctx.settleVisualElapsedMs;
    uint32_t arrivalLead = 0U;
    if (!ctx.minimalAnimations && reveal.automatic &&
        snapshot.render.promotionArrivalSquare == reveal.square &&
        arrivalElapsed >= confirmationElapsed) {
        const uint32_t arrivalBeforeConfirmation = arrivalElapsed - confirmationElapsed;
        if (arrivalBeforeConfirmation < promotion_piece_cue::ARRIVAL_MS) {
            arrivalLead = promotion_piece_cue::ARRIVAL_MS - arrivalBeforeConfirmation;
        }
    }
    const auto& phase = snapshot.interaction.taggedState;
    const bool duck = phase.tag == runtime::ChessInteractionPhase::DuckRelocation;
    const bool terminal = ctx.renderIntent.HasCelebration();
    const bool idle = IsIdleInteraction(snapshot);
    const bool nextPickup = phase.tag == runtime::ChessInteractionPhase::PieceLifted;
    const uint32_t duration = terminal ? REVEAL_HOLD_MS : runtime::PromotionConfirmation::REVEAL_MS;
    if (reveal.square >= 64U || confirmationElapsed >= duration + arrivalLead ||
        (ctx.physicalOcc64 & (1ULL << reveal.square)) == 0ULL ||
        reveal.side != snapshot.render.lastMoveMostRecentSide ||
        reveal.square != (reveal.side == PersistedSide::White
            ? snapshot.render.whiteLastMoveToSquare : snapshot.render.blackLastMoveToSquare) ||
        (!idle && !duck && !nextPickup) ||
        phase.requiredAction == runtime::RequiredPhysicalAction::CorrectBoard ||
        ctx.invalidBoard.IsActive() ||
        ctx.humanIllegalMask != 0ULL || ctx.cpuIllegalMask != 0ULL ||
        ctx.captureReturnMask != 0ULL || ctx.undoPromptActive ||
        ctx.renderIntent.menuForegroundVisible || snapshot.render.textOverlayActive ||
        ctx.in->higherPriorityStatusOverlayActive || ctx.in->litchcraftActive ||
        snapshot.system.safeModeActive || snapshot.system.hardwareTestActive ||
        snapshot.system.amptestActive || snapshot.system.otaInProgress ||
        runtime::IsRuntimeHealthPlayPaused(ctx.in->runtimeHealth)) {
        return false;
    }
    const uint64_t destination = UINT64_C(1) << reveal.square;
    if ((PhysicalRemovalSquareMask(ctx) & destination) != 0ULL ||
        (nextPickup && ((ctx.validDestMask & destination) != 0ULL ||
                       (ctx.hintMoveValid && ctx.hintToSquare == reveal.square)))) {
        return false;
    }

    const auto owner = OwnerForPersistedSide(reveal.side, ctx.firstPlayerIsP2);
    const auto color = owner == UiOwner::P2 ? ctx.p2PieceRgb : ctx.p1PieceRgb;
    const auto paint = [&](const uint8_t square, const uint8_t alpha) {
        constexpr auto blend = animation::BlendMode::AlphaBlend;
        ApplyToBoardSquare(board[square], BoardLedSlots::SquarePair, color, alpha, blend);
        if (square == reveal.square) {
            ApplyToBoardSquare(board[square], BoardLedSlots::PiecePair, color, alpha, blend);
        }
        grid[square] = AlphaBlendRgb(grid[square], color, alpha);
    };
    // A quick acknowledgement can bypass settling. Finish the local cue
    // before expanding the reveal, without delaying the chess action.
    if (confirmationElapsed < arrivalLead) {
        SetPiecePair(board[reveal.square], ScaleRgbSaturating(color,
            promotion_piece_cue::ArrivalScale256(arrivalElapsed, false)));
        return true;
    }
    const uint32_t elapsed = confirmationElapsed - arrivalLead;
    if ((idle || duck) && !terminal && !ctx.minimalAnimations && elapsed < REVEAL_LAND_MS) {
        const auto levels = PromotionRevealLevels(reveal, elapsed);
        for (uint8_t square = 0U; square < 64U; ++square) {
            if (levels[square] == 0U || square == snapshot.game.duckSquare ||
                (duck && ctx.hintDuckMoveValid && square == ctx.hintDuckToSquare) ||
                (snapshot.game.inCheck && snapshot.game.threatenedKingSquareValid &&
                 square == snapshot.game.threatenedKingSquare) ||
                (ctx.kingSafetyPromptActive && (square == ctx.kingSafetyKingSquare ||
                                               square == ctx.kingSafetyAttackerSquare))) continue;
            paint(square, levels[square]);
        }
    }
    const uint8_t landingAlpha = ctx.minimalAnimations || elapsed < REVEAL_LAND_MS
        ? 255U : static_cast<uint8_t>(255U - (elapsed - REVEAL_LAND_MS) * 70U /
              (runtime::PromotionConfirmation::REVEAL_MS - REVEAL_LAND_MS));
    paint(reveal.square, landingAlpha);
    return true;
}

void ApplyPlacementEffects(const ComposeContext& ctx, const bool forceOff,
                           const uint8_t square, modules::Rgb& rail) {
    if (ctx.renderIntent.HasCelebration() || forceOff || ctx.invalidBoard.IsActive()) return;
    if (IsSettlingActive(ctx.Snapshot()))
        ApplyLocalPlacement({ctx.settleAnimationStyle, ctx.moveToSquare,
            ctx.settleVisualElapsedMs, ctx.settleMoverPieceRgb, 96U, ctx.minimalAnimations}, square, rail);
    if (ctx.settleAfterglowActive &&
        (!IsSettlingActive(ctx.Snapshot()) ||
         ctx.settleAnimationStyle == modules::SettleAnimationStyle::Ripple ||
         (ctx.settleAfterglowSquare < 64U &&
          (ctx.physicalOcc64 & (UINT64_C(1) << ctx.settleAfterglowSquare)) != 0ULL)))
        ApplyLocalPlacement({ctx.settleAnimationStyle, ctx.settleAfterglowSquare,
            ctx.settleAfterglowElapsedMs, ctx.settleAfterglowRgb, 255U, ctx.minimalAnimations}, square, rail);
}

void ApplyPlacementEmitterEffects(const ComposeContext& ctx, modules::BoardLedFrame& board) {
    if (IsSettlingActive(ctx.Snapshot()))
        ApplyOrbit(ctx, {ctx.settleAnimationStyle, ctx.moveToSquare,
            ctx.settleVisualElapsedMs, ctx.settleMoverPieceRgb, 96U, ctx.minimalAnimations}, board);
    else if (ctx.settleAfterglowActive)
        ApplyOrbit(ctx, {ctx.settleAnimationStyle, ctx.settleAfterglowSquare,
            ctx.settleAfterglowElapsedMs, ctx.settleAfterglowRgb, 255U, ctx.minimalAnimations}, board);
}

void ApplyPlacementStylePreview(const ComposeContext& ctx, modules::BoardLedFrame& board,
                                std::array<modules::Rgb, 64>& grid) {
    const auto& preview = ctx.boardStylePreview;
    if (preview.kind != modules::BoardStylePreviewKind::Placement || RequiredPresentationOwnsBoard(ctx) ||
        runtime::IsRuntimeHealthPlayPaused(ctx.in->runtimeHealth)) return;
    const auto color = PieceRgbForPieceSide(ctx, preview.white
        ? modules::LitchMaxEngine::PieceSide::White : modules::LitchMaxEngine::PieceSide::Black);
    const PlacementEffect effect{preview.placementStyle, preview.anchorSquare,
        preview.elapsedMs, color, 96U, ctx.minimalAnimations};
    for (uint8_t square = 0U; square < 64U; ++square) {
        ApplyLocalPlacement(effect, square, board[square].left);
        ApplyLocalPlacement(effect, square, board[square].right);
    }
    ApplyOrbit(ctx, effect, board);
    for (uint8_t square = 0U; square < 64U; ++square)
        grid[square] = modules::RepresentativeSquareColor(board[square]);
}

} // namespace modules::led_frame_composer::internal
