#include "modules/led_frame_composer_internal.hpp"

#include "hardware/led_layout.hpp"
#include "modules/animation_timing.hpp"
#include "modules/led_render_policy.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"
#include "runtime/game_state_machine_castling.hpp"
#include "runtime/square_mapping.hpp"

namespace modules::led_frame_composer {
namespace internal {

namespace {

modules::LitchMaxEngine::PieceSide SideForFenPiece(const char piece) {
    if (piece >= 'A' && piece <= 'Z') {
        return modules::LitchMaxEngine::PieceSide::White;
    }
    if (piece >= 'a' && piece <= 'z') {
        return modules::LitchMaxEngine::PieceSide::Black;
    }
    return modules::LitchMaxEngine::PieceSide::None;
}

modules::LitchMaxEngine::PieceSide OppositePieceSide(
    const modules::LitchMaxEngine::PieceSide side) {
    if (side == modules::LitchMaxEngine::PieceSide::White) {
        return modules::LitchMaxEngine::PieceSide::Black;
    }
    if (side == modules::LitchMaxEngine::PieceSide::Black) {
        return modules::LitchMaxEngine::PieceSide::White;
    }
    return modules::LitchMaxEngine::PieceSide::None;
}

bool TryGetPendingDuckChessPieceSide(
    const runtime::InteractionModelSnapshot& snapshot,
    const uint8_t logicalSquare,
    modules::LitchMaxEngine::PieceSide& outSide) {
    const runtime::TaggedInteractionState& phase =
        snapshot.interaction.taggedState;
    if (snapshot.game.variant != modules::GameVariant::Duck ||
        snapshot.game.duckTurnPhase !=
            modules::DuckTurnPhase::DuckRelocation ||
        phase.plan.kind != runtime::PhysicalTransitionKind::DuckRelocation ||
        logicalSquare >= 64U) {
        return false;
    }

    const modules::LitchMaxEngine::PieceSide stagedMover =
        snapshot.game.sideToMove == modules::SideToMove::White
            ? modules::LitchMaxEngine::PieceSide::White
            : modules::LitchMaxEngine::PieceSide::Black;
    const uint64_t mask = 1ULL << logicalSquare;
    if ((snapshot.board.opponentOccupiedMask & mask) != 0ULL) {
        outSide = stagedMover;
        return true;
    }
    if ((snapshot.board.moverOccupiedMask & mask) != 0ULL) {
        outSide = OppositePieceSide(stagedMover);
        return true;
    }
    return false;
}

} // namespace

uint64_t ComposeBoard(const ComposeContext& ctx,
                      modules::BoardLedFrame& outLedFrame,
                      std::array<modules::Rgb, 64>& outGridFrame) {
    const auto& in = *ctx.in;
    const auto& snapshot = ctx.Snapshot();
    const BoardComposeRuntime boardRuntime = BuildBoardComposeRuntime(ctx);
    const modules::UiOwner expectedMoverOwner = boardRuntime.expectedMoverOwner;
    const modules::UiOwner expectedOpponentOwner = boardRuntime.expectedOpponentOwner;
    const bool gameOverCelebration = ctx.renderIntent.HasCelebration();
    const bool pendingCancelUndoTransition = !gameOverCelebration &&
                                             IsCancelUndoTransitionActive(snapshot);
    const runtime::MoveRenderTransition& transition =
        snapshot.render.moveTransition;

    const auto TryGetCancelUndoReappliedOwner = [&](const uint8_t logicalSq, modules::UiOwner& outOwner) -> bool {
        if (!pendingCancelUndoTransition || ctx.activeOwner == modules::UiOwner::None) {
            return false;
        }

        const modules::UiOwner movedOwner =
            (ctx.activeOwner == modules::UiOwner::P2) ? modules::UiOwner::P1 : modules::UiOwner::P2;
        if (transition.toSquare < 64U && logicalSq == transition.toSquare) {
            outOwner = movedOwner;
            return true;
        }

        uint8_t rookFrom = 0;
        uint8_t rookTo = 0;
        if (transition.hasCastlingRook &&
            runtime::TryGetCastlingRookMoveForKingMove(transition.fromSquare,
                                                       transition.toSquare,
                                                       rookFrom,
                                                       rookTo) &&
            rookTo < 64U && logicalSq == rookTo) {
            outOwner = movedOwner;
            return true;
        }

        return false;
    };

    auto& boardFrame = outLedFrame;
    const auto& duckInteraction = snapshot.interaction.taggedState;
    uint64_t renderedDuckMask = 0ULL;
    uint64_t earlyDuckLanding = 0ULL;
    if (snapshot.game.variant == modules::GameVariant::Duck &&
        modules::IsValidDuckSquare(snapshot.game.duckSquare)) {
        renderedDuckMask = 1ULL << snapshot.game.duckSquare;
        if (duckInteraction.plan.kind == runtime::PhysicalTransitionKind::DuckRelocation &&
            duckInteraction.plan.progress.relocationsPlacedMask != 0U &&
            duckInteraction.plan.moveTo < 64U) {
            renderedDuckMask = 1ULL << duckInteraction.plan.moveTo;
        } else if (!gameOverCelebration && !ctx.invalidBoard.IsActive() &&
                   ctx.humanIllegalMask == 0ULL) {
            earlyDuckLanding = ctx.physicalOcc64 &
                ~runtime::ProjectHumanChessOccupancy(
                    renderedDuckMask, duckInteraction, ctx.physicalOcc64);
            if (earlyDuckLanding != 0ULL) renderedDuckMask = earlyDuckLanding;
        }
    } else if (snapshot.game.variant == modules::GameVariant::Undecided &&
               modules::IsValidDuckSquare(snapshot.game.initialDuckCandidateSquare)) {
        renderedDuckMask = 1ULL << snapshot.game.initialDuckCandidateSquare;
    }

    for (uint8_t logicalSq = 0; logicalSq < 64; ++logicalSq) {
        const uint8_t physicalSq = logicalSq;

        const uint8_t r = static_cast<uint8_t>(logicalSq / 8);
        const uint8_t f = static_cast<uint8_t>(logicalSq % 8);
        const bool dark = ((r + f) & 1U) != 0U;

        const modules::Rgb squareRgb = dark ? ctx.boostedDarkSquareRgb : ctx.boostedLightSquareRgb;

        const bool occupied = ((snapshot.board.physicalOccupiedMask >> logicalSq) & 1ULL) != 0ULL;
        const bool isDuckPiece =
            occupied && (renderedDuckMask & (1ULL << logicalSq)) != 0ULL;

        const bool isIllegalSquare = !gameOverCelebration &&
                                     ctx.correctionFeedback == modules::CorrectionFeedback::Strong &&
                                     (((ctx.humanIllegalMask | ctx.cpuIllegalMask) >> logicalSq) & 1ULL) != 0ULL;
        const bool isCpuTentativeCorrectionSquare =
            !gameOverCelebration &&
            ((ctx.cpuTentativeCorrectionMask >> logicalSq) & 1ULL) != 0ULL;
        const bool isCaptureReturnSquare = !gameOverCelebration &&
                                           ((ctx.captureReturnMask >> logicalSq) & 1ULL) != 0ULL;
        // The classified invalid-board cue owns its squares. Generic illegal
        // blackout must not create a second, competing animation there.
        const bool exemptFromIllegalBlackout = gameOverCelebration ||
                                               isDuckPiece ||
                                               ((ctx.invalidBoard.Mask() >> logicalSq) & 1ULL) != 0ULL;
        const bool forceOff = (!exemptFromIllegalBlackout && ctx.illegalOn && isIllegalSquare) ||
                              (ctx.captureReturnOn && isCaptureReturnSquare);

        modules::Rgb finalSquare = squareRgb;
        if (ctx.illegalOn && isIllegalSquare) {
            finalSquare = modules::Rgb{255, 64, 64};
        } else if (ctx.captureReturnOn && isCaptureReturnSquare) {
            // An opponent vacancy marks where that piece should be returned.
            const bool opponentVacancy = !occupied &&
                (ctx.expectedOpponentOcc64 & (1ULL << logicalSq)) != 0ULL;
            finalSquare = opponentVacancy ? PieceRgbForOwner(ctx, expectedOpponentOwner)
                                         : modules::RGB_YELLOW;
        }

        modules::Rgb pieceRgb = modules::RGB_BLACK;
        if (!forceOff && occupied) {
            modules::UiOwner cancelUndoOwner = modules::UiOwner::None;
            if (TryGetCancelUndoReappliedOwner(logicalSq, cancelUndoOwner)) {
                pieceRgb = PieceRgbForOwner(ctx, cancelUndoOwner);
            } else if (snapshot.board.enginePiecesValid) {
                modules::LitchMaxEngine::PieceSide side =
                    modules::LitchMaxEngine::PieceSide::None;
                const bool pendingDuckSide =
                    TryGetPendingDuckChessPieceSide(
                        snapshot, logicalSq, side);
                const bool placedRelocationSide =
                    !pendingDuckSide && TryGetPlacedRelocationDestinationSide(
                                            snapshot,
                                            ctx.physicalOcc64 & ~earlyDuckLanding,
                                            logicalSq,
                                            side);
                if (!pendingDuckSide && !placedRelocationSide) {
                    side = SideForFenPiece(
                        snapshot.board.enginePieces[logicalSq]);
                }
                if (side == modules::LitchMaxEngine::PieceSide::None) {
                    (void)TryInferTransitionPieceSide(snapshot, ctx.physicalOcc64, logicalSq, side);
                }

                if (side == modules::LitchMaxEngine::PieceSide::White) {
                    pieceRgb = PieceRgbForPieceSide(ctx, modules::LitchMaxEngine::PieceSide::White);
                } else if (side == modules::LitchMaxEngine::PieceSide::Black) {
                    pieceRgb = PieceRgbForPieceSide(ctx, modules::LitchMaxEngine::PieceSide::Black);
                } else {
                    pieceRgb = finalSquare;
                }
            } else {
                const uint64_t bit = (1ULL << logicalSq);
                if ((ctx.expectedMoverOcc64 & bit) != 0ULL) {
                    pieceRgb = PieceRgbForOwner(ctx, expectedMoverOwner);
                } else if ((ctx.expectedOpponentOcc64 & bit) != 0ULL) {
                    pieceRgb = PieceRgbForOwner(ctx, expectedOpponentOwner);
                } else {
                    pieceRgb = finalSquare;
                }
            }
        }

        if (ctx.minimumBrightness) {
            // Only the ordinary piece layer is reduced. Required setup, undo,
            // correction and directed-move prompts below retain their colors.
            // With the existing304/256 piece and320/256 frame gains, this maps
            // the largest ordinary channels to code1 at global8/256.
            constexpr uint16_t MINIMUM_ORDINARY_PIECE_SCALE256 = 22U;
            pieceRgb = ScaleRgbSaturating(pieceRgb, MINIMUM_ORDINARY_PIECE_SCALE256);
        }

        BoardSquarePromptState promptState;
        ApplyFreshSetupPresentation(ctx,
                                    boardRuntime,
                                    forceOff,
                                    occupied,
                                    isDuckPiece,
                                    logicalSq,
                                    finalSquare,
                                    pieceRgb,
                                    promptState);

        ApplyLastMovePulse(ctx, boardRuntime, forceOff, occupied, logicalSq, finalSquare, pieceRgb);

        if (!gameOverCelebration && !forceOff && occupied &&
            ctx.allPiecesFlashActive && !ctx.allPiecesFlashOn) {
            pieceRgb = modules::RGB_BLACK;
        }

        if (!gameOverCelebration) {
            ApplySquareStatePrompts(ctx,
                                    boardRuntime,
                                    forceOff,
                                    occupied,
                                    logicalSq,
                                    finalSquare,
                                    pieceRgb,
                                    promptState);
        }

        {
            static constexpr uint16_t PIECE_BRIGHTER_SCALE256 = 304; // ~1.19 in Q8.8

            ApplyThreatOverlays(ctx, boardRuntime, forceOff, logicalSq, finalSquare, pieceRgb);

            ApplyDirectedMovePrompt(ctx, boardRuntime, forceOff, occupied, logicalSq, finalSquare, pieceRgb);

            const bool duckRelocationActive =
                snapshot.game.variant == modules::GameVariant::Duck &&
                duckInteraction.plan.kind ==
                    runtime::PhysicalTransitionKind::DuckRelocation &&
                duckInteraction.tag ==
                    runtime::ChessInteractionPhase::DuckRelocation;
            const bool duckRelocationSource =
                duckRelocationActive &&
                duckInteraction.plan.moveFrom == logicalSq;
            const bool fixedDuckRelocationTarget =
                duckRelocationActive &&
                duckInteraction.plan.moveTo < 64U &&
                duckInteraction.plan.moveTo == logicalSq;
            const bool duckAwaitingLift =
                duckRelocationSource &&
                duckInteraction.requiredAction ==
                    runtime::RequiredPhysicalAction::LiftPrimary;
            const bool duckInHand =
                duckRelocationSource &&
                duckInteraction.requiredAction ==
                    runtime::RequiredPhysicalAction::PlacePrimary;
            uint8_t duckAttention = 255U;
            if (!forceOff && duckAwaitingLift) {
                duckAttention = ctx.minimalAnimations ? 255U : modules::animation::SampleValue(
                    boardRuntime.globalAnim,
                    boardRuntime.dangerPulseToken);
                const uint8_t squareAlpha = static_cast<uint8_t>(
                    64U +
                    (static_cast<uint16_t>(duckAttention) * 191U) / 255U);
                finalSquare = modules::AlphaBlendRgb(
                    finalSquare, modules::RGB_YELLOW, squareAlpha);
            } else if (!forceOff && duckInHand) {
                finalSquare = modules::RGB_YELLOW;
            }
            if (!forceOff && fixedDuckRelocationTarget) {
                const uint8_t targetAlpha =
                    duckInteraction.requiredAction ==
                            runtime::RequiredPhysicalAction::PlacePrimary
                        ? 255U
                        : 160U;
                finalSquare = modules::AlphaBlendRgb(
                    finalSquare, modules::RGB_YELLOW, targetAlpha);
            }

            const modules::Rgb finalPiece = forceOff
                ? modules::RGB_BLACK
                : ScaleRgbSaturating(pieceRgb, PIECE_BRIGHTER_SCALE256);
            modules::Rgb adjustedFinalSquare = finalSquare;
            modules::Rgb adjustedFinalPiece = finalPiece;

            if (promptState.pieceFinalScale256 != 256U) {
                adjustedFinalPiece = ScaleRgbSaturating(adjustedFinalPiece, promptState.pieceFinalScale256);
            }

            // The duck is neutral game state, not a FEN chess piece. Keep its
            // center LED independent of either player's configured palette.
            if (isDuckPiece && !forceOff) {
                if (duckAwaitingLift) {
                    const uint16_t pulseScale = static_cast<uint16_t>(
                        48U +
                        (static_cast<uint16_t>(duckAttention) * 208U) /
                            255U);
                    adjustedFinalPiece = ScaleRgbSaturating(
                        modules::RGB_YELLOW, pulseScale);
                } else {
                    adjustedFinalPiece = ctx.minimumBrightness
                        ? ScaleRgbSaturating(modules::RGB_YELLOW, 26U)
                        : modules::RGB_YELLOW;
                }
            }

            if (duckAwaitingLift && !forceOff) {
                // Preserve the pulse shape through the compositor boost. Its
                // extra strength belongs to the existing post-brightness accent,
                // where spare current can brighten it without early RGB clipping.
                constexpr uint16_t PRE_BOOST_SCALE256 = 256U * 256U /
                    led_render_policy::COMPOSER_GLOBAL_BRIGHTNESS_BOOST_SCALE256;
                adjustedFinalSquare = ScaleRgbSaturating(adjustedFinalSquare, PRE_BOOST_SCALE256);
                adjustedFinalPiece = ScaleRgbSaturating(adjustedFinalPiece, PRE_BOOST_SCALE256);
            }

            if (isCpuTentativeCorrectionSquare && !forceOff) {
                adjustedFinalSquare = ScaleRgbSaturating(
                    modules::RGB_ORANGE, 160U);
                if (occupied) {
                    adjustedFinalPiece = modules::AlphaBlendRgb(
                        adjustedFinalPiece, modules::RGB_ORANGE, 192U);
                }
            }

            modules::BoardSquareLeds& ledSlots = boardFrame[physicalSq];
            if (promptState.useMismatchRainbow) {
                modules::ApplyToBoardSquare(ledSlots,
                                            modules::BoardLedSlots::Left,
                                            promptState.mismatchRainbowLeds.left);
                modules::ApplyToBoardSquare(ledSlots,
                                            modules::BoardLedSlots::PiecePair,
                                            promptState.mismatchRainbowLeds.top);
                modules::ApplyToBoardSquare(ledSlots,
                                            modules::BoardLedSlots::Right,
                                            promptState.mismatchRainbowLeds.right);
            } else {
                modules::ApplyToBoardSquare(ledSlots,
                                            modules::BoardLedSlots::SquarePair,
                                            adjustedFinalSquare);
                modules::ApplyToBoardSquare(ledSlots,
                                            modules::BoardLedSlots::PiecePair,
                                            adjustedFinalPiece);
            }
            ApplyKingQueenSwapPairShuttle(
                ctx, logicalSq, occupied, ledSlots);

            // Expected setup placement cannot confirm a piece. Empty centers
            // stay black; specifically designed moving effects compose later.
            if (!occupied) {
                modules::SetPiecePair(ledSlots, modules::RGB_BLACK);
            }
        }
    }

    ApplyDirectedMoveLanding(ctx, outLedFrame);
    ApplyPlacementEmitterEffects(ctx, outLedFrame);
    const uint64_t brightTaps = ApplyPhysicalRemovalBrackets(ctx, outLedFrame);
    for (uint8_t square = 0U; square < 64U; ++square) {
        outGridFrame[square] = modules::RepresentativeSquareColor(boardFrame[square]);
    }
    return brightTaps;
}

} // namespace internal
} // namespace modules::led_frame_composer
