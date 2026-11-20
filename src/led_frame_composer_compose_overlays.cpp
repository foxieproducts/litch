#include "modules/led_frame_composer_internal.hpp"

#include "runtime/directed_physical_prompt.hpp"

#include "modules/animation_timing.hpp"
#include "modules/led_text_renderer.hpp"
#include "modules/orientation.hpp"
#include "modules/promotion_piece_cue.hpp"

namespace modules::led_frame_composer {
namespace internal {

namespace {

void CopyGrid(const std::array<modules::Rgb, 64>& source,
              std::array<modules::Rgb, 64>& outLogicalGrid) {
    for (uint8_t logicalSq = 0; logicalSq < 64; ++logicalSq) {
        outLogicalGrid[logicalSq] = source[logicalSq];
    }
}

void CopyGridToFrame(const std::array<modules::Rgb, 64>& logicalGrid,
                              modules::BoardLedFrame& outLedFrame,
                              std::array<modules::Rgb, 64>& outPhysicalGrid) {
    for (uint8_t logicalSq = 0; logicalSq < 64; ++logicalSq) {
        const modules::Rgb c = logicalGrid[logicalSq];
        outLedFrame[logicalSq].left = c;
        outLedFrame[logicalSq].right = c;
        outPhysicalGrid[logicalSq] = c;
    }
}

} // namespace

void ApplyMenuBackgroundDim(const ComposeContext& ctx,
                            modules::BoardLedFrame& outLedFrame,
                            std::array<modules::Rgb, 64>& outGridFrame) {
    if (!ctx.renderIntent.menuBackgroundDimVisible) {
        return;
    }

    const auto& in = *ctx.in;
    const uint8_t a = (in.menuActive && in.menuVisualAlpha255 == 0U) ? 255U : in.menuVisualAlpha255;
    if (a == 0U) {
        return;
    }

    // Reduce board background brightness while the menu is open so the menu UI
    // remains readable. This must affect both square LEDs and piece LEDs.
    uint16_t menuDimScale256 = in.menuBackgroundDimScale256;
    if (menuDimScale256 > 256U) {
        menuDimScale256 = 256U;
    }

    // Lerp scale from 1.0 (256) -> menuDimScale256 as alpha ramps 0..255.
    const uint32_t span = 256U - static_cast<uint32_t>(menuDimScale256);
    const uint32_t sub = (span * static_cast<uint32_t>(a) + 127U) / 255U;
    const uint16_t scale256 = static_cast<uint16_t>(256U - sub);

    for (uint8_t physicalSq = 0; physicalSq < 64U; ++physicalSq) {
        outLedFrame[physicalSq].left =
            ScaleRgbSaturating(outLedFrame[physicalSq].left,
                               scale256);
        outLedFrame[physicalSq].top = ScaleRgbSaturating(outLedFrame[physicalSq].top, scale256);
        outLedFrame[physicalSq].bottom = ScaleRgbSaturating(outLedFrame[physicalSq].bottom, scale256);
        outLedFrame[physicalSq].right =
            ScaleRgbSaturating(outLedFrame[physicalSq].right,
                               scale256);

        outGridFrame[physicalSq] = ScaleRgbSaturating(outGridFrame[physicalSq], scale256);
    }
}

void ApplyMenuOverlay(const ComposeContext& ctx,
                      modules::BoardLedFrame& outLedFrame,
                      std::array<modules::Rgb, 64>& outGridFrame,
                      modules::GridForegroundLayer* outMenuForeground) {
    const auto& in = *ctx.in;
    if (!ctx.renderIntent.menuForegroundVisible || !in.menuOverlay) {
        return;
    }

    std::array<modules::Rgb, 64> logicalGrid{};
    CopyGrid(outGridFrame, logicalGrid);

    std::array<modules::Rgb, modules::led_text_renderer::GRID_PIXELS> composed{};
    in.menuOverlay->Compose(
        logicalGrid, composed, in.menuActive, outMenuForeground);

    for (uint8_t logicalSq = 0; logicalSq < 64; ++logicalSq) {
        const modules::Rgb c = composed[logicalSq];
        outLedFrame[logicalSq].left = c;
        outLedFrame[logicalSq].right = c;
        outGridFrame[logicalSq] = c;
    }
}

void ApplyCenterTextOverlay(const ComposeContext& ctx,
                            modules::BoardLedFrame& outLedFrame,
                            std::array<modules::Rgb, 64>& outGridFrame) {
    const auto& in = *ctx.in;
    const auto& snapshot = ctx.Snapshot();
    runtime::DirectedPhysicalPrompt directedUndoPrompt;
    const bool directedUndoPromptValid =
        runtime::TryGetDirectedPhysicalPrompt(
            snapshot.interaction.taggedState, directedUndoPrompt);
    const bool secondaryUndoPrompt =
        directedUndoPromptValid &&
        directedUndoPrompt.relocationIndex == 1U;

    if (!ctx.renderIntent.centerTextOverlayVisible) {
        return;
    }

    // A known Undo relocation shares ordinary movement guidance. Do not dim
    // its travel or repaint its landing with the former return-square pulse.
    if (ctx.undoPromptActive && directedUndoPromptValid) {
        return;
    }

    bool promotionIconActive = false;
    modules::PromotionPiece promotionIconPiece =
        modules::PromotionPiece::Queen;
    bool undoOverlayActive = false;
    bool textColorValid = false;
    modules::Rgb textColor = modules::RGB_WHITE;
    // A physical-board correction owns the complete board surface. Keep the
    // pending promotion selection in state, but do not dim or paint over the
    // squares the user must restore.
    const bool correctionOwnsBoard = ctx.invalidBoard.IsActive();
    if (IsHumanPromotionActive(snapshot.interaction) &&
        !correctionOwnsBoard) {
        promotionIconActive = true;
        promotionIconPiece = snapshot.interaction.promotionSelection;
        if (ctx.promoMoveValid) {
            textColor = ctx.promoMoverPieceRgb;
            textColorValid = true;
        }
    } else if (IsCpuMovePromptActive(snapshot.interaction) &&
               !correctionOwnsBoard) {
        if (snapshot.interaction.cpuPromotionValid) {
            // CPU promotion UX: give the user a short moment to see the
            // destination square prompt before showing the selected piece.
            static constexpr uint32_t CPU_PROMO_OVERLAY_PRELUDE_MS = 650U;
            if (snapshot.interaction.cpuMovePromptElapsedMs >= CPU_PROMO_OVERLAY_PRELUDE_MS) {
                promotionIconActive = modules::promotion_piece_cue::
                    TryPieceFromSuffix(
                        snapshot.interaction.cpuPromotionSuffixLower,
                        promotionIconPiece);
            }
        }
    } else if (ctx.undoPromptActive) {
        undoOverlayActive = true;
    }

    if (!promotionIconActive && !undoOverlayActive) {
        return;
    }

    if (!textColorValid) {
        const modules::UiOwner owner =
            modules::led_frame_composer::OwnerForSideToMove(snapshot.game.sideToMove, ctx.firstPlayerIsP2);
        textColor = (owner == modules::UiOwner::P2) ? ctx.p2PieceRgb : ctx.p1PieceRgb;
    }

    // Strongly dim the whole board so the promotion silhouette or Undo's
    // spatial target remains readable (including piece LEDs).
    uint16_t bgScale256 = 48U; // ~19% intensity
    bool grayscaleBg = false;
    if (undoOverlayActive) {
        // Keep board detail visible while the spatial return/current-square
        // guidance owns the interaction.
        bgScale256 = 72U; // ~28% intensity
    }
    if (ctx.undoRestorePhase) {
        bgScale256 = 64U; // ~25% intensity
        grayscaleBg = true;
    }

    // Promotion UX: keep the destination square readable even when the center
    // text overlay dims the board.
    uint8_t protectLogicalSq = 255;
    if (promotionIconActive &&
        IsHumanPromotionActive(snapshot.interaction)) {
        if (ctx.promoMoveValid && ctx.promoToSquare < 64U) {
            protectLogicalSq = ctx.promoToSquare;
        }
    } else if (IsCpuMovePromptActive(snapshot.interaction)) {
        // Only protect when the CPU promotion piece is actually being shown.
        if (promotionIconActive && ctx.moveToSquare < 64U) {
            protectLogicalSq = ctx.moveToSquare;
        }
    }

    uint8_t protectPhysicalSq = 255;
    if (protectLogicalSq < 64U) {
        protectPhysicalSq = protectLogicalSq;
    }

    uint8_t protectUndoCurrentPhysicalSq = 255;
    uint8_t protectUndoRookCurrentPhysicalSq = 255;
    uint8_t protectUndoCurrentLogicalSq = 255;
    uint8_t protectUndoRookCurrentLogicalSq = 255;
    uint64_t protectUndoCandidatePhysicalMask = 0ULL;
    if (undoOverlayActive && ctx.undoMoverPhase) {
        if (secondaryUndoPrompt &&
            snapshot.interaction.undoCastlingRookMoveValid &&
            snapshot.interaction.undoCastlingRookTo < 64U) {
            protectUndoRookCurrentLogicalSq = snapshot.interaction.undoCastlingRookTo;
            protectUndoRookCurrentPhysicalSq =
                snapshot.interaction.undoCastlingRookTo;
        } else if (ctx.undoToSquare < 64U) {
            protectUndoCurrentLogicalSq = ctx.undoToSquare;
            protectUndoCurrentPhysicalSq = ctx.undoToSquare;
        }
    }

    if (undoOverlayActive && ctx.undoRestorePhase) {
        const BoardComposeRuntime boardRuntime = BuildBoardComposeRuntime(ctx);
        if (boardRuntime.undoCandidateBlinkActive) {
            for (uint8_t logicalSq = 0; logicalSq < 64U; ++logicalSq) {
                if (((boardRuntime.undoCandidateBlinkMask >> logicalSq) & 1ULL) == 0ULL) {
                    continue;
                }

                const uint8_t physicalSq = logicalSq;
                if (physicalSq < 64U) {
                    protectUndoCandidatePhysicalMask |= (1ULL << physicalSq);
                }
            }
        }
    }

    for (uint8_t physicalSq = 0; physicalSq < 64U; ++physicalSq) {
        if (physicalSq == protectPhysicalSq || physicalSq == protectUndoCurrentPhysicalSq ||
            physicalSq == protectUndoRookCurrentPhysicalSq ||
            ((protectUndoCandidatePhysicalMask >> physicalSq) & 1ULL) != 0ULL) {
            continue;
        }
        outLedFrame[physicalSq].top = ScaleRgbSaturating(outLedFrame[physicalSq].top, bgScale256);
        outLedFrame[physicalSq].bottom = ScaleRgbSaturating(outLedFrame[physicalSq].bottom, bgScale256);
    }

    std::array<modules::Rgb, 64> composed{};
    CopyGrid(outGridFrame, composed);

    for (uint8_t logicalSq = 0; logicalSq < 64U; ++logicalSq) {
        if (logicalSq == protectLogicalSq || logicalSq == protectUndoCurrentLogicalSq ||
            logicalSq == protectUndoRookCurrentLogicalSq) {
            continue;
        }

        auto& px = composed[logicalSq];
        if (grayscaleBg) {
            const uint16_t y = static_cast<uint16_t>(px.r) + static_cast<uint16_t>(px.g) + static_cast<uint16_t>(px.b);
            const uint8_t gray = static_cast<uint8_t>((y + 1U) / 3U);
            px = modules::Rgb{gray, gray, gray};
        }
        px = ScaleRgbSaturating(px, bgScale256);
    }

    if (promotionIconActive) {
        modules::promotion_piece_cue::DrawIcon(
            composed, promotionIconPiece, textColor);
    }

    const auto BlendToward = [&](const uint8_t logicalSq, const modules::Rgb target, const uint8_t a) {
        if (logicalSq >= 64U || a == 0U) {
            return;
        }
        composed[logicalSq] = modules::AlphaBlendRgb(composed[logicalSq], target, a);
    };

    const modules::animation::AnimationContext undoAnim{
        snapshot.interaction.undoPromptElapsedMs};
    const auto UndoPickupAlpha = [&]() -> uint8_t {
        return ctx.minimalAnimations ? 160U : modules::animation::SampleValue(
            undoAnim,
            modules::animation::LED_FRAME_TOKENS.interactionPickupPulse);
    };

    if (ctx.undoMoverPhase) {
        const uint8_t returnLogical =
            secondaryUndoPrompt &&
                    snapshot.interaction.undoCastlingRookFrom < 64U
                ? snapshot.interaction.undoCastlingRookFrom
                : ctx.undoFromSquare;
        if (returnLogical < 64U) {
            const uint8_t a = UndoPickupAlpha();

            BlendToward(returnLogical, ctx.undoMoverPromptRgb, a);
        }
    }

    if (ctx.undoRestorePhase && ctx.undoRestoreSquare < 64U) {
        const uint8_t restoreLogical = ctx.undoRestoreSquare;
        if (restoreLogical < 64U) {
            const uint8_t a = UndoPickupAlpha();

            BlendToward(restoreLogical, ctx.undoRestorePromptRgb, a);
        }
    }

    CopyGridToFrame(composed, outLedFrame, outGridFrame);
}

} // namespace internal
} // namespace modules::led_frame_composer
