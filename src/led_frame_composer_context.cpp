#include "modules/led_frame_composer_internal.hpp"

#include "runtime/directed_physical_prompt.hpp"
#include <new>

#include "modules/light_square_appearance.hpp"
#include "modules/board_ripple.hpp"
#include "modules/minimal_animations_setting.hpp"
#include "modules/persisted_side.hpp"
#include "modules/player_end_palette.hpp"
#include "modules/ui_color_keys.hpp"
#include "modules/ui_color_presets.hpp"
#include "modules/ui_color_wheel.hpp"
#include "modules/white_squares_setting.hpp"
#include "modules/assistance_preferences.hpp"
#include "modules/turn_anim_style_setting.hpp"

namespace modules::led_frame_composer {

namespace {

modules::PersistedSide OppositePersistedSide(const modules::PersistedSide side) {
    switch (side) {
        case modules::PersistedSide::White:
            return modules::PersistedSide::Black;
        case modules::PersistedSide::Black:
            return modules::PersistedSide::White;
        case modules::PersistedSide::Unknown:
        default:
            return modules::PersistedSide::Unknown;
    }
}

char TryGetFenPieceAtSquare(const runtime::InteractionModelSnapshot& snapshot,
                            const uint8_t square) {
    if (square >= 64U || !snapshot.board.enginePiecesValid) {
        return '\0';
    }
    return snapshot.board.enginePieces[square];
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


namespace internal {

bool CanInitialKingPlacementAssistOwnBoard(
    const bool assistanceEnabled,
    const runtime::InteractionModelSnapshot& snapshot,
    const RenderIntent& renderIntent) {
    static constexpr uint64_t INITIAL_CHESS_OCCUPANCY =
        0xFFFF00000000FFFFULL;
    const bool expectedUntouchedDuck =
        snapshot.game.variant == modules::GameVariant::Duck &&
        snapshot.game.duckSquare < 64U &&
        snapshot.board.expectedOccupiedMask ==
            (INITIAL_CHESS_OCCUPANCY |
             (1ULL << snapshot.game.duckSquare));
    const runtime::ChessInteractionPhase phase =
        snapshot.interaction.taggedState.tag;
    const bool setupOwnsBoard =
        snapshot.game.initialVariantValidationActive ||
        snapshot.interaction.placementModeActive ||
        phase == runtime::ChessInteractionPhase::Idle ||
        phase == runtime::ChessInteractionPhase::Correction;
    const bool coveredByForeground =
        renderIntent.HasCelebration() ||
        renderIntent.menuForegroundVisible ||
        renderIntent.centerTextOverlayVisible ||
        renderIntent.coachingOverlayActive ||
        snapshot.render.allPiecesFlashActive ||
        snapshot.render.bootFadeActive ||
        snapshot.system.safeModeActive ||
        snapshot.system.hardwareTestActive ||
        snapshot.system.amptestActive ||
        snapshot.system.otaInProgress;
    const uint64_t duckMask =
        snapshot.game.variant == modules::GameVariant::Duck &&
                snapshot.game.duckSquare < 64U
            ? (1ULL << snapshot.game.duckSquare)
        : snapshot.game.variant == modules::GameVariant::Undecided &&
                snapshot.game.initialDuckCandidateSquare < 64U
            ? (1ULL << snapshot.game.initialDuckCandidateSquare)
            : 0ULL;
    const uint64_t ordinaryExtraMask =
        snapshot.board.physicalOccupiedMask &
        ~snapshot.board.expectedOccupiedMask & ~duckMask;
    return assistanceEnabled &&
           snapshot.render.initialKingPlacementPromptActive &&
           snapshot.render.initialKingPlacementPromptMask != 0ULL &&
           !coveredByForeground &&
           !snapshot.interaction.kingQueenSwapPromptActive &&
           !snapshot.interaction.undoPromptActive &&
           ordinaryExtraMask == 0ULL &&
           setupOwnsBoard &&
           snapshot.game.setupLifecycle == runtime::SetupLifecycle::FreshSetup &&
           (snapshot.board.expectedOccupiedMask == INITIAL_CHESS_OCCUPANCY ||
            expectedUntouchedDuck);
}

bool CanInitialKingPlacementAssistOwnBoard(
    const fxe::SettingsStore& settings,
    const runtime::InteractionModelSnapshot& snapshot,
    const RenderIntent& renderIntent) {
    return CanInitialKingPlacementAssistOwnBoard(
        settings.ReadValue("BOARD_SETUP_ASSISTANCE", true), snapshot, renderIntent);
}

bool BuildComposeContext(const ComposeInputs& in, ComposeContext& outCtx) {
    outCtx.~ComposeContext();
    ::new (static_cast<void*>(&outCtx)) ComposeContext{};
    outCtx.in = &in;
    outCtx.boardStylePreview = in.boardStylePreview;
    if ((!in.settings && !in.resolvedSettings) || !in.interactionSnapshot) {
        return false;
    }

    outCtx.settings = in.resolvedSettings != nullptr
        ? *in.resolvedSettings : CaptureComposeSettings(*in.settings);
    outCtx.snapshot = in.interactionSnapshot;
    outCtx.nowMs = outCtx.Snapshot().system.nowMs;
    outCtx.firstPlayerIsP2 = in.firstPlayerIsP2;

    const auto& settings = outCtx.settings;
    const auto& snapshot = outCtx.Snapshot();

    outCtx.renderIntent = BuildRenderIntent({.snapshot = &snapshot,
                                             .menuRequested = in.menuActive,
                                             .menuVisualActive = in.menuVisualAlpha255 != 0U,
                                             .hintMoveValid = snapshot.render.hintMoveValid});

    outCtx.minimumBrightness =
        settings.minimumBrightness;

    modules::Rgb lightSquareRgb = modules::ui_color_presets::SQUARE_COLOR_PRESETS[0];
    if (!in.neutralStartupGrid &&
        settings.coloredLightSquares) {
        static constexpr uint16_t RESTRAINED_COLOR_SCALE256 =
            modules::ui_color_presets::SQUARE_COLOR_PRESETS[0].r;
        lightSquareRgb = ScaleRgbSaturating(
            modules::ui_color_wheel::WheelToRgb(
                settings.lightSquareHue),
            RESTRAINED_COLOR_SCALE256);
    }
    {
        const bool whiteSquaresOn = settings.whiteSquaresOn;
        if (!whiteSquaresOn || outCtx.minimumBrightness) {
            lightSquareRgb = modules::RGB_BLACK;
        }
    }

    const uint16_t lightSquareScale256 = static_cast<uint16_t>(
        (in.neutralStartupGrid
             ? modules::light_square_appearance::MAX_BRIGHTNESS
             : settings.lightSquareBrightness) *
        32U);
    outCtx.boostedLightSquareRgb =
        ScaleRgbSaturating(lightSquareRgb, lightSquareScale256);
    outCtx.boostedDarkSquareRgb = modules::RGB_BLACK;

    const uint8_t p1Wheel = settings.nearEndPieceWheel;
    const uint8_t p2Wheel = settings.farEndPieceWheel;

    // Stored wheels belong to fixed near/far ends. Logical players select
    // their assigned end's palette without mutating or exchanging storage.
    outCtx.p1Wheel = in.firstPlayerIsP2 ? p2Wheel : p1Wheel;
    outCtx.p2Wheel = in.firstPlayerIsP2 ? p1Wheel : p2Wheel;

    outCtx.p1PieceRgb = modules::ui_color_wheel::WheelToRgb(outCtx.p1Wheel);
    outCtx.p2PieceRgb = modules::ui_color_wheel::WheelToRgb(outCtx.p2Wheel);

    // Accent colors: small wheel offsets so edge UI can use a "highlight" variant
    // of the player's chosen piece color.
    outCtx.p1AccentRgb = modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(outCtx.p1Wheel + 16U));
    outCtx.p2AccentRgb = modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(outCtx.p2Wheel + 16U));

    outCtx.checkmateAnimationStyle =
        settings.checkmateStyle;
    outCtx.minimalAnimations =
        settings.minimalAnimations;
    outCtx.settleAnimationStyle =
        settings.settleStyle;

    modules::PersistedSide visualSidePersist =
        snapshot.interaction.visualActiveSideOverrideValid
            ? snapshot.interaction.visualActiveSideOverride
            : modules::PersistedSide::Unknown;
    if (visualSidePersist == modules::PersistedSide::Unknown &&
        snapshot.interaction.undoPromptActive) {
        visualSidePersist = OppositePersistedSide(modules::PersistedSideFromSideToMove(snapshot.game.sideToMove));
    }

    const modules::PersistedSide edgeActiveSidePersist =
        snapshot.interaction.statusEdgeActiveSideOverride;
    if (snapshot.interaction.statusEdgeActiveSideOverrideValid) {
        outCtx.edgeActiveOwner = OwnerForPersistedSide(edgeActiveSidePersist, outCtx.firstPlayerIsP2);
    } else {
        outCtx.edgeActiveOwner = modules::led_frame_composer::OwnerForSideToMove(snapshot.game.sideToMove, outCtx.firstPlayerIsP2);
    }

    // Active-owner is the board/prompt owner. Edge turn animation has its own
    // explicit overlay owner so undo transitions do not need render-time guesses.
    outCtx.activeOwner = (visualSidePersist != modules::PersistedSide::Unknown)
                             ? OwnerForPersistedSide(visualSidePersist, outCtx.firstPlayerIsP2)
                             : modules::led_frame_composer::OwnerForSideToMove(snapshot.game.sideToMove, outCtx.firstPlayerIsP2);
    outCtx.activeWheel = (outCtx.activeOwner == modules::UiOwner::P2) ? outCtx.p2Wheel : outCtx.p1Wheel;
    const uint8_t activePromptWheel = static_cast<uint8_t>(outCtx.activeWheel + 32U);
    const uint8_t availablePromptWheel = static_cast<uint8_t>(outCtx.activeWheel + 16U);
    outCtx.activePromptRgb = modules::ui_color_wheel::WheelToRgb(activePromptWheel);
    outCtx.availablePromptRgb = modules::ui_color_wheel::WheelToRgb(availablePromptWheel);

    outCtx.validDestMask = snapshot.interaction.validDestinationMask;
    outCtx.castlingKingDestinationMask =
        snapshot.interaction.castlingKingDestinationMask;
    outCtx.castlingRookSourceMask =
        snapshot.interaction.castlingRookSourceMask;
    const runtime::TaggedInteractionState& interactionState =
        snapshot.interaction.taggedState;
    const runtime::PhysicalTransitionPlan& plan = interactionState.plan;
    outCtx.moveFromSquare = interactionState.tag == runtime::ChessInteractionPhase::PieceLifted
                                ? interactionState.selectedSquare
                                : plan.moveFrom;
    outCtx.moveToSquare = IsSettlingActive(snapshot) &&
                                  snapshot.render.settleSquare < 64U
                              ? snapshot.render.settleSquare
                              : plan.moveTo;

    outCtx.hintMoveValid = snapshot.render.hintMoveValid;
    outCtx.hintFromSquare = snapshot.render.hintMoveFromSquare;
    outCtx.hintToSquare = snapshot.render.hintMoveToSquare;
    outCtx.hintCaptureSquare = snapshot.render.hintCaptureSquare;
    outCtx.hintDuckMoveValid = snapshot.render.hintDuckMoveValid;
    outCtx.hintDuckFromSquare =
        snapshot.render.hintDuckMoveFromSquare;
    outCtx.hintDuckToSquare = snapshot.render.hintDuckMoveToSquare;
    outCtx.coachingOverlayActive = outCtx.renderIntent.coachingOverlayActive;
    if (!outCtx.minimalAnimations && outCtx.renderIntent.hintPromptVisible &&
        IsIdleInteraction(snapshot) && snapshot.render.hintRefinementActive) {
        const uint32_t halfMs = snapshot.render.hintRefinementDurationMs / 2U;
        const uint32_t age = snapshot.render.hintRefinementElapsedMs;
        if (age < halfMs) {
            outCtx.hintFromSquare = snapshot.render.previousHintFromSquare;
            outCtx.hintToSquare = snapshot.render.previousHintToSquare;
            outCtx.hintCaptureSquare = snapshot.render.previousHintCaptureSquare;
            outCtx.hintDuckFromSquare = snapshot.render.previousHintDuckFromSquare;
            outCtx.hintDuckToSquare = snapshot.render.previousHintDuckToSquare;
            outCtx.hintDuckMoveValid = outCtx.hintDuckFromSquare < 64U && outCtx.hintDuckToSquare < 64U;
            outCtx.hintRefinementAlpha255 = static_cast<uint8_t>((halfMs - age) * 255U / halfMs);
        } else {
            outCtx.hintFromSquare = snapshot.render.incomingHintFromSquare;
            outCtx.hintToSquare = snapshot.render.incomingHintToSquare;
            outCtx.hintCaptureSquare = snapshot.render.incomingHintCaptureSquare;
            outCtx.hintDuckFromSquare = snapshot.render.incomingHintDuckFromSquare;
            outCtx.hintDuckToSquare = snapshot.render.incomingHintDuckToSquare;
            outCtx.hintDuckMoveValid = outCtx.hintDuckFromSquare < 64U && outCtx.hintDuckToSquare < 64U;
            outCtx.hintRefinementAlpha255 = static_cast<uint8_t>((age - halfMs) * 255U / halfMs);
        }
    }

    outCtx.settleOn = snapshot.render.settleOn;
    outCtx.settleTimingValid = snapshot.render.settleTimingValid;
    outCtx.settleElapsedMs = snapshot.render.settleElapsedMs;
    outCtx.settleDurationMs = snapshot.render.settleDurationMs;
    outCtx.settleVisualElapsedMs = snapshot.render.settleVisualElapsedMs;
    outCtx.settlePresentationDurationMs =
        snapshot.render.settlePresentationDurationMs;
    outCtx.settleVariant = snapshot.render.settleAnimationVariant;

    {
        const bool longerRainbow = outCtx.settleAnimationStyle == SettleAnimationStyle::RainbowOrbit &&
            !outCtx.minimalAnimations;
        outCtx.settleAfterglowActive = snapshot.render.settleAfterglowActive &&
            (longerRainbow || snapshot.render.settleAfterglowElapsedMs < board_ripple::PLACEMENT_DURATION_MS);
        outCtx.settleAfterglowSquare = snapshot.render.settleAfterglowSquare;
        outCtx.settleAfterglowElapsedMs = snapshot.render.settleAfterglowElapsedMs;

        // Settle afterglow color follows the mover's piece color.
        // If the settle was aborted (piece lifted back off the destination square),
        // the engine has not advanced; the mover is the current side-to-move.
        // Otherwise, the engine has advanced by the time the afterglow frame is
        // composed, so the mover is opposite the current side-to-move.
        modules::SideToMove moverSide = snapshot.render.settleMoverSideKnown
            ? (snapshot.render.settleMoverIsBlack
                   ? modules::SideToMove::Black
                   : modules::SideToMove::White)
            : snapshot.game.sideToMove;
        const bool stagedDuckChessMove =
            snapshot.game.variant == modules::GameVariant::Duck &&
            snapshot.game.duckTurnPhase ==
                modules::DuckTurnPhase::DuckRelocation &&
            snapshot.interaction.taggedState.plan.kind ==
                runtime::PhysicalTransitionKind::DuckRelocation;
        const runtime::TaggedInteractionState& interactionState =
            snapshot.interaction.taggedState;
        const bool uncommittedCastlingRelocation =
            interactionState.tag ==
                runtime::ChessInteractionPhase::ExecutingPlan &&
            interactionState.plan.kind ==
                runtime::PhysicalTransitionKind::Castling &&
            (interactionState.plan.source ==
                 runtime::PhysicalTransitionSource::Human ||
                 runtime::IsCpuDirectedTransition(interactionState.plan)) &&
            !interactionState.plan.progress.actionAcknowledged;
        if (!snapshot.render.settleMoverSideKnown &&
            !snapshot.render.settleAfterglowAbort &&
            !stagedDuckChessMove && !uncommittedCastlingRelocation) {
            moverSide = (snapshot.game.sideToMove == modules::SideToMove::Black)
                            ? modules::SideToMove::White
                            : modules::SideToMove::Black;
        }
        // A pickup return or the first castling relocation can start a new
        // bloom without starting another settle. Its occupied piece/plan owns
        // the color, not the cached mover of a preceding completed turn. An
        // empty aborted destination still needs that retained settle identity.
        const uint8_t afterglowSquare = snapshot.render.settleAfterglowSquare;
        if (afterglowSquare < 64U &&
            (snapshot.board.physicalOccupiedMask &
             (UINT64_C(1) << afterglowSquare)) != 0ULL) {
            auto pieceSide = modules::LitchMaxEngine::PieceSide::None;
            if (!TryGetPlacedRelocationDestinationSide(
                    snapshot, snapshot.board.physicalOccupiedMask,
                    afterglowSquare, pieceSide)) {
                pieceSide = SideForFenPiece(
                    TryGetFenPieceAtSquare(snapshot, afterglowSquare));
            }
            if (pieceSide != modules::LitchMaxEngine::PieceSide::None) {
                moverSide = pieceSide == modules::LitchMaxEngine::PieceSide::Black
                                ? modules::SideToMove::Black
                                : modules::SideToMove::White;
            }
        }
        const modules::UiOwner moverOwner = modules::led_frame_composer::OwnerForSideToMove(moverSide, outCtx.firstPlayerIsP2);
        outCtx.settleAfterglowRgb = PieceRgbForOwner(outCtx, moverOwner);
    }

    {
        // Last move per player (P1/P2). These are stored by persisted side.
        const modules::PersistedSide p1Side = modules::PersistedSide::White;
        const modules::PersistedSide p2Side = modules::PersistedSide::Black;

        if (p1Side == modules::PersistedSide::White) {
            outCtx.p1LastMoveValid = snapshot.render.whiteLastMoveValid;
            outCtx.p1LastMoveFrom = snapshot.render.whiteLastMoveFromSquare;
            outCtx.p1LastMoveTo = snapshot.render.whiteLastMoveToSquare;
        } else {
            outCtx.p1LastMoveValid = snapshot.render.blackLastMoveValid;
            outCtx.p1LastMoveFrom = snapshot.render.blackLastMoveFromSquare;
            outCtx.p1LastMoveTo = snapshot.render.blackLastMoveToSquare;
        }

        if (p2Side == modules::PersistedSide::White) {
            outCtx.p2LastMoveValid = snapshot.render.whiteLastMoveValid;
            outCtx.p2LastMoveFrom = snapshot.render.whiteLastMoveFromSquare;
            outCtx.p2LastMoveTo = snapshot.render.whiteLastMoveToSquare;
        } else {
            outCtx.p2LastMoveValid = snapshot.render.blackLastMoveValid;
            outCtx.p2LastMoveFrom = snapshot.render.blackLastMoveFromSquare;
            outCtx.p2LastMoveTo = snapshot.render.blackLastMoveToSquare;
        }

        outCtx.lastMoveMostRecentKnown = snapshot.render.lastMoveMostRecentKnown;
        outCtx.lastMoveMostRecentIsP1 = snapshot.render.lastMoveMostRecentKnown &&
                                        snapshot.render.lastMoveMostRecentSide == p1Side;
    }

    outCtx.undoPromptActive = snapshot.interaction.undoPromptActive;
    outCtx.undoFromSquare = snapshot.interaction.undoFromSquare;
    outCtx.undoToSquare = snapshot.interaction.undoToSquare;
    outCtx.undoMoverSideKnown = snapshot.interaction.undoMoverSideKnown;
    outCtx.undoMoverSidePersist = snapshot.interaction.undoMoverSide;
    outCtx.undoRestoreValid = snapshot.interaction.undoRestoreValid;
    outCtx.undoRestoreSquare = snapshot.interaction.undoRestoreSquare;
    outCtx.undoRestorePieceFen = snapshot.interaction.undoRestorePieceFen;
    outCtx.undoMoverPieceFenKnown = snapshot.interaction.undoMoverPieceFenKnown;
    outCtx.undoMoverPieceFen = snapshot.interaction.undoMoverPieceFen;

    if (outCtx.undoPromptActive && !outCtx.undoMoverPieceFenKnown) {
        outCtx.undoMoverPieceFen = TryGetFenPieceAtSquare(snapshot, outCtx.undoToSquare);
        outCtx.undoMoverPieceFenKnown = (outCtx.undoMoverPieceFen != '\0');
    }

    outCtx.undoMoverPieceRgb = modules::RGB_WHITE;
    outCtx.undoMoverPromptRgb = modules::RGB_WHITE;
    modules::UiOwner undoMoverOwner = modules::UiOwner::None;
    if (outCtx.undoPromptActive) {
        if (outCtx.undoMoverSideKnown) {
            undoMoverOwner = OwnerForPersistedSide(outCtx.undoMoverSidePersist, outCtx.firstPlayerIsP2);
            outCtx.undoMoverPieceRgb = PieceRgbForOwner(outCtx, undoMoverOwner);
        } else if (outCtx.undoToSquare < 64U) {
            switch (SideForFenPiece(TryGetFenPieceAtSquare(snapshot, outCtx.undoToSquare))) {
                case modules::LitchMaxEngine::PieceSide::White:
                    undoMoverOwner = OwnerForPieceSide(modules::LitchMaxEngine::PieceSide::White, outCtx.firstPlayerIsP2);
                    outCtx.undoMoverPieceRgb = PieceRgbForPieceSide(outCtx, modules::LitchMaxEngine::PieceSide::White);
                    break;
                case modules::LitchMaxEngine::PieceSide::Black:
                    undoMoverOwner = OwnerForPieceSide(modules::LitchMaxEngine::PieceSide::Black, outCtx.firstPlayerIsP2);
                    outCtx.undoMoverPieceRgb = PieceRgbForPieceSide(outCtx, modules::LitchMaxEngine::PieceSide::Black);
                    break;
                case modules::LitchMaxEngine::PieceSide::None:
                default:
                    break;
            }
        }

        if (undoMoverOwner == modules::UiOwner::None && visualSidePersist != modules::PersistedSide::Unknown) {
            undoMoverOwner = OwnerForPersistedSide(visualSidePersist, outCtx.firstPlayerIsP2);
            outCtx.undoMoverPieceRgb = PieceRgbForOwner(outCtx, undoMoverOwner);
        }

        if (undoMoverOwner != modules::UiOwner::None) {
            const uint8_t ownerWheel = (undoMoverOwner == modules::UiOwner::P2) ? outCtx.p2Wheel : outCtx.p1Wheel;
            outCtx.undoMoverPromptRgb = modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(ownerWheel + 32U));
        }
    }

    outCtx.undoRestorePieceRgb = modules::RGB_WHITE;
    outCtx.undoRestorePromptRgb = modules::RGB_WHITE;
    if (outCtx.undoRestoreValid) {
        const bool restoreIsBlack = (outCtx.undoRestorePieceFen >= 'a' && outCtx.undoRestorePieceFen <= 'z');
        const modules::UiOwner restoreOwner =
            restoreIsBlack
                ? OwnerForPieceSide(modules::LitchMaxEngine::PieceSide::Black, outCtx.firstPlayerIsP2)
                : OwnerForPieceSide(modules::LitchMaxEngine::PieceSide::White, outCtx.firstPlayerIsP2);
        outCtx.undoRestorePieceRgb = PieceRgbForOwner(outCtx, restoreOwner);
        if (restoreOwner != modules::UiOwner::None) {
            const uint8_t ownerWheel = (restoreOwner == modules::UiOwner::P2) ? outCtx.p2Wheel : outCtx.p1Wheel;
            outCtx.undoRestorePromptRgb = modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(ownerWheel + 32U));
        }
    }

    outCtx.undoMoverReturned = false;
    if (outCtx.undoPromptActive && outCtx.undoFromSquare < 64U && outCtx.undoToSquare < 64U) {
        outCtx.undoMoverReturned =
            ((snapshot.board.physicalOccupiedMask >> outCtx.undoFromSquare) & 1ULL) != 0ULL &&
            ((snapshot.board.physicalOccupiedMask >> outCtx.undoToSquare) & 1ULL) == 0ULL;
    }

    outCtx.undoRestorePhase = outCtx.undoPromptActive && outCtx.undoRestoreValid && outCtx.undoMoverReturned;
    outCtx.undoMoverPhase = outCtx.undoPromptActive && !outCtx.undoRestorePhase;

    outCtx.kingQueenSwapPromptActive = false;
    outCtx.kingQueenSwapPromptMask = 0;
    outCtx.kingQueenSwapPromptFlashMask = 0;
    const uint64_t kqMask64 = snapshot.interaction.kingQueenSwapPromptMask;
    const bool kqPromptReplacedBySystemStatus =
        runtime::IsRuntimeHealthPlayPaused(in.runtimeHealth) ||
        in.higherPriorityStatusOverlayActive ||
        snapshot.system.safeModeActive ||
        snapshot.system.hardwareTestActive ||
        snapshot.system.amptestActive ||
        snapshot.system.otaInProgress ||
        outCtx.renderIntent.HasCelebration();
    if (snapshot.interaction.kingQueenSwapPromptActive &&
        kqMask64 != 0ULL && !kqPromptReplacedBySystemStatus) {
        outCtx.kingQueenSwapPromptActive = true;
        // Snapshot square masks are always logical. ComposeBoard performs the
        // one logical-to-physical rotation at the output boundary.
        outCtx.kingQueenSwapPromptMask = kqMask64;

        const uint64_t kqFlash64 = snapshot.interaction.kingQueenSwapPromptFlashMask;
        // A zero pending mask is the truthful 200 ms completion dwell. Do not
        // relight all four squares while stability is being confirmed.
        outCtx.kingQueenSwapPromptFlashMask = kqFlash64;
    }

    outCtx.promoMoveValid = IsHumanPromotionActive(snapshot.interaction);
    outCtx.promoFromSquare = outCtx.promoMoveValid ? plan.moveFrom : 255U;
    outCtx.promoToSquare = outCtx.promoMoveValid ? plan.moveTo : 255U;
    outCtx.promoMoverIsBlack = snapshot.game.sideToMove == modules::SideToMove::Black;
    const modules::UiOwner promoMoverOwner =
        outCtx.promoMoverIsBlack
            ? OwnerForPieceSide(modules::LitchMaxEngine::PieceSide::Black, outCtx.firstPlayerIsP2)
            : OwnerForPieceSide(modules::LitchMaxEngine::PieceSide::White, outCtx.firstPlayerIsP2);
    outCtx.promoMoverPieceRgb = PieceRgbForOwner(outCtx, promoMoverOwner);

    outCtx.settleMoverPieceRgb = modules::RGB_WHITE;
    if (IsSettlingActive(snapshot)) {
        // Settling flash color must be stable for the full animation.
        // Production latches the mover before acknowledgement. The fallback
        // below serves snapshots without authoritative chess-side metadata.
        const modules::UiOwner sideToMoveOwner =
            modules::led_frame_composer::OwnerForSideToMove(snapshot.game.sideToMove, outCtx.firstPlayerIsP2);
        const runtime::TaggedInteractionState& interactionState =
            snapshot.interaction.taggedState;
        const bool acknowledgedHumanSettle =
            snapshot.render.humanMoveSettlingActive &&
            (interactionState.tag == runtime::ChessInteractionPhase::Idle ||
             interactionState.plan.progress.actionAcknowledged);
        const modules::UiOwner moverOwner =
            snapshot.render.settleMoverSideKnown
                ? modules::led_frame_composer::OwnerForSideToMove(
                      snapshot.render.settleMoverIsBlack
                          ? modules::SideToMove::Black
                          : modules::SideToMove::White,
                      outCtx.firstPlayerIsP2)
                : ((snapshot.render.cpuMoveSettlingActive ||
                    acknowledgedHumanSettle)
                       ? ((sideToMoveOwner == modules::UiOwner::P2)
                              ? modules::UiOwner::P1
                              : modules::UiOwner::P2)
                       : sideToMoveOwner);
        outCtx.settleMoverPieceRgb = PieceRgbForOwner(outCtx, moverOwner);
    }

    outCtx.pendingCaptureSquareValid =
        IsPendingHumanEnPassantRemoval(snapshot.interaction) &&
        plan.removalSquare < 64U;
    outCtx.pendingCaptureSquare = outCtx.pendingCaptureSquareValid
                                      ? plan.removalSquare
                                      : 255U;
    outCtx.cpuTargetOccValid = IsCpuMovePromptActive(snapshot.interaction);
    outCtx.physicalOcc64 = snapshot.board.physicalOccupiedMask;
    outCtx.expectedOcc64 = snapshot.board.expectedOccupiedMask;

    outCtx.expectedMoverOcc64 = snapshot.board.moverOccupiedMask;
    outCtx.expectedOpponentOcc64 = snapshot.board.opponentOccupiedMask;

    static constexpr uint64_t INITIAL_CHESS_OCCUPANCY =
        0xFFFF00000000FFFFULL;
    const bool expectedUntouchedDuck =
        snapshot.game.variant == modules::GameVariant::Duck &&
        snapshot.game.duckSquare < 64U &&
        outCtx.expectedOcc64 ==
            (INITIAL_CHESS_OCCUPANCY | (1ULL << snapshot.game.duckSquare));
    const runtime::ChessInteractionPhase phase =
        snapshot.interaction.taggedState.tag;
    const bool setupOwnsBoard =
        snapshot.game.initialVariantValidationActive ||
        snapshot.interaction.placementModeActive ||
        snapshot.interaction.kingQueenSwapPromptActive ||
        phase == runtime::ChessInteractionPhase::Idle ||
        phase == runtime::ChessInteractionPhase::Correction;
    const bool freshLifecycleOwnsBoard =
        !outCtx.renderIntent.HasCelebration() && setupOwnsBoard &&
        snapshot.game.setupLifecycle == runtime::SetupLifecycle::FreshSetup &&
        (outCtx.expectedOcc64 == INITIAL_CHESS_OCCUPANCY ||
         expectedUntouchedDuck);
    const uint64_t rawMismatch =
        outCtx.physicalOcc64 ^ outCtx.expectedOcc64;
    outCtx.freshSetupActive =
        freshLifecycleOwnsBoard && rawMismatch != 0ULL;

    const bool activeCastlingPlan =
        runtime::HasActivePhysicalPlan(snapshot.interaction) &&
        plan.kind == runtime::PhysicalTransitionKind::Castling &&
        plan.relocationCount == 2U;
    outCtx.partialCastlingTargetOcc64 = 0ULL;
    runtime::DirectedPhysicalPrompt directedCastlingPrompt;
    if (activeCastlingPlan &&
        runtime::TryGetDirectedPhysicalPrompt(
            interactionState, directedCastlingPrompt)) {
        outCtx.partialCastlingTargetOcc64 =
            1ULL << directedCastlingPrompt.toSquare;
    }
    outCtx.partialCastlingTargetOccValid =
        activeCastlingPlan &&
        outCtx.physicalOcc64 != plan.beforeOccupiedMask &&
        outCtx.physicalOcc64 != plan.afterOccupiedMask;
    // Background analysis never owns or suppresses physical correction cues.
    outCtx.humanIllegalMask = snapshot.interaction.humanIllegalMask;
    outCtx.cpuIllegalMask = snapshot.interaction.cpuIllegalMask;
    outCtx.cpuTentativeCorrectionMask = snapshot.interaction.cpuTentativeCorrectionMask;
    outCtx.illegalOn = snapshot.interaction.illegalOn;
    outCtx.illegalElapsedMs = snapshot.interaction.illegalElapsedMs;
    outCtx.illegalPeriodMs = snapshot.interaction.illegalPeriodMs;
    outCtx.kingSafetyPromptActive = snapshot.interaction.kingSafetyPromptActive;
    outCtx.kingSafetyKingSquare = snapshot.interaction.kingSafetyKingSquare;
    outCtx.kingSafetyAttackerSquare = snapshot.interaction.kingSafetyAttackerSquare;
    outCtx.captureReturnMask = snapshot.interaction.captureReturnMask;
    outCtx.captureReturnOn = snapshot.interaction.captureReturnOn;
    outCtx.captureReturnElapsedMs = snapshot.interaction.captureReturnElapsedMs;

    outCtx.allPiecesFlashActive = snapshot.render.allPiecesFlashActive;
    outCtx.allPiecesFlashOn = snapshot.render.allPiecesFlashOn;

    outCtx.invalidBoard = ComputeInvalidBoardState(outCtx);
    const bool interactionAnchoredMismatch =
        outCtx.invalidBoard.IsActive() &&
        !snapshot.interaction.placementModeActive &&
        outCtx.humanIllegalMask == 0ULL &&
        outCtx.cpuIllegalMask == 0ULL;
    outCtx.invalidBoard.elapsedMs = interactionAnchoredMismatch
                                        ? snapshot.interaction.lastInteractionElapsedMs
                                        : outCtx.illegalElapsedMs;

    ConfigureCorrectionFeedback(outCtx);

    outCtx.freshSetupIllegalPickup =
        ((outCtx.humanIllegalMask | outCtx.cpuIllegalMask) &
         outCtx.invalidBoard.missingMask) != 0ULL;
    outCtx.initialKingPlacementPromptActive =
        CanInitialKingPlacementAssistOwnBoard(
            settings.initialKingPlacementAssistance, snapshot, outCtx.renderIntent) &&
        !outCtx.freshSetupIllegalPickup &&
        !runtime::IsRuntimeHealthPlayPaused(in.runtimeHealth) &&
        !in.higherPriorityStatusOverlayActive;
    outCtx.initialKingPlacementPromptMask =
        outCtx.initialKingPlacementPromptActive
            ? snapshot.render.initialKingPlacementPromptMask
            : 0ULL;
    outCtx.initialKingPlacementPromptElapsedMs =
        outCtx.initialKingPlacementPromptActive
            ? snapshot.render.initialKingPlacementPromptElapsedMs
            : 0U;

    return true;
}

} // namespace internal
} // namespace modules::led_frame_composer
