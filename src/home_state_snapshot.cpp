#include "modules/home_state_snapshot.hpp"
#include "modules/beginner_mode_controller.hpp"
#include "modules/chess_strings.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/promotion_helpers.hpp"
#include "modules/promotion_piece_cue.hpp"
#include "runtime/chess_interaction_duck_projection.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_domain_types.hpp"

namespace modules {
namespace {
uint32_t RemainingWholeSeconds(const uint32_t remainingMs) {
    return remainingMs == 0U ? 0U : ((remainingMs - 1U) / 1000U) + 1U;
}

modules::HomeStateAction ResolveHomeStateAction(
    const runtime::GameStateMachine& stateMachine) {
    if (stateMachine.IsPromotionActive()) {
        const auto& model = stateMachine.GetChessInteractionModel();
        const uint64_t chessOccupancy = runtime::ProjectHumanChessOccupancy(
            model.acknowledged, model.phase, model.board.occupiedMask);
        const uint64_t unexplained = (chessOccupancy ^
            runtime::ProjectPhysicalOccupancyForInteractionProgress(model.acknowledged, model.phase)) &
            ~runtime::GetLegalProgressPhysicalMask(model.phase);
        return unexplained != 0ULL ? modules::HomeStateAction::Correction
                                  : modules::HomeStateAction::Promote;
    }
    if (stateMachine.IsPlacementModeActive() ||
        stateMachine.IsKingQueenSwapPromptActive()) {
        return modules::HomeStateAction::Correction;
    }
    if (stateMachine.HasHumanIllegal() || stateMachine.HasCaptureReturn()) {
        return modules::HomeStateAction::Correction;
    }
    if (stateMachine.IsDuckRelocationActive() &&
        stateMachine.GetChessInteractionModel().phase.requiredAction ==
            runtime::RequiredPhysicalAction::CorrectBoard) {
        return modules::HomeStateAction::Correction;
    }
    if (stateMachine.IsMoveSettlingActive()) {
        return modules::HomeStateAction::Settle;
    }
    if (stateMachine.IsDuckRelocationActive()) {
        return modules::HomeStateAction::Duck;
    }
    if (stateMachine.HasQueuedCpuMove()) {
        return modules::HomeStateAction::Cpu;
    }
    if (stateMachine.IsHumanPieceInHand()) {
        return modules::HomeStateAction::Lift;
    }

    const runtime::ChessInteractionPhase phase =
        stateMachine.GetChessInteractionModel().phase.tag;
    if (phase == runtime::ChessInteractionPhase::Correction ||
        (stateMachine.IsCpuThinkingActive() &&
         stateMachine.GetChessInteractionModel().phase.requiredAction ==
             runtime::RequiredPhysicalAction::CorrectBoard)) {
        return modules::HomeStateAction::Correction;
    }
    if (phase != runtime::ChessInteractionPhase::Idle) {
        return modules::HomeStateAction::Progress;
    }
    return modules::HomeStateAction::Idle;
}

} // namespace

HomeStateNotification BuildHomeStateSnapshot(
    const runtime::GameStateMachine& state,
    const GamePersistenceCoordinator& game,
    const SettingsStore& settings,
    const BeginnerModeController& beginnerMode,
    const PersistenceDegradation degradation,
    const HomeAssistanceRuntimeInputs& assistanceInputs) {
    const auto variant = game.GetGameVariantStateSnapshot();
    const uint64_t physicalOccupancy = state.GetPhysicalOccupancyMask();
    modules::HomeStateNotification snapshot;
    const bool clockTerminal = game.HasClockTerminalOutcome() || game.HasResignationOutcome();
    if (variant.variant == GameVariant::Classic && !clockTerminal && !variant.outcome.IsTerminal()) {
        const auto& chess = game.GetEngineDerivedState();
        if (chess.inCheckmate) snapshot.classicResult = HomeClassicResult::Checkmate;
        else if (chess.inStalemate) snapshot.classicResult = HomeClassicResult::Stalemate;
        else if (chess.inDraw) snapshot.classicResult = HomeClassicResult::Draw;
    }
    const bool terminal = clockTerminal || snapshot.classicResult != HomeClassicResult::Ongoing;
    const bool resignArmed = game.IsKingResignationArmed();
    snapshot.action = terminal ? modules::HomeStateAction::Idle
                                    : resignArmed ? modules::HomeStateAction::Lift
                                                  : ResolveHomeStateAction(state);
    snapshot.fen = game.GetHomePresentationFen();
    snapshot.occupiedMask = physicalOccupancy;
    snapshot.variant = variant.variant;
    snapshot.duckSquare = modules::HomeStateDuckSquareForPresentation(
        variant,
        game.GetInitialDuckCandidateSquare());
    snapshot.outcome = variant.outcome;
    snapshot.clockWinner = game.GetClockTerminalWinner();
    const runtime::GameClock& clock =
        game.GetGameClock();
    snapshot.clockStateValid = true;
    snapshot.clockEnabled = clock.IsEnabled();
    snapshot.clockRunning = clock.IsRunning();
    snapshot.clockPaused = clock.IsPaused();
    snapshot.clockActiveSide =
        clock.GetActiveSide() == runtime::ClockSide::Black ? 'b' : 'w';
    snapshot.clockTimerSetting =
        static_cast<uint8_t>(game.GetClockTimerSetting());
    const modules::ControlCoordinatorResult clockSetup =
        game.CheckControlStartOnlyGate("TIMER");
    snapshot.clockSetupState =
        clockSetup == modules::ControlCoordinatorResult::Applied
            ? 'y'
            : clockSetup == modules::ControlCoordinatorResult::Busy ? 'b'
                                                                    : 'n';
    snapshot.clockWhiteRemainingSeconds = RemainingWholeSeconds(
        clock.GetRemainingMs(runtime::ClockSide::White));
    snapshot.clockBlackRemainingSeconds = RemainingWholeSeconds(
        clock.GetRemainingMs(runtime::ClockSide::Black));
    snapshot.boardRotated = settings.ReadValue("BOARD_ROTATE", false);
    // Logical P1 remains White. GAME_FIRST_PLAYER changes the physical end,
    // while the accepted occupancy above is already mapped to logical squares.
    snapshot.whiteOwnerIsP2 = false;
    snapshot.placementMode = !terminal && state.IsPlacementModeActive();
    snapshot.freshSetup =
        !terminal &&
        runtime::ClassifySetupLifecycle(
            variant.variant,
            game.GetDuckTurnPhase(),
            state.GetMovedSquaresMask()) ==
        runtime::SetupLifecycle::FreshSetup;
    snapshot.kingQueenSwapPromptActive =
        !terminal && state.IsKingQueenSwapPromptActive();
    snapshot.vexMoveFirst =
        game.IsInitialDuckObstructionRelocationPromptActive();
    snapshot.undoPending = game.IsUndoActionPending();
    snapshot.coachingStateValid = true;
    snapshot.coachingMask =
        static_cast<uint8_t>(
            beginnerMode.IsEnabled(UiOwner::P1) ? 1U : 0U) |
        static_cast<uint8_t>(
            beginnerMode.IsEnabled(UiOwner::P2) ? 2U : 0U);
    snapshot.assistance =
        game.BuildHomeAssistanceProjection(assistanceInputs);
    if (snapshot.action == HomeStateAction::Correction &&
        variant.variant == GameVariant::Duck && !state.HasCaptureReturn() &&
        !state.HasCpuIllegal() && modules::IsValidDuckSquare(variant.duckSquare)) {
        const auto& model = state.GetChessInteractionModel();
        snapshot.duckLandingRemovalMask = runtime::GetDuckLandingRemovalMask(
            1ULL << variant.duckSquare, model.phase, model.acknowledged.occupiedMask,
            physicalOccupancy, state.GetHumanIllegalMask());
    }
    snapshot.persistenceDegradation = degradation;
    if (snapshot.persistenceDegradation ==
            modules::PersistenceDegradation::None &&
        settings.HasConfirmedStorageFailure()) {
        snapshot.persistenceDegradation =
            modules::PersistenceDegradation::StorageIo;
    }
    if (!terminal && !resignArmed && !snapshot.outcome.IsTerminal()) {
        snapshot.cpuThinking = game.GetCpuThinkingPresentation();
    }
    snapshot.physicalMoveValid = !terminal && !resignArmed && state.TryGetQueuedCpuMove(
        snapshot.physicalFromSquare,
        snapshot.physicalToSquare,
        snapshot.physicalCaptureSquareValid,
        snapshot.physicalCaptureSquare);
    const auto& phase = state.GetChessInteractionModel().phase;
    const bool humanRemovalAction = snapshot.action == HomeStateAction::Lift ||
                                    snapshot.action == HomeStateAction::Settle;
    const bool humanRemovalSuppressed = terminal || resignArmed || snapshot.outcome.IsTerminal() ||
        assistanceInputs.menuOverlayActive || assistanceInputs.quickSettingsOverlayActive ||
        assistanceInputs.gameResetHoldActive || state.IsTextOverlayActive() ||
        state.IsPlacementModeActive() || state.IsKingQueenSwapPromptActive() ||
        state.IsUndoPromptActive() || state.IsPromotionActive() ||
        state.HasHumanIllegal() || state.HasCpuIllegal();
    const auto& plan = phase.plan;
    if (!snapshot.physicalMoveValid && humanRemovalAction && !humanRemovalSuppressed &&
        phase.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
        phase.requiredAction == runtime::RequiredPhysicalAction::RemoveCapture &&
        plan.source == runtime::PhysicalTransitionSource::Human &&
        (plan.kind == runtime::PhysicalTransitionKind::Capture ||
         plan.kind == runtime::PhysicalTransitionKind::EnPassant) &&
        plan.moveFrom < 64U && plan.moveTo < 64U && plan.removalRequired &&
        !plan.progress.removalComplete && plan.removalSquare < 64U &&
        (physicalOccupancy & (UINT64_C(1) << plan.removalSquare)) != 0ULL) {
        snapshot.physicalMoveValid = true;
        snapshot.physicalFromSquare = plan.moveFrom;
        snapshot.physicalToSquare = plan.moveTo;
        snapshot.physicalCaptureSquareValid = true;
        snapshot.physicalCaptureSquare = plan.removalSquare;
    }
    if (!snapshot.physicalMoveValid && !terminal && !resignArmed &&
        !snapshot.outcome.IsTerminal() && !snapshot.placementMode && !snapshot.undoPending &&
        state.IsPromotionActive() && phase.tag == runtime::ChessInteractionPhase::AwaitingAction &&
        plan.kind == runtime::PhysicalTransitionKind::Promotion &&
        plan.source == runtime::PhysicalTransitionSource::Human &&
        plan.promotionRequired && plan.moveFrom < 64U && plan.moveTo < 64U &&
        (snapshot.action == HomeStateAction::Promote || snapshot.action == HomeStateAction::Correction)) {
        snapshot.physicalMoveValid = true;
        snapshot.physicalFromSquare = plan.moveFrom;
        snapshot.physicalToSquare = plan.moveTo;
        snapshot.physicalCaptureSquareValid = false;
        if (snapshot.action == HomeStateAction::Promote) {
            snapshot.promotionSelection = static_cast<uint8_t>(state.GetPromotionSelection()) + 1U;
        }
    }
    if (state.IsDuckRelocationActive() &&
        (snapshot.action == HomeStateAction::Duck ||
         snapshot.action == HomeStateAction::Correction) &&
        snapshot.outcome.reason == VariantOutcomeReason::None &&
        snapshot.clockWinner == GameWinner::None) {
        const bool selectable =
            phase.selectedSquare == runtime::CHESS_INTERACTION_INVALID_SQUARE;
        snapshot.duckRelocationOwner =
            runtime::IsCpuDirectedTransition(phase.plan) ? 'c'
            : phase.plan.source == runtime::PhysicalTransitionSource::Human &&
                    selectable ? 'h' : 'u';
        snapshot.duckRelocationFromSquare = phase.plan.moveFrom;
        // selectedSquare is the plan's requirement; moveTo can instead be a
        // human's tentative placement. Fixed Undo/redo remains neutral.
        snapshot.duckRelocationToSquare = phase.selectedSquare;
    }
    snapshot.whiteLastMoveValid = state.TryGetLastMoveForSide(
        modules::PersistedSide::White,
        snapshot.whiteLastMoveFromSquare,
        snapshot.whiteLastMoveToSquare);
    snapshot.blackLastMoveValid = state.TryGetLastMoveForSide(
        modules::PersistedSide::Black,
        snapshot.blackLastMoveFromSquare,
        snapshot.blackLastMoveToSquare);
    modules::PersistedSide mostRecentMoveSide = modules::PersistedSide::Unknown;
    if (state.TryGetLastMoveMostRecentSide(mostRecentMoveSide)) {
        snapshot.lastMoveMostRecentSide =
            mostRecentMoveSide == modules::PersistedSide::White ? 'w' : 'b';
        if (snapshot.promotionSelection != 0U) return snapshot;
        const auto& confirmation = state.GetRenderState().promotionConfirmation;
        const bool white = mostRecentMoveSide == PersistedSide::White;
        const uint8_t from = white ? snapshot.whiteLastMoveFromSquare
                                   : snapshot.blackLastMoveFromSquare;
        const uint8_t to = white ? snapshot.whiteLastMoveToSquare
                                 : snapshot.blackLastMoveToSquare;
        const bool staged = variant.pendingChessMove &&
            variant.pendingMoveFrom == from && variant.pendingMoveTo == to &&
            snapshot.duckRelocationOwner == 'h';
        const bool current = confirmation.square == to && confirmation.side == mostRecentMoveSide &&
            fxe::Millis() - confirmation.startedAtMs < runtime::PromotionConfirmation::NOTICE_MS;
        const char suffix = PromotionPieceToUciChar(confirmation.piece);
        const char promoted = PromotionFenPieceChar(
            white ? SideToMove::White : SideToMove::Black, suffix);
        char fenPiece = '\0';
        const bool matches = staged ? variant.pendingPromotionSuffix == suffix
            : current && TryGetFenPieceAtSquare(snapshot.fen, to, fenPiece) && fenPiece == promoted;
        if (current && matches && !snapshot.undoPending) {
            snapshot.confirmedPromotion = static_cast<uint8_t>(confirmation.piece) + 1U;
            snapshot.promotionRevealActive = true;
            snapshot.promotionAutomatic = confirmation.automatic;
        } else if (staged) {
            PromotionPiece piece;
            if (promotion_piece_cue::TryPieceFromSuffix(variant.pendingPromotionSuffix, piece)) {
                snapshot.confirmedPromotion = static_cast<uint8_t>(piece) + 1U;
            }
        }
    }

    return snapshot;
}
bool TryPrepareHomeStatePublication(
    const runtime::GameStateMachine& state,
    const GamePersistenceCoordinator& game,
    const SettingsStore& settings,
    const BeginnerModeController& beginnerMode,
    const PersistenceDegradation degradation,
    const HomeAssistanceRuntimeInputs& assistanceInputs,
    HomeStateNotificationPublisher& publisher,
    const bool force,
    HomeStateNotificationLine& outLine) {
    if (!ShouldPublishHomeStateNotification(
            game.GetGameVariantStateSnapshot(),
            state.GetPhysicalOccupancyMask(),
            game.IsInitialDuckRetirementObservation())) {
        return false;
    }
    return publisher.TryPrepare(
        BuildHomeStateSnapshot(state, game, settings, beginnerMode,
                               degradation, assistanceInputs),
        force, outLine);
}

} // namespace modules
