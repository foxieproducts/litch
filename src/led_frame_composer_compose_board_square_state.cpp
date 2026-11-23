#include <algorithm>
#include <cstdlib>

#include "modules/led_frame_composer_internal.hpp"
#include "modules/assistance_preferences.hpp"
#include "modules/beginner_mode_policy.hpp"
#include "modules/promotion_piece_cue.hpp"

#include "runtime/directed_physical_prompt.hpp"
#include "runtime/game_state_machine_render_state.hpp"

namespace modules::led_frame_composer {
namespace internal {

namespace {

constexpr uint32_t CASTLING_INTENT_PERIOD_MS = 1200U;

modules::Rgb LegalDestinationColor(const ComposeContext& ctx, const uint8_t logicalSq,
                                  const modules::Rgb background) {
    const auto& snapshot = ctx.Snapshot();
    const auto& phase = snapshot.interaction.taggedState;
    const auto& reveal = snapshot.render.legalDestinationReveal;
    if (ctx.minimalAnimations || ctx.minimumBrightness || !reveal.Started() ||
        phase.tag != runtime::ChessInteractionPhase::PieceLifted || phase.selectedSquare >= 64U ||
        ctx.renderIntent.menuForegroundVisible || ctx.renderIntent.centerTextOverlayVisible ||
        snapshot.interaction.placementModeActive || ctx.invalidBoard.IsActive() ||
        ctx.humanIllegalMask != 0ULL || ctx.cpuIllegalMask != 0ULL ||
        ctx.cpuTentativeCorrectionMask != 0ULL || ctx.captureReturnMask != 0ULL ||
        ctx.in->higherPriorityStatusOverlayActive ||
        runtime::IsRuntimeHealthPlayPaused(ctx.in->runtimeHealth)) {
        return ctx.availablePromptRgb;
    }

    const uint32_t elapsedMs = ctx.nowMs - reveal.StartMs();
    if (elapsedMs >= runtime::LEGAL_DESTINATION_REVEAL_MS) {
        return ctx.availablePromptRgb;
    }

    const int files = std::abs(static_cast<int>(logicalSq % 8U) -
                               static_cast<int>(phase.selectedSquare % 8U));
    const int ranks = std::abs(static_cast<int>(logicalSq / 8U) -
                               static_cast<int>(phase.selectedSquare / 8U));
    // One board-square step is equally quick along a rank, file or diagonal.
    const uint32_t delayMs = static_cast<uint32_t>(std::max(files, ranks)) *
        runtime::LEGAL_DESTINATION_STEP_MS;
    if (elapsedMs <= delayMs) return background;
    const uint32_t fadeElapsedMs = elapsedMs - delayMs;
    if (fadeElapsedMs >= runtime::LEGAL_DESTINATION_FADE_MS) {
        return ctx.availablePromptRgb;
    }
    const uint8_t alpha = static_cast<uint8_t>(
        fadeElapsedMs * 255U / runtime::LEGAL_DESTINATION_FADE_MS);
    return modules::AlphaBlendRgb(background, ctx.availablePromptRgb, alpha);
}

uint8_t CastlingIntentPulseAlpha(const uint32_t elapsedMs,
                                 const uint32_t phaseOffsetMs,
                                 const uint8_t minimumAlpha,
                                 const uint8_t maximumAlpha) {
    const modules::animation::AnimationContext animation{
        static_cast<uint32_t>(elapsedMs + phaseOffsetMs)};
    const uint8_t pulse = modules::animation::TriangleWave255(
        animation, CASTLING_INTENT_PERIOD_MS);
    return static_cast<uint8_t>(
        minimumAlpha +
        (static_cast<uint16_t>(maximumAlpha - minimumAlpha) * pulse) / 255U);
}

} // namespace

void ApplySquareStatePrompts(const ComposeContext& ctx,
                             const BoardComposeRuntime& runtime,
                             const bool forceOff,
                             const bool occupied,
                             const uint8_t logicalSq,
                             modules::Rgb& ioFinalSquare,
                             modules::Rgb& ioPieceRgb,
                             BoardSquarePromptState& ioPromptState) {
    const auto& in = *ctx.in;
    const auto& snapshot = ctx.Snapshot();
    const bool legalMovesVisible = ctx.settings.LegalMoves(ctx.activeOwner);
    const bool suppressExpectedMismatchPrompts =
        IsCancelUndoTransitionActive(snapshot);
    const bool interactionCorrectionActive =
        (((ctx.humanIllegalMask | ctx.cpuIllegalMask) &
          ctx.invalidBoard.missingMask) != 0ULL);
    const bool squareUsesInvalidBoardCue =
        ((ctx.invalidBoard.Mask() >> logicalSq) & 1ULL) != 0ULL;

    if (forceOff) {
        return;
    }

    // Settling is mandatory progress feedback. Optional placement/restore
    // effects cooperate above it; physical-action and correction prompts win.
    if (IsSettlingActive(snapshot) && ctx.moveToSquare < 64U) {
        static constexpr uint32_t DEFAULT_SETTLE_PRESENTATION_MS = 2000U;
        const uint32_t durationMs =
            ctx.settlePresentationDurationMs == 0U
                ? DEFAULT_SETTLE_PRESENTATION_MS
                : ctx.settlePresentationDurationMs;
        const uint32_t elapsedMs =
            ctx.settleVisualElapsedMs < durationMs
                ? ctx.settleVisualElapsedMs
                : durationMs;
        const uint32_t phase255 =
            (elapsedMs * 255U) / durationMs;

        const uint8_t toSq = ctx.moveToSquare;
        const bool isTo = logicalSq == toSq;

        static constexpr uint32_t START_PERIOD_MS = 520U;
        static constexpr uint32_t END_PERIOD_MS = 80U;
        const uint32_t range = START_PERIOD_MS - END_PERIOD_MS;
        const uint32_t period =
            START_PERIOD_MS - (range * phase255) / 255U;

        const modules::animation::AnimationContext settleAnim{elapsedMs};
        const uint8_t pulse =
            modules::animation::TriangleWave255(settleAnim, period);
        const uint8_t solidBlend = static_cast<uint8_t>(
            (static_cast<uint16_t>(phase255) *
                 static_cast<uint16_t>(phase255) +
             127U) /
            255U);
        const uint16_t inverseAlpha =
            static_cast<uint16_t>(255U - solidBlend);
        const uint8_t alphaBase = ctx.minimalAnimations
            ? 160U
            : static_cast<uint8_t>(
                (static_cast<uint16_t>(pulse) * inverseAlpha +
                 255U * static_cast<uint16_t>(solidBlend) + 127U) / 255U);

        if (isTo) {
            ioFinalSquare = modules::AlphaBlendRgb(
                ioFinalSquare, ctx.settleMoverPieceRgb, alphaBase);
        }
    }
    if (ctx.promoMoveValid && ctx.promoToSquare < 64U &&
        logicalSq == ctx.promoToSquare && occupied) {
        ioPieceRgb = ctx.promoMoverPieceRgb;
    }
    ApplyPlacementEffects(ctx, forceOff, logicalSq, ioFinalSquare);
    const auto& plan = snapshot.interaction.taggedState.plan;
    const uint32_t arrivalElapsed = ctx.settleVisualElapsedMs;
    const bool promotionSelecting = ctx.promoMoveValid &&
        arrivalElapsed < promotion_piece_cue::ARRIVAL_MS;
    if (occupied && logicalSq == snapshot.render.promotionArrivalSquare &&
        logicalSq == plan.moveTo && (snapshot.render.humanMoveSettlingActive || promotionSelecting) &&
        plan.promotionRequired &&
        plan.source == runtime::PhysicalTransitionSource::Human &&
        !ctx.invalidBoard.IsActive() && !in.higherPriorityStatusOverlayActive &&
        !runtime::IsRuntimeHealthPlayPaused(in.runtimeHealth)) {
        // The local swell can finish alongside Choose when Duck handoff
        // skips the timed move gate.
        ioPieceRgb = promotionSelecting ? ctx.promoMoverPieceRgb : ctx.settleMoverPieceRgb;
        ioPromptState.pieceFinalScale256 = promotion_piece_cue::ArrivalScale256(
            arrivalElapsed, ctx.minimalAnimations);
    }

    if ((ctx.undoMoverPhase || ctx.undoRestorePhase) && ctx.undoFromSquare < 64U && ctx.undoToSquare < 64U) {
        const auto undoPickupAlpha = [&]() -> uint8_t {
            return ctx.minimalAnimations ? 160U : modules::animation::SampleValue(
                runtime.undoAnim,
                modules::animation::LED_FRAME_TOKENS.interactionPickupPulse);
        };
        runtime::DirectedPhysicalPrompt directedUndoPrompt;
        const bool directedUndoPromptValid =
            runtime::TryGetDirectedPhysicalPrompt(
                snapshot.interaction.taggedState, directedUndoPrompt);
        const bool primaryUndoPrompt =
            !directedUndoPromptValid ||
            directedUndoPrompt.relocationIndex == 0U;

        if (!directedUndoPromptValid && ctx.undoMoverPhase && primaryUndoPrompt &&
            logicalSq == ctx.undoToSquare) {
            static constexpr uint8_t SOLID_ALPHA = 160U;
            if (runtime.undoMoverExpectedPresent) {
                ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare, ctx.undoMoverPieceRgb, SOLID_ALPHA);
            }

            if (occupied) {
                ioPieceRgb = ctx.undoMoverPieceRgb;
            }
        }

        if (!directedUndoPromptValid && ctx.undoMoverPhase && primaryUndoPrompt &&
            logicalSq == ctx.undoFromSquare &&
            !squareUsesInvalidBoardCue) {
            ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare, ctx.undoMoverPromptRgb, undoPickupAlpha());
        }

        if (ctx.undoRestorePhase && ctx.undoRestoreSquare < 64U && logicalSq == ctx.undoRestoreSquare &&
            !squareUsesInvalidBoardCue) {
            ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare, ctx.undoRestorePromptRgb, undoPickupAlpha());
        }

        if (!directedUndoPromptValid && ctx.undoMoverPhase &&
            !runtime.undoMoverPieceBlinkOn && logicalSq == ctx.undoToSquare && occupied) {
            ioPieceRgb = modules::RGB_BLACK;
        }

        static constexpr uint8_t INITIAL_SQUARE_ALPHA = 140U;
        if (!directedUndoPromptValid && !ctx.invalidBoard.IsActive() && runtime.undoMoverPieceBlinkOn) {
            if (((runtime.undoMoverInitialMask >> logicalSq) & 1ULL) != 0ULL) {
                ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare, ctx.undoMoverPieceRgb, INITIAL_SQUARE_ALPHA);
            }

            if (((runtime.undoRestoreInitialMask >> logicalSq) & 1ULL) != 0ULL) {
                ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare, ctx.undoRestorePieceRgb, INITIAL_SQUARE_ALPHA);
            }
        }

        const bool allowUndoCandidatePulse = !directedUndoPromptValid && runtime.undoCandidateBlinkActive && occupied &&
                                             ((runtime.undoCandidateBlinkMask >> logicalSq) & 1ULL) != 0ULL &&
                                             (!ctx.invalidBoard.IsActive() || ctx.undoRestorePhase);
        if (allowUndoCandidatePulse) {
            ioPieceRgb = runtime.undoCandidateBlinkRgb;
            ioPromptState.pieceFinalScale256 = ctx.minimalAnimations ? 256U : UndoCandidatePiecePulseScale256(runtime);
        }
    }

    // History transitions suppress expected occupancy differences, but a
    // misplaced piece still needs the ordinary local amber correction cue.
    // Strong feedback keeps its existing dedicated illegal-square owner.
    if (!ctx.invalidBoard.IsActive() &&
        ctx.correctionFeedback == modules::CorrectionFeedback::Gentle &&
        (((ctx.humanIllegalMask | ctx.cpuIllegalMask) >> logicalSq) & 1ULL) != 0ULL) {
        ApplyCorrectionSquarePrompt(ctx, runtime, occupied, logicalSq,
                                    ioFinalSquare, ioPieceRgb, ioPromptState);
    }

    if (!suppressExpectedMismatchPrompts &&
        (!ctx.freshSetupActive || interactionCorrectionActive) &&
        ctx.invalidBoard.IsActive()) {
        const uint64_t missingMask = ctx.invalidBoard.missingMask;
        const uint64_t extraMask = ctx.invalidBoard.extraMask |
                                   ctx.invalidBoard.contextMask;

        if ((((missingMask | extraMask) >> logicalSq) & 1ULL) != 0ULL) {
            ApplyCorrectionSquarePrompt(ctx, runtime, occupied, logicalSq,
                                        ioFinalSquare, ioPieceRgb, ioPromptState);
        }
    } else if (IsHumanPickupGuidanceActive(snapshot.interaction)) {
        uint8_t humanPromptFromSquare = ctx.moveFromSquare;
        runtime::DirectedPhysicalPrompt directedHumanPrompt;
        if (snapshot.interaction.taggedState.tag ==
                runtime::ChessInteractionPhase::ExecutingPlan) {
            humanPromptFromSquare =
                runtime::TryGetDirectedPhysicalPrompt(
                    snapshot.interaction.taggedState,
                    directedHumanPrompt)
                    ? directedHumanPrompt.fromSquare
                    : runtime::CHESS_INTERACTION_INVALID_SQUARE;
        }
        if (humanPromptFromSquare < 64U &&
            logicalSq == humanPromptFromSquare) {
            const uint8_t alpha = ctx.minimalAnimations ? 160U : modules::animation::SampleValue(
                runtime.globalAnim,
                modules::animation::LED_FRAME_TOKENS.interactionPickupPulse);
            ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare, ctx.activePromptRgb, alpha);
        } else if (legalMovesVisible && ((ctx.castlingKingDestinationMask >> logicalSq) & 1ULL) !=
                   0ULL) {
            // A castle destination is not merely a king move two files away.
            // A restrained rhythmic blend signals the coordinated second
            // piece while keeping the normal active-player color language.
            const uint8_t alpha = ctx.minimalAnimations ? 104U : CastlingIntentPulseAlpha(
                snapshot.interaction.lastInteractionElapsedMs,
                0U,
                48U,
                160U);
            ioFinalSquare = modules::AlphaBlendRgb(
                ctx.availablePromptRgb, ctx.activePromptRgb, alpha);
        } else if (legalMovesVisible && ((ctx.castlingRookSourceMask >> logicalSq) & 1ULL) != 0ULL) {
            // The rook answers the king destination half a beat later. Only
            // the square field is blended; the occupied rook remains calmly
            // lit in its ordinary piece color.
            const uint8_t alpha = ctx.minimalAnimations ? 48U : CastlingIntentPulseAlpha(
                snapshot.interaction.lastInteractionElapsedMs,
                CASTLING_INTENT_PERIOD_MS / 2U,
                16U,
                80U);
            ioFinalSquare = modules::AlphaBlendRgb(
                ioFinalSquare, ctx.activePromptRgb, alpha);
        } else if (legalMovesVisible && ((ctx.validDestMask >> logicalSq) & 1ULL) != 0ULL &&
                   !IsHintCaptureRemovalSquare(ctx, logicalSq)) {
            ioFinalSquare = LegalDestinationColor(ctx, logicalSq, ioFinalSquare);
        }
        ApplyHeldHintMovePrompt(ctx, runtime, logicalSq, ioFinalSquare);
    } else if (snapshot.interaction.taggedState.tag ==
                   runtime::ChessInteractionPhase::CaptureStaged &&
               !ctx.invalidBoard.IsActive() &&
               !ctx.renderIntent.menuForegroundVisible &&
               !ctx.renderIntent.centerTextOverlayVisible &&
               snapshot.interaction.captureFirstCapturerMask != 0ULL &&
               (((snapshot.interaction.captureFirstCapturerMask >> logicalSq) & 1ULL) != 0ULL ||
                (!occupied && logicalSq == snapshot.interaction.taggedState.selectedSquare))) {
        if (((snapshot.interaction.captureFirstCapturerMask >> logicalSq) & 1ULL) != 0ULL) {
            ioFinalSquare = ctx.activePromptRgb;
            ioPromptState.pieceFinalScale256 = ctx.minimalAnimations ? 256U : static_cast<uint16_t>(160U +
                modules::animation::TriangleWave255(runtime.globalAnim, 900U) * 96U / 255U);
        } else {
            // Mark the removed victim, including an off-destination EP pawn.
            // The chosen mover's plan will own its actual landing square.
            ioFinalSquare = ctx.availablePromptRgb;
        }
    } else if (IsIdleInteraction(snapshot) && in.previewFromLogical < 64U) {
        if (logicalSq == in.previewFromLogical) {
            const uint8_t alpha = modules::animation::SampleValue(
                runtime.globalAnim,
                modules::animation::LED_FRAME_TOKENS.interactionPickupPulse);
            ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare, ctx.activePromptRgb, alpha);
        } else if (((in.previewDestMask >> logicalSq) & 1ULL) != 0ULL) {
            ioFinalSquare = ctx.availablePromptRgb;
        }
    } else if (ctx.renderIntent.hintPromptVisible && ctx.hintMoveValid &&
               (IsIdleInteraction(snapshot) || snapshot.interaction.taggedState.tag ==
                    runtime::ChessInteractionPhase::DuckRelocation)) {
        ApplyHintMovePrompt(ctx, runtime, logicalSq, ioFinalSquare, ioPromptState);
    } else if (ctx.partialCastlingTargetOccValid) {
        const uint64_t bit = (1ULL << logicalSq);
        const bool targetOcc = (ctx.partialCastlingTargetOcc64 & bit) != 0ULL;
        const bool physOcc = (ctx.physicalOcc64 & bit) != 0ULL;
        if (targetOcc && !physOcc) {
            ioFinalSquare = ctx.availablePromptRgb;
        }
    }
}

} // namespace internal
} // namespace modules::led_frame_composer
