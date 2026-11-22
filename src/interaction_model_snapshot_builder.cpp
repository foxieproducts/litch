#include "runtime/interaction_model_snapshot_builder.hpp"

#include "runtime/chess_castling_eligibility.hpp"
#include "runtime/game_state_machine.hpp"

#include <cstring>

#ifdef ARDUINO
#include "firmware/firmware_tasks_internal.hpp"
#endif

namespace runtime {
namespace {

bool ParseFenPieces(const char* fen, std::array<char, 64>& outPieces) {
    outPieces.fill('\0');
    if (fen == nullptr || fen[0] == '\0') {
        return false;
    }
    uint8_t rank = 0U;
    uint8_t file = 0U;
    for (const char* p = fen; *p != '\0' && *p != ' '; ++p) {
        if (*p == '/') {
            if (file != 8U || rank >= 7U) {
                return false;
            }
            ++rank;
            file = 0U;
        } else if (*p >= '1' && *p <= '8') {
            file = static_cast<uint8_t>(file + static_cast<uint8_t>(*p - '0'));
            if (file > 8U) {
                return false;
            }
        } else {
            const bool piece = (*p >= 'A' && *p <= 'Z') ||
                               (*p >= 'a' && *p <= 'z');
            if (!piece || rank >= 8U || file >= 8U) {
                return false;
            }
            outPieces[static_cast<uint8_t>(rank * 8U + file)] = *p;
            ++file;
        }
    }
    return rank == 7U && file == 8U;
}

} // namespace

InteractionModelSnapshot BuildInteractionModelSnapshot(const InteractionModelSnapshotBuildInputs& inputs) {
    InteractionModelSnapshot snapshot;
    if (inputs.stateMachine == nullptr || inputs.derived == nullptr) {
        return snapshot;
    }

    const auto& stateMachine = *inputs.stateMachine;
    const auto& derived = *inputs.derived;
    const auto& renderState = stateMachine.GetRenderState();
    snapshot.render.turnPresentationValid = renderState.turnIndicator.initialized;
    snapshot.render.turnScale256 = renderState.turnIndicator.Scale256();
    snapshot.render.turnElapsedMs = renderState.turnIndicator.motion.ElapsedMs();
    const uint32_t nowMs = fxe::Millis();

    snapshot.system.nowMs = nowMs;

    snapshot.board.physicalOccupiedMask = stateMachine.GetPhysicalOccupancyMask();
    snapshot.board.expectedOccupiedMask = stateMachine.GetExpectedOccupancyMask();
    snapshot.board.moverOccupiedMask = stateMachine.GetExpectedMoverOccupancyMask();
    snapshot.board.opponentOccupiedMask = stateMachine.GetExpectedOpponentOccupancyMask();
    if (inputs.authoritativeFen != nullptr) {
        snapshot.board.enginePiecesValid = ParseFenPieces(
            inputs.authoritativeFen, snapshot.board.enginePieces);
    }

    snapshot.interaction.validDestinationMask = stateMachine.GetValidDestMask();
    snapshot.interaction.captureFirstCapturerMask = inputs.captureFirstCapturerMask;
    const ChessInteractionModel& chessInteraction =
        stateMachine.GetChessInteractionModel();
    if (chessInteraction.phase.tag == ChessInteractionPhase::PieceLifted) {
        const CastlingPickupIntent castlingIntent = BuildCastlingPickupIntent(
            chessInteraction.acknowledged,
            chessInteraction.phase.selectedSquare,
            snapshot.interaction.validDestinationMask);
        snapshot.interaction.castlingKingDestinationMask =
            castlingIntent.kingDestinationMask;
        snapshot.interaction.castlingRookSourceMask =
            castlingIntent.rookSourceMask;
    }
    snapshot.interaction.humanIllegalMask = stateMachine.GetHumanIllegalMask();
    snapshot.interaction.cpuIllegalMask = stateMachine.GetCpuIllegalMask();
    snapshot.interaction.cpuTentativeCorrectionMask =
        stateMachine.GetCpuTentativeCorrectionMask();
    snapshot.interaction.undoPromptActive = stateMachine.IsUndoPromptActive();
    snapshot.interaction.kingQueenSwapPromptActive = stateMachine.IsKingQueenSwapPromptActive();
    snapshot.interaction.placementModeActive = stateMachine.IsPlacementModeActive();
    snapshot.interaction.placementMissingMask = stateMachine.GetPlacementMissingMask();
    snapshot.interaction.placementExtraMask = stateMachine.GetPlacementExtraMask();
    snapshot.interaction.interactionStartedFromMismatch =
        stateMachine.DidCurrentInteractionStartFromMismatch();
    snapshot.interaction.taggedState = chessInteraction.phase;
    if (chessInteraction.phase.tag == ChessInteractionPhase::AwaitingAction &&
        (chessInteraction.phase.plan.source == PhysicalTransitionSource::Human ||
         IsCpuDirectedTransition(chessInteraction.phase.plan))) {
        const ChessPositionState projected = ProjectChessPositionAfterPlan(
            chessInteraction.acknowledged, chessInteraction.phase.plan);
        const uint64_t removedMover =
            ~chessInteraction.board.occupiedMask & projected.moverOccupiedMask;
        if (removedMover != 0ULL &&
            (removedMover & (removedMover - 1ULL)) == 0ULL) {
            snapshot.interaction.deferredFollowupMask = removedMover;
            const uint64_t difference =
                chessInteraction.board.occupiedMask ^ projected.occupiedMask;
            const uint64_t placed = difference & chessInteraction.board.occupiedMask;
            if (placed != 0ULL && (placed & (placed - 1ULL)) == 0ULL &&
                difference == (removedMover | placed)) {
                // A complete next-side relocation may precede this result too.
                // Keep it neutral until acknowledgement replays the observation;
                // the ordinary action still owns authoritative move validation.
                snapshot.interaction.deferredFollowupMask |= placed;
            }
        }
    }

    snapshot.interaction.visualActiveSideOverrideValid =
        stateMachine.TryGetVisualActiveSideOverride(
            snapshot.interaction.visualActiveSideOverride);
    snapshot.interaction.statusEdgeActiveSideOverrideValid =
        stateMachine.TryGetStatusEdgeActiveSideOverride(
            snapshot.interaction.statusEdgeActiveSideOverride);

    snapshot.render.settleSquare = stateMachine.GetSettleSquare();
    snapshot.render.settleOn = stateMachine.IsSettleFlashOn();
    snapshot.render.settleTimingValid =
        stateMachine.TryGetSettleElapsedAndDurationMs(
            snapshot.render.settleElapsedMs,
            snapshot.render.settleDurationMs);
    snapshot.render.settleVisualElapsedMs =
        renderState.settleVisualElapsed.ElapsedMs();
    snapshot.render.promotionArrivalSquare = renderState.promotionArrivalSquare;
    snapshot.render.settlePresentationDurationMs =
        stateMachine.GetSettlePresentationDurationMs();
    snapshot.render.settleMoverSideKnown = renderState.settleMoverSideKnown;
    snapshot.render.settleMoverIsBlack = renderState.settleMoverIsBlack;
    snapshot.render.cpuMoveSettlingActive =
        stateMachine.IsCpuMoveSettlingActive();
    snapshot.render.humanMoveSettlingActive =
        stateMachine.IsMoveSettlingActive() &&
        !snapshot.render.cpuMoveSettlingActive;
    snapshot.render.moveTransition = renderState.moveTransition;

    snapshot.interaction.undoPromptElapsedMs =
        stateMachine.GetUndoPromptElapsedMs();
    if (stateMachine.TryGetUndoPromptMove(
            snapshot.interaction.undoFromSquare,
            snapshot.interaction.undoToSquare)) {
        snapshot.interaction.undoPromptActive = true;
    }
    snapshot.interaction.undoMoverSideKnown =
        stateMachine.TryGetUndoPromptMoverSide(
            snapshot.interaction.undoMoverSide);
    snapshot.interaction.undoRestoreValid =
        stateMachine.TryGetUndoRestoreInfo(
            snapshot.interaction.undoRestoreSquare,
            snapshot.interaction.undoRestorePieceFen);
    snapshot.interaction.undoMoverPieceFenKnown =
        stateMachine.TryGetUndoPromptMoverPieceFen(
            snapshot.interaction.undoMoverPieceFen);
    snapshot.interaction.undoCastlingRookMoveValid =
        stateMachine.TryGetUndoPromptCastlingRookMove(
            snapshot.interaction.undoCastlingRookFrom,
            snapshot.interaction.undoCastlingRookTo);

    uint64_t promptMask = 0ULL;
    if (stateMachine.TryGetKingQueenSwapPromptMask(promptMask)) {
        snapshot.interaction.kingQueenSwapPromptMask = promptMask;
    }
    if (stateMachine.TryGetKingQueenSwapPromptFlashMask(promptMask)) {
        snapshot.interaction.kingQueenSwapPromptFlashMask = promptMask;
    }

    snapshot.interaction.promotionElapsedMs = stateMachine.GetPromotionElapsedMs();
    snapshot.interaction.promotionSelection =
        stateMachine.GetPromotionSelection();
    snapshot.interaction.cpuPromotionValid =
        stateMachine.TryGetQueuedCpuPromotionSuffix(
            snapshot.interaction.cpuPromotionSuffixLower);
    snapshot.interaction.cpuMovePromptElapsedMs =
        stateMachine.GetCpuMovePromptElapsedMs();
    snapshot.interaction.illegalOn = stateMachine.IsIllegalFlashOn();
    snapshot.interaction.illegalElapsedMs =
        stateMachine.GetIllegalFlashElapsedMs();
    snapshot.interaction.illegalPeriodMs =
        stateMachine.GetIllegalFlashPeriodMs();
    snapshot.interaction.kingSafetyPromptActive =
        stateMachine.TryGetKingSafetyPrompt(
            snapshot.interaction.kingSafetyKingSquare,
            snapshot.interaction.kingSafetyAttackerSquare);
    snapshot.interaction.captureReturnMask =
        stateMachine.GetCaptureReturnMask();
    snapshot.interaction.captureReturnOn =
        stateMachine.IsCaptureReturnFlashOn();
    snapshot.interaction.captureReturnElapsedMs =
        stateMachine.GetCaptureReturnFlashElapsedMs();
    snapshot.interaction.lastInteractionElapsedMs =
        stateMachine.GetLastInteractionElapsedMs();

    snapshot.game.variant = inputs.variant;
    snapshot.game.duckSquare = inputs.duckSquare;
    snapshot.game.initialDuckCandidateSquare =
        inputs.initialDuckCandidateSquare;
    snapshot.game.initialVariantValidationActive =
        inputs.initialVariantValidationActive;
    snapshot.game.setupLifecycle = inputs.setupLifecycle;
    snapshot.game.duckTurnPhase = inputs.duckTurnPhase;
    snapshot.game.variantOutcome = inputs.variantOutcome;
    snapshot.game.variantTerminalStartMs = inputs.variantTerminalStartMs;
    snapshot.game.kingResignationOwner = inputs.kingResignationOwner;
    snapshot.game.kingResignationSquare = inputs.kingResignationSquare;
    snapshot.game.kingResignationElapsedMs = inputs.kingResignationElapsedMs;
    snapshot.game.kingResignationArmed = inputs.kingResignationArmed;
    snapshot.game.sideToMove = derived.sideToMove;
    snapshot.game.cpuThinking = stateMachine.IsCpuThinkingActive();
    snapshot.game.hintThinking = renderState.hintThinkingActive;
    snapshot.game.inCheck = derived.inCheck;
    snapshot.game.inCheckmate = derived.inCheckmate;
    snapshot.game.inDraw = derived.inDraw;
    snapshot.game.inStalemate = derived.inStalemate;
    snapshot.game.threatenedKingSquareValid = derived.threatenedKingSquareValid;
    snapshot.game.threatenedKingSquare = derived.threatenedKingSquare;
    snapshot.game.checkingPiecesMask = derived.checkingPiecesMask;
    snapshot.game.checkStartValid = derived.checkStartValid;
    snapshot.game.checkStartMs = derived.checkStartMs;
    snapshot.game.checkmateStartValid = derived.checkmateStartValid;
    snapshot.game.checkmateStartMs = derived.checkmateStartMs;
    snapshot.game.drawStartValid = derived.drawStartValid;
    snapshot.game.drawStartMs = derived.drawStartMs;
    snapshot.game.drawReason = derived.drawReason;
    snapshot.game.clockWinner = inputs.clockWinner;
    snapshot.game.clockTerminalStartMs = inputs.clockTerminalStartMs;
    if (inputs.gameClock != nullptr) {
        snapshot.game.clockEnabled = inputs.gameClock->IsEnabled();
        snapshot.game.clockRunning = inputs.gameClock->IsRunning();
        snapshot.game.clockPaused = inputs.gameClock->IsPaused();
        snapshot.game.clockActiveSide = inputs.gameClock->GetActiveSide();
        snapshot.game.clockWhiteRemainingMs =
            inputs.gameClock->GetRemainingMs(ClockSide::White);
        snapshot.game.clockBlackRemainingMs =
            inputs.gameClock->GetRemainingMs(ClockSide::Black);
    }

    snapshot.render.thinkingControlOwner = renderState.thinkingControls.Owner();
    snapshot.render.thinkingControlScale256 = renderState.thinkingControls.Scale256();
    snapshot.render.thinkingControlElapsedMs = renderState.thinkingControls.ElapsedMs();
    snapshot.render.thinkingControlCpu = renderState.thinkingControlsCpu;
    snapshot.render.hintBrewingOwner = renderState.hintBrewing.Owner();
    snapshot.render.hintBrewingScale256 = renderState.hintBrewing.Scale256();
    snapshot.render.hintBrewingElapsedMs = renderState.hintBrewing.ElapsedMs();
    snapshot.render.manualHintAcknowledgmentValid = renderState.manualHintAcknowledgmentValid;
    snapshot.render.hintDisplayRequested = renderState.hintDisplayRequested;
    snapshot.render.hintFeedbackPending = renderState.hintFeedbackPending;
    snapshot.render.hintFeedbackTimingValid = renderState.hintFeedbackElapsed.Started();
    snapshot.render.hintFeedbackElapsedMs = renderState.hintFeedbackElapsed.ElapsedMs();
    snapshot.render.hintMoveValid = renderState.hintMoveValid;
    snapshot.render.hintMoveFromSquare = renderState.hintMoveFromSquare;
    snapshot.render.hintMoveToSquare = renderState.hintMoveToSquare;
    const auto captureSquare = [&](const uint8_t from, const uint8_t to) {
        const auto& position = chessInteraction.acknowledged;
        if (from < 64U && to < 64U &&
            (position.moverOccupiedMask & (1ULL << from)) != 0ULL) {
            if ((position.opponentOccupiedMask & (1ULL << to)) != 0ULL) return to;
            if ((position.pawnMask & (1ULL << from)) != 0ULL &&
                to == position.enPassantTargetSquare &&
                position.enPassantCaptureSquare < 64U &&
                (position.opponentOccupiedMask &
                 (1ULL << position.enPassantCaptureSquare)) != 0ULL) {
                return position.enPassantCaptureSquare;
            }
        }
        return CHESS_INTERACTION_INVALID_SQUARE;
    };
    if (renderState.hintMoveValid) {
        snapshot.render.hintCaptureSquare = captureSquare(
            renderState.hintMoveFromSquare, renderState.hintMoveToSquare);
    }
    const auto& refinement = renderState.hintRefinement;
    snapshot.render.hintRefinementActive = refinement.elapsed.Started() &&
        refinement.elapsed.ElapsedMs() < refinement.durationMs;
    snapshot.render.hintRefinementElapsedMs = refinement.elapsed.ElapsedMs();
    snapshot.render.previousHintFromSquare = refinement.outgoing.fromSquare;
    snapshot.render.previousHintToSquare = refinement.outgoing.toSquare;
    snapshot.render.previousHintCaptureSquare = captureSquare(refinement.outgoing.fromSquare, refinement.outgoing.toSquare);
    snapshot.render.previousHintDuckFromSquare = refinement.outgoing.duckFromSquare;
    snapshot.render.previousHintDuckToSquare = refinement.outgoing.duckToSquare;
    snapshot.render.hintRefinementDurationMs = refinement.durationMs;
    snapshot.render.incomingHintFromSquare = refinement.incoming.fromSquare;
    snapshot.render.incomingHintToSquare = refinement.incoming.toSquare;
    snapshot.render.incomingHintCaptureSquare = captureSquare(refinement.incoming.fromSquare, refinement.incoming.toSquare);
    snapshot.render.incomingHintDuckFromSquare = refinement.incoming.duckFromSquare;
    snapshot.render.incomingHintDuckToSquare = refinement.incoming.duckToSquare;
    snapshot.render.hintDuckMoveValid = renderState.hintDuckMoveValid;
    snapshot.render.hintDuckMoveFromSquare =
        renderState.hintDuckMoveFromSquare;
    snapshot.render.hintDuckMoveToSquare =
        renderState.hintDuckMoveToSquare;
    snapshot.render.allPiecesFlashActive = IsAllPiecesFlashActive(renderState, nowMs);
    snapshot.render.allPiecesFlashOn = IsAllPiecesFlashOn(renderState, nowMs);
    snapshot.render.initialKingPlacementPromptActive =
        !inputs.startupPresentationActive && TryGetInitialKingPlacementPrompt(
            renderState,
            snapshot.render.initialKingPlacementPromptMask);
    if (snapshot.render.initialKingPlacementPromptActive) {
        snapshot.render.initialKingPlacementPromptElapsedMs =
            renderState.initialKingPlacementPromptElapsed.ElapsedMs();
    }
    snapshot.render.settleAfterglowActive = TryGetSettleAfterglow(renderState,
                                                                  snapshot.render.settleAfterglowSquare,
                                                                  snapshot.render.settleAfterglowElapsedMs);
    snapshot.render.settleAfterglowAbort = renderState.settleAfterglowAbort;
    snapshot.render.legalDestinationReveal = renderState.legalDestinationReveal;
    snapshot.render.settleAnimationVariant = renderState.settleAnimVariant;
    snapshot.render.whiteLastMoveValid = renderState.lastMoveWhite.valid;
    snapshot.render.whiteLastMoveFromSquare = renderState.lastMoveWhite.from;
    snapshot.render.whiteLastMoveToSquare = renderState.lastMoveWhite.to;
    snapshot.render.blackLastMoveValid = renderState.lastMoveBlack.valid;
    snapshot.render.blackLastMoveFromSquare = renderState.lastMoveBlack.from;
    snapshot.render.blackLastMoveToSquare = renderState.lastMoveBlack.to;
    snapshot.render.lastMoveMostRecentKnown = renderState.lastMoveMostRecentSide != modules::PersistedSide::Unknown;
    snapshot.render.lastMoveMostRecentSide = renderState.lastMoveMostRecentSide;
    snapshot.render.promotionConfirmation = renderState.promotionConfirmation;
    snapshot.render.textOverlayActive = stateMachine.IsTextOverlayActive();
    snapshot.render.menuOverlayActive = inputs.menuOverlayActive;
    snapshot.render.quickSettingsOverlayActive = inputs.quickSettingsOverlayActive;
    snapshot.render.bootFadeActive = inputs.bootFadeActive;
    snapshot.render.startupPresentationActive = inputs.startupPresentationActive;

    snapshot.system.safeModeActive = inputs.safeModeActive;
    snapshot.system.hardwareTestActive = inputs.hardwareTestActive;
    snapshot.system.amptestActive = inputs.amptestActive;
    snapshot.system.otaInProgress = inputs.otaInProgress;
    return snapshot;
}

#ifdef ARDUINO
InteractionModelSnapshot BuildInteractionModelSnapshot(const firmware_internal::FirmwareSystem& sys) {
    const auto& derived = sys.gamePersistence.GetEngineDerivedState();
    InteractionModelSnapshot snapshot = BuildInteractionModelSnapshot({
        .stateMachine = &sys.stateMachine,
        .derived = &derived,
        .authoritativeFen = sys.gamePersistence.GetPresentationFen(),
        .captureFirstCapturerMask = sys.gamePersistence.GetCaptureFirstCapturerMask(),
        .gameClock = &sys.gamePersistence.GetGameClock(),
        .clockWinner = sys.gamePersistence.GetClockTerminalWinner(),
        .clockTerminalStartMs = sys.gamePersistence.GetClockTerminalStartMs(),
        .menuOverlayActive = sys.menuOverlay.IsActive(),
        .quickSettingsOverlayActive = sys.quickSettingsOverlayActive,
        .bootFadeActive = sys.startupLeds.Fade().IsActive(),
        .startupPresentationActive = !firmware_internal::StartupPresentationComplete(sys),
        .safeModeActive = sys.safeModeActive,
        .hardwareTestActive = sys.hardwareTest.IsActive(),
        .amptestActive = sys.amptest.IsActive() || sys.amptestVerify.IsActive() || sys.comptest.IsActive(),
        .otaInProgress = sys.ble.IsOtaInProgress(),
        .variant = sys.gamePersistence.GetGameVariant(),
        .duckSquare = sys.gamePersistence.GetDuckSquare(),
        .initialDuckCandidateSquare =
            sys.gamePersistence.GetInitialDuckCandidateSquare(),
        .initialVariantValidationActive =
            sys.gamePersistence.IsInitialVariantValidationPending(),
        .setupLifecycle = ClassifySetupLifecycle(
            sys.gamePersistence.GetGameVariant(),
            sys.gamePersistence.GetDuckTurnPhase(),
            sys.stateMachine.GetMovedSquaresMask()),
        .duckTurnPhase = sys.gamePersistence.GetDuckTurnPhase(),
        .variantOutcome = sys.gamePersistence.GetVariantOutcome(),
        .variantTerminalStartMs = sys.gamePersistence.GetVariantTerminalStartMs(),
        .kingResignationOwner = sys.gamePersistence.GetKingResignationOwner(),
        .kingResignationSquare = sys.gamePersistence.GetKingResignationSquare(),
        .kingResignationElapsedMs = sys.gamePersistence.GetKingResignationElapsedMs(),
        .kingResignationArmed = sys.gamePersistence.IsKingResignationArmed(),
    });
    return snapshot;
}
#endif

} // namespace runtime
