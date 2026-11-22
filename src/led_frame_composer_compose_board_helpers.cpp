#include "modules/led_frame_composer_internal.hpp"
#include "modules/assistance_preferences.hpp"

#include "modules/ui_color_wheel.hpp"
#include "runtime/directed_physical_prompt.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"
#include "runtime/game_state_machine_render_state.hpp"

namespace modules::led_frame_composer {
namespace internal {

namespace {

constexpr uint32_t CLOCK_WARNING_THRESHOLD_MS = 10U * 1000U;
constexpr uint32_t CHECK_CUE_PERIOD_MS = 1800U;
constexpr uint32_t CHECK_CUE_BEAT_MS = 360U;
constexpr uint32_t CHECK_CUE_ANSWER_OFFSET_MS = 560U;

modules::Rgb CheckCuePiece(const ComposeContext& ctx, const bool king,
                           const modules::Rgb baseline) {
    if (ctx.minimalAnimations) {
        const auto dimmed = ctx.minimumBrightness ? baseline : ScaleRgbSaturating(baseline, 96U);
        const uint8_t white = ctx.minimumBrightness ? 26U : king ? 160U : 96U;
        return modules::AlphaBlendRgb(dimmed, {white, white, white}, king ? 192U : 160U);
    }
    const auto& snapshot = ctx.Snapshot();
    const uint32_t elapsed = snapshot.game.checkStartValid
        ? ctx.nowMs - snapshot.game.checkStartMs : snapshot.interaction.lastInteractionElapsedMs;
    const uint32_t phase = elapsed % CHECK_CUE_PERIOD_MS;
    const uint32_t local = king ? phase : phase >= CHECK_CUE_ANSWER_OFFSET_MS
        ? phase - CHECK_CUE_ANSWER_OFFSET_MS : CHECK_CUE_PERIOD_MS;
    if (local >= CHECK_CUE_BEAT_MS) return baseline;

    // A nonzero dip makes a white accent readable even over a white piece.
    // At minimum brightness the ordinary pair is already at its output floor.
    const auto dip = ctx.minimumBrightness ? baseline : ScaleRgbSaturating(baseline, 102U);
    const uint8_t peak = ctx.minimumBrightness ? 26U : 208U;
    const modules::Rgb white{peak, peak, peak};
    if (local < 70U) return dip;
    if (local < 150U) return modules::AlphaBlendRgb(dip, white,
        static_cast<uint8_t>((local - 70U) * 255U / 80U));
    if (local < 230U) return white;
    return modules::AlphaBlendRgb(white, baseline,
        static_cast<uint8_t>((local - 230U) * 255U / 130U));
}

modules::LitchMaxEngine::PieceSide SideForFenPiece(const char piece) {
    if (piece >= 'A' && piece <= 'Z') {
        return modules::LitchMaxEngine::PieceSide::White;
    }
    if (piece >= 'a' && piece <= 'z') {
        return modules::LitchMaxEngine::PieceSide::Black;
    }
    return modules::LitchMaxEngine::PieceSide::None;
}

} // namespace

bool TryGetPlacedRelocationDestinationSide(
    const runtime::InteractionModelSnapshot& snapshot,
    const uint64_t physicalOcc64,
    const uint8_t logicalSquare,
    modules::LitchMaxEngine::PieceSide& outSide) {
    const runtime::PhysicalTransitionPlan& plan =
        snapshot.interaction.taggedState.plan;
    if (!snapshot.board.enginePiecesValid || logicalSquare >= 64U ||
        !runtime::HasActivePhysicalPlan(snapshot.interaction) ||
        plan.kind == runtime::PhysicalTransitionKind::Undo) {
        return false;
    }

    const uint64_t destinationBit = UINT64_C(1) << logicalSquare;
    if ((physicalOcc64 & destinationBit) == 0ULL) {
        return false;
    }

    for (uint8_t i = 0U; i < plan.relocationCount; ++i) {
        const runtime::PieceRelocation& relocation = plan.relocations[i];
        if (relocation.from >= 64U || relocation.to != logicalSquare ||
            (plan.progress.relocationsPlacedMask & (1U << i)) == 0U ||
            (physicalOcc64 & (UINT64_C(1) << relocation.from)) != 0ULL) {
            continue;
        }

        outSide = SideForFenPiece(
            snapshot.board.enginePieces[relocation.from]);
        return outSide != modules::LitchMaxEngine::PieceSide::None;
    }
    return false;
}

InvalidBoardState ComputeInvalidBoardState(const ComposeContext& ctx) {
    const auto& snapshot = ctx.Snapshot();

    InvalidBoardState out;
    if (snapshot.game.initialVariantValidationActive) {
        return out;
    }
    const uint64_t latchedDuckMask =
        snapshot.game.variant == modules::GameVariant::Duck &&
                modules::IsValidDuckSquare(snapshot.game.duckSquare)
            ? (1ULL << snapshot.game.duckSquare)
            : 0ULL;
    const uint64_t candidateDuckMask =
        snapshot.game.variant == modules::GameVariant::Undecided &&
                modules::IsValidDuckSquare(
                    snapshot.game.initialDuckCandidateSquare)
            ? (1ULL << snapshot.game.initialDuckCandidateSquare)
            : 0ULL;
    const uint64_t duckMask = latchedDuckMask | candidateDuckMask;
    const bool earlyDuckLift = snapshot.game.variant == modules::GameVariant::Duck &&
        !snapshot.game.variantOutcome.IsTerminal() &&
        runtime::IsEarlyDuckLiftAllowed(snapshot.interaction.taggedState);
    const uint64_t chessOccupancy = earlyDuckLift
        ? runtime::ProjectHumanChessOccupancy(
            latchedDuckMask, snapshot.interaction.taggedState, ctx.physicalOcc64)
        : ctx.physicalOcc64;
    const uint64_t expectedMissingMask =
        ctx.expectedOcc64 & ~chessOccupancy;
    const uint64_t expectedExtraMask =
        (chessOccupancy & ~ctx.expectedOcc64) & ~duckMask;
    const bool pendingCancelUndoTransition =
        IsCancelUndoTransitionActive(snapshot);

    // Undo owns the board feedback while its guided physical transition is
    // active, even if a stale compatibility flag also claims placement mode.
    // The coordinator has already applied the engine undo, so every raw
    // occupancy difference at this point is either expected undo progress or
    // covered by the dedicated local human-illegal cue.
    if (ctx.undoPromptActive) {
        return out;
    }

    const uint64_t duckLandingRemoval = runtime::GetDuckLandingRemovalMask(
        latchedDuckMask, snapshot.interaction.taggedState, ctx.expectedOcc64,
        ctx.physicalOcc64, ctx.humanIllegalMask);
    if (duckLandingRemoval != 0ULL && ctx.cpuIllegalMask == 0ULL &&
        ctx.captureReturnMask == 0ULL) {
        out.extraMask = duckLandingRemoval;
        return out;
    }

    if (snapshot.interaction.placementModeActive) {
        out.missingMask = snapshot.interaction.placementMissingMask;
        out.extraMask = snapshot.interaction.placementExtraMask & ~duckMask;
        return out;
    }

    if (pendingCancelUndoTransition && !ctx.undoPromptActive) {
        return out;
    }

    const bool expectedOccValid = (ctx.expectedOcc64 != 0ULL);
    const runtime::TaggedInteractionState& interactionState =
        snapshot.interaction.taggedState;
    if (interactionState.tag == runtime::ChessInteractionPhase::Correction) {
        const uint64_t duckExtraMask =
            duckMask & ctx.physicalOcc64 & ~ctx.expectedOcc64;
        const uint64_t correctionMask =
            (interactionState.correctionMask != 0ULL
                 ? interactionState.correctionMask
                 : (expectedMissingMask | expectedExtraMask)) &
            ~duckExtraMask;
        out.missingMask = expectedMissingMask & correctionMask;
        out.extraMask = expectedExtraMask & correctionMask;
        out.contextMask = correctionMask &
                          ~(out.missingMask | out.extraMask);
        return out;
    }

    if (expectedOccValid) {
        const bool mismatchPickupOverride = !pendingCancelUndoTransition &&
                                            interactionState.tag ==
                                                runtime::ChessInteractionPhase::PieceLifted &&
                                            snapshot.interaction.interactionStartedFromMismatch &&
                                            ctx.moveFromSquare < 64U &&
                                            (((expectedMissingMask >> ctx.moveFromSquare) & 1ULL) != 0ULL);
        if (mismatchPickupOverride) {
            out.missingMask = expectedMissingMask;
            out.extraMask = expectedExtraMask;
            return out;
        }

        const uint64_t legalProgressMask =
            runtime::GetLegalProgressPhysicalMask(interactionState) |
            snapshot.interaction.deferredFollowupMask;
        const uint64_t cpuSettleBounceMask =
            snapshot.render.cpuMoveSettlingActive &&
                    snapshot.render.settleSquare < 64U
                ? (UINT64_C(1) << snapshot.render.settleSquare)
                : 0ULL;
        const uint64_t unexplainedMissingMask =
            expectedMissingMask & ~legalProgressMask &
            ~ctx.cpuTentativeCorrectionMask & ~cpuSettleBounceMask;
        const uint64_t unexplainedExtraMask =
            expectedExtraMask & ~legalProgressMask &
            ~ctx.cpuTentativeCorrectionMask & ~cpuSettleBounceMask;
        if (!pendingCancelUndoTransition &&
            (unexplainedMissingMask | unexplainedExtraMask) != 0ULL) {
            out.missingMask = unexplainedMissingMask;
            out.extraMask = unexplainedExtraMask;
            return out;
        }
    }

    return out;
}

modules::BoardSquareLeds BuildMismatchRainbowLeds(const BoardComposeRuntime& runtime,
                                                  const uint8_t logicalSq) {
    const uint8_t squareWheel = static_cast<uint8_t>(runtime.mismatchRainbowBaseWheel + logicalSq * 9U);
    return modules::BoardSquareLeds{
        modules::ui_color_wheel::WheelToRgb(squareWheel),
        modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(squareWheel + 64U)),
        modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(squareWheel + 128U)),
        modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(squareWheel + 64U)),
    };
}

void ApplyThreatOverlays(const ComposeContext& ctx,
                         const BoardComposeRuntime& runtime,
                         const bool forceOff,
                         const uint8_t logicalSq,
                         modules::Rgb& ioFinalSquare,
                         modules::Rgb& ioPieceRgb) {
    if (ctx.renderIntent.HasCelebration() ||
        ctx.Snapshot().game.variant == modules::GameVariant::Duck) {
        return;
    }

    const auto& snapshot = ctx.Snapshot();

    if (!snapshot.interaction.placementModeActive && !IsSettlingActive(snapshot) && !forceOff &&
        snapshot.game.threatenedKingSquareValid && snapshot.game.threatenedKingSquare == logicalSq) {
        const runtime::ClockSide activeSide = modules::ClockSideFromSideToMove(snapshot.game.sideToMove);
        const uint32_t activeRemainingMs =
            activeSide == runtime::ClockSide::White
                ? snapshot.game.clockWhiteRemainingMs
                : snapshot.game.clockBlackRemainingMs;
        const bool clockWarning = snapshot.game.clockEnabled &&
                                  snapshot.game.clockRunning &&
                                  !snapshot.game.clockPaused &&
                                  snapshot.game.clockActiveSide == activeSide &&
                                  activeRemainingMs <= CLOCK_WARNING_THRESHOLD_MS;
        if (clockWarning) {
            ioFinalSquare = modules::Rgb{255, 0, 0};
        }
    }

    if (!snapshot.interaction.placementModeActive && !IsSettlingActive(snapshot) && !forceOff &&
        snapshot.game.threatenedKingSquareValid && snapshot.game.threatenedKingSquare < 64U) {
        const bool mate = snapshot.game.inCheckmate;
        const bool check = snapshot.game.inCheck;
        if (mate || check) {
            const uint8_t kingSq = snapshot.game.threatenedKingSquare;
            const bool isKingSquare = (logicalSq == kingSq);

            if (mate && isKingSquare) {
                const uint8_t pulse = ctx.minimalAnimations
                                          ? 162U
                                          : modules::animation::TriangleWave255(
                                                runtime.globalAnim, 600U);
                const uint8_t alpha = static_cast<uint8_t>(
                    90U + (static_cast<uint16_t>(pulse) * 110U) / 255U);
                ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare, modules::Rgb{255, 64, 64}, alpha);
            }

            const uint64_t squareBit = UINT64_C(1) << logicalSq;
            const bool isChecker = (snapshot.game.checkingPiecesMask & squareBit) != 0ULL;
            if (check && !mate && (isKingSquare || isChecker) &&
                (ctx.physicalOcc64 & squareBit) != 0ULL &&
                !ctx.invalidBoard.IsActive() && ctx.humanIllegalMask == 0ULL &&
                ctx.cpuIllegalMask == 0ULL && ctx.captureReturnMask == 0ULL &&
                !ctx.kingSafetyPromptActive && !ctx.allPiecesFlashActive && !ctx.undoPromptActive &&
                !ctx.promoMoveValid && !snapshot.interaction.cpuPromotionValid &&
                !ctx.renderIntent.menuForegroundVisible && !ctx.renderIntent.centerTextOverlayVisible &&
                !runtime::IsRuntimeHealthPlayPaused(ctx.in->runtimeHealth) &&
                !ctx.in->higherPriorityStatusOverlayActive &&
                !snapshot.system.safeModeActive && !snapshot.system.hardwareTestActive &&
                !snapshot.system.amptestActive && !snapshot.system.otaInProgress &&
                (PhysicalRemovalSquareMask(ctx) & squareBit) == 0ULL) {
                const modules::UiOwner checkedOwner =
                    modules::led_frame_composer::OwnerForSideToMove(snapshot.game.sideToMove, ctx.firstPlayerIsP2);
                if (ctx.settings.CheckCue(checkedOwner)) {
                    ioPieceRgb = CheckCuePiece(ctx, isKingSquare, ioPieceRgb);
                }
            }
        }
    }

    if (!snapshot.interaction.placementModeActive && !IsSettlingActive(snapshot) && !forceOff &&
        ctx.kingSafetyPromptActive && ctx.illegalOn &&
        !ctx.in->menuActive && !ctx.renderIntent.menuForegroundVisible &&
        !snapshot.render.textOverlayActive && !ctx.undoPromptActive &&
        !runtime::IsRuntimeHealthPlayPaused(ctx.in->runtimeHealth) &&
        !ctx.in->higherPriorityStatusOverlayActive && !snapshot.system.safeModeActive &&
        !snapshot.system.hardwareTestActive && !snapshot.system.amptestActive &&
        !snapshot.system.otaInProgress) {
        const modules::UiOwner checkedOwner =
            modules::led_frame_composer::OwnerForSideToMove(snapshot.game.sideToMove, ctx.firstPlayerIsP2);
        const modules::Rgb checkedPieceRgb = PieceRgbForOwner(ctx, checkedOwner);
        static constexpr uint16_t CHECKED_FLASH_SCALE256 = 336U;
        const modules::Rgb checkedBright = ScaleRgbSaturating(checkedPieceRgb, CHECKED_FLASH_SCALE256);
        const modules::Rgb checkedFlashRgb = modules::AlphaBlendRgb(checkedBright, modules::RGB_YELLOW, 64U);
        const modules::Rgb attackerFlashRgb = modules::AlphaBlendRgb(modules::RGB_RED, modules::RGB_YELLOW, 56U);
        const uint8_t dangerAlpha = modules::animation::SampleValue(runtime.globalAnim, runtime.dangerPulseToken);
        if (ctx.kingSafetyKingSquare < 64U && logicalSq == ctx.kingSafetyKingSquare) {
            ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare, checkedFlashRgb, dangerAlpha);
        } else if (ctx.kingSafetyAttackerSquare < 64U && logicalSq == ctx.kingSafetyAttackerSquare) {
            ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare, attackerFlashRgb, dangerAlpha);
        }
    }
}

void ApplyLastMovePulse(const ComposeContext& ctx,
                        const BoardComposeRuntime& runtime,
                        const bool forceOff,
                        const bool occupied,
                        const uint8_t logicalSq,
                        modules::Rgb& ioFinalSquare,
                        modules::Rgb& ioPieceRgb) {
    const auto& snapshot = ctx.Snapshot();
    const bool pendingCancelUndoTransition =
        IsCancelUndoTransitionActive(snapshot);
    const bool duckRelocationContinuesTurn =
        snapshot.game.variant == modules::GameVariant::Duck &&
        snapshot.game.duckTurnPhase ==
            modules::DuckTurnPhase::DuckRelocation &&
        snapshot.interaction.taggedState.plan.kind ==
            runtime::PhysicalTransitionKind::DuckRelocation;

    const bool allowLastMovePulse = (!ctx.renderIntent.HasCelebration() &&
                                     !forceOff &&
                                     (IsIdleInteraction(snapshot) ||
                                      duckRelocationContinuesTurn) &&
                                     !ctx.renderIntent.menuForegroundVisible && !snapshot.interaction.placementModeActive &&
                                     !ctx.undoPromptActive && !ctx.coachingOverlayActive &&
                                     !pendingCancelUndoTransition);
    if (!allowLastMovePulse) {
        return;
    }

    const uint8_t pulse01_255 =
        ctx.minimalAnimations
            ? static_cast<uint8_t>(128U)
            : modules::animation::SampleValue(
                  runtime.globalAnim, runtime.lastMovePulseToken);
    static constexpr int16_t PULSE_DELTA = 16;

    const auto PulseRgbPlusMinus = [&](const modules::Rgb base, const uint8_t pulse) -> modules::Rgb {
        const int16_t delta = static_cast<int16_t>(
            static_cast<int32_t>((static_cast<int32_t>(pulse) * (PULSE_DELTA * 2) + 127) / 255) - PULSE_DELTA);

        const auto addClamped = [&](const uint8_t value) -> uint8_t {
            int16_t x = static_cast<int16_t>(value) + delta;
            if (x < 0) {
                x = 0;
            } else if (x > 255) {
                x = 255;
            }
            return static_cast<uint8_t>(x);
        };

        return modules::Rgb{addClamped(base.r), addClamped(base.g), addClamped(base.b)};
    };

    bool p1AllowFrom = true;
    bool p1AllowTo = true;
    bool p2AllowFrom = true;
    bool p2AllowTo = true;

    if (ctx.lastMoveMostRecentKnown && ctx.p1LastMoveValid && ctx.p2LastMoveValid) {
        const bool preferP1 = ctx.lastMoveMostRecentIsP1;
        const auto SuppressIfOverlap = [&](const uint8_t p1Sq,
                                           const uint8_t p2Sq,
                                           bool& p1Allow,
                                           bool& p2Allow) {
            if (p1Sq < 64U && p2Sq < 64U && p1Sq == p2Sq) {
                if (preferP1) {
                    p2Allow = false;
                } else {
                    p1Allow = false;
                }
            }
        };

        SuppressIfOverlap(ctx.p1LastMoveFrom, ctx.p2LastMoveFrom, p1AllowFrom, p2AllowFrom);
        SuppressIfOverlap(ctx.p1LastMoveFrom, ctx.p2LastMoveTo, p1AllowFrom, p2AllowTo);
        SuppressIfOverlap(ctx.p1LastMoveTo, ctx.p2LastMoveFrom, p1AllowTo, p2AllowFrom);
        SuppressIfOverlap(ctx.p1LastMoveTo, ctx.p2LastMoveTo, p1AllowTo, p2AllowTo);
    }

    const auto PulseSquare = [&](const bool valid,
                                 const uint8_t fromSq,
                                 const uint8_t toSq,
                                 const modules::Rgb ownerPieceRgb,
                                 const bool allowFrom,
                                 const bool allowTo) {
        if (!valid) {
            return;
        }

        const modules::Rgb pulsingOwnerRgb = PulseRgbPlusMinus(ownerPieceRgb, pulse01_255);

        if (allowFrom && fromSq < 64U && logicalSq == fromSq) {
            const uint8_t alpha = static_cast<uint8_t>(24U + (static_cast<uint16_t>(pulse01_255) * 64U) / 255U);
            ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare, pulsingOwnerRgb, alpha);
        }

        if (allowTo && toSq < 64U && logicalSq == toSq) {
            const uint8_t alpha = static_cast<uint8_t>(32U + (static_cast<uint16_t>(pulse01_255) * 96U) / 255U);
            ioFinalSquare = modules::AlphaBlendRgb(ioFinalSquare, pulsingOwnerRgb, alpha);

            if (occupied) {
                ioPieceRgb = pulsingOwnerRgb;
            }
        }
    };

    PulseSquare(ctx.p1LastMoveValid, ctx.p1LastMoveFrom, ctx.p1LastMoveTo, ctx.p1PieceRgb, p1AllowFrom, p1AllowTo);
    PulseSquare(ctx.p2LastMoveValid, ctx.p2LastMoveFrom, ctx.p2LastMoveTo, ctx.p2PieceRgb, p2AllowFrom, p2AllowTo);
}


void ApplyDirectedMovePrompt(const ComposeContext& ctx,
                             const BoardComposeRuntime& runtime,
                             const bool forceOff,
                             const bool occupied,
                             const uint8_t logicalSq,
                             modules::Rgb& ioFinalSquare,
                             modules::Rgb& ioPieceRgb) {
    if (ctx.renderIntent.HasCelebration()) {
        return;
    }

    const auto& in = *ctx.in;
    const auto& snapshot = ctx.Snapshot();

    const bool cpuPromptActive = ctx.cpuTargetOccValid;
    const bool castlingPromptActive = ctx.partialCastlingTargetOccValid;
    if (in.menuActive || forceOff || (!cpuPromptActive && !castlingPromptActive && !ctx.undoMoverPhase)) {
        return;
    }

    const uint64_t bit = (1ULL << logicalSq);
    runtime::DirectedPhysicalPrompt directedPrompt;
    const bool directedPromptValid =
        runtime::TryGetDirectedPhysicalPrompt(
            snapshot.interaction.taggedState, directedPrompt);

    const bool physOcc = (ctx.physicalOcc64 & bit) != 0ULL;

    if (directedPromptValid &&
        logicalSq == directedPrompt.fromSquare && physOcc) {
        ioFinalSquare = ctx.activePromptRgb;
    } else if (directedPromptValid && logicalSq == directedPrompt.toSquare && !physOcc) {
        ioFinalSquare = ctx.availablePromptRgb;
    }

    // A castle's other relocation waits with its ordinary piece identity.
    // Blink only the piece that the current directed prompt asks to lift.
    const bool leavingSquare = directedPromptValid && physOcc &&
                               logicalSq == directedPrompt.fromSquare;
    bool moverSidePiece = false;
    if (leavingSquare) {
        if (ctx.undoMoverPhase) {
            // Engine Undo may already have restored an opponent on this
            // square. Its current physical occupant is still the returning mover.
            ioPieceRgb = ctx.undoMoverPieceRgb;
            moverSidePiece = true;
        } else if (snapshot.board.enginePiecesValid) {
            modules::LitchMaxEngine::PieceSide side =
                snapshot.board.enginePiecesValid
                    ? SideForFenPiece(snapshot.board.enginePieces[logicalSq])
                    : modules::LitchMaxEngine::PieceSide::None;
            if (side == modules::LitchMaxEngine::PieceSide::None) {
                (void)TryInferTransitionPieceSide(snapshot, ctx.physicalOcc64, logicalSq, side);
            }
            moverSidePiece = (OwnerForPieceSide(side, ctx.firstPlayerIsP2) == runtime.expectedMoverOwner);
        } else {
            moverSidePiece = (ctx.expectedMoverOcc64 & bit) != 0ULL;
        }
    }

    if (leavingSquare && moverSidePiece && occupied && !runtime.moverPieceBlinkOn) {
        ioPieceRgb = modules::RGB_BLACK;
    }
}

} // namespace internal
} // namespace modules::led_frame_composer
