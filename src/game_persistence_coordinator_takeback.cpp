#include "modules/game_persistence_coordinator.hpp"

#include "modules/takeback_grace_setting.hpp"
#include "runtime/app_event_dispatch.hpp"

namespace modules {
namespace {

uint64_t SquareMask(const uint8_t square) {
    return square < 64U ? (1ULL << square) : 0ULL;
}

bool IsBoardEvent(const runtime::AppEventType type) {
    return type == runtime::AppEventType::BoardObservation ||
           type == runtime::AppEventType::SensorDelta;
}

bool ReadBoardRotate(const SettingsStore& settings) {
    return settings.ReadValue(
               "BOARD_ROTATE", static_cast<uint32_t>(0U)) != 0U;
}

bool ReadFirstPlayerIsP2(const SettingsStore& settings) {
    return settings.ReadValue(
               "GAME_FIRST_PLAYER", static_cast<uint32_t>(1U)) == 2U;
}

} // namespace

uint32_t GamePersistenceCoordinator::ReadTakebackGraceSeconds() const {
    return takeback_grace_setting::ReadSeconds(m_settings);
}

void GamePersistenceCoordinator::ClearTakebackGrace() {
    if (m_takebackGrace.hintOwned) {
        m_stateMachine.ClearHintMove();
    }
    m_takebackGrace = TakebackGraceState{};
}

void GamePersistenceCoordinator::
    RefreshPreparedTakebackGraceClockSnapshot() {
    if (m_takebackGrace.prepared) {
        m_takebackGrace.beforeClock = m_gameClock.CaptureState();
    }
}

void GamePersistenceCoordinator::PrepareTakebackGrace(
    const runtime::PhysicalTransitionPlan& move) {
    ClearTakebackGrace();
    if (move.source != runtime::PhysicalTransitionSource::Human ||
        move.moveFrom >= 64U || move.moveTo >= 64U) {
        return;
    }

    TakebackGraceState prepared;
    prepared.prepared = true;
    prepared.from = move.moveFrom;
    prepared.to = move.moveTo;
    if (move.kind == runtime::PhysicalTransitionKind::Castling &&
        move.relocationCount == 2U &&
        move.relocations[1].from < 64U &&
        move.relocations[1].to < 64U) {
        prepared.secondaryFrom = move.relocations[1].from;
        prepared.secondaryTo = move.relocations[1].to;
    }
    if (move.removalRequired && move.removalSquare < 64U) {
        prepared.restorationSquare = move.removalSquare;
    }
    prepared.timerSetting = ReadTimerSetting();
    prepared.boardRotate = ReadBoardRotate(m_settings);
    prepared.firstPlayerIsP2 = ReadFirstPlayerIsP2(m_settings);
    prepared.historyFieldCountBefore = m_undoHistoryFieldCount;
    prepared.historyRevisionBefore = m_undoHistoryRevision;
    prepared.beforeClock = m_gameClock.CaptureState();
    prepared.beforeRngSeed = m_vsCpuRngSeed;
    prepared.beforeMovedSquares =
        m_stateMachine.GetMovedSquaresMaskBeforeLastAcknowledgedHumanMove();
    m_takebackGrace = prepared;
}

void GamePersistenceCoordinator::ArmPreparedTakebackGrace(
    const bool requiresDuckFirst,
    const uint8_t duckFrom,
    const uint8_t duckTo) {
    if (!m_takebackGrace.prepared) {
        return;
    }

    const uint16_t expectedHistoryCount =
        m_takebackGrace.historyFieldCountBefore ==
                UNDO_HISTORY_FIELD_CAPACITY
            ? UNDO_HISTORY_FIELD_CAPACITY
            : static_cast<uint16_t>(
                  m_takebackGrace.historyFieldCountBefore +
                  UNDO_HISTORY_FIELDS_PER_MOVE);
    const uint32_t expectedHistoryRevision =
        m_takebackGrace.historyRevisionBefore + 1U;
    const uint32_t durationMs = takeback_grace_setting::ToDurationMs(
        ReadTakebackGraceSeconds());
    if (durationMs == 0U ||
        m_undoHistoryFieldCount != expectedHistoryCount ||
        m_undoHistoryRevision != expectedHistoryRevision ||
        (requiresDuckFirst &&
         (duckFrom >= 64U || duckTo >= 64U ||
          duckFrom == duckTo))) {
        ClearTakebackGrace();
        return;
    }

    m_takebackGrace.prepared = false;
    m_takebackGrace.armed = true;
    m_takebackGrace.requiresDuckFirst = requiresDuckFirst;
    m_takebackGrace.duckFrom = duckFrom;
    m_takebackGrace.duckTo = duckTo;
    m_takebackGrace.armedAtMs = fxe::Millis();
    m_takebackGrace.durationMs = durationMs;
    m_takebackGrace.historyFieldCountAfter =
        m_undoHistoryFieldCount;
    m_takebackGrace.historyRevisionAfter =
        m_undoHistoryRevision;
    m_takebackGrace.afterOccupiedMask =
        m_stateMachine.GetExpectedOccupancyMask();
    m_takebackGrace.afterClock = m_gameClock.CaptureState();
    m_takebackGrace.afterRngSeed = m_vsCpuRngSeed;
    m_takebackGrace.afterMovedSquares =
        m_stateMachine.GetMovedSquaresMask();
}

void GamePersistenceCoordinator::RestoreTakebackGraceSnapshots(
    const bool forward) {
    if (!m_takebackGrace.undoInProgress) {
        return;
    }
    m_gameClock.RestoreState(
        forward ? m_takebackGrace.afterClock
                : m_takebackGrace.beforeClock);
    m_vsCpuRngSeed = forward ? m_takebackGrace.afterRngSeed
                             : m_takebackGrace.beforeRngSeed;
    m_stateMachine.SetMovedSquaresMask(
        forward ? m_takebackGrace.afterMovedSquares
                : m_takebackGrace.beforeMovedSquares);
}

bool GamePersistenceCoordinator::BeginTakebackGraceUndo(
    const runtime::AppEvent& event,
    const bool observationAlreadyApplied) {
    if (!m_takebackGrace.armed) {
        return false;
    }

    const bool duckUndo = m_takebackGrace.requiresDuckFirst;
    if (m_takebackGrace.hintOwned) {
        m_stateMachine.ClearHintMove();
        m_takebackGrace.hintOwned = false;
    }
    RequestCancelAllThinking();
    (void)m_stateMachine.CancelCompletedSettleForUndo();
    if (m_takebackGrace.ambiguousCapture) {
        m_stateMachine.CancelMovePreviewForUndo();
    }

    const bool began = duckUndo
                           ? BeginDuckUndoIfNeeded(
                                 1U, UndoCommandOrigin::GraceTakeback)
                           : BeginNextUndo(UndoCommandOrigin::GraceTakeback);
    if (!began) {
        ClearTakebackGrace();
        return false;
    }

    m_takebackGrace.armed = false;
    m_takebackGrace.ambiguousCapture = false;
    m_takebackGrace.duckChessFirstGuidance = false;
    m_takebackGrace.duckInverseSelected = false;
    m_takebackGrace.undoInProgress = true;
    // Chess-first recovery already applied the original observation through
    // Correction. Do not replay its chess placement into the new Duck plan.
    if (!observationAlreadyApplied) {
        runtime::DispatchAppEventToStateMachine(m_stateMachine, event);
    }
    return true;
}

bool GamePersistenceCoordinator::ValidateTakebackGraceBoardEvent(
    const runtime::AppEvent& event) {
    if (!IsBoardEvent(event.type) ||
        m_takebackGrace.prepared ||
        m_takebackGrace.undoInProgress ||
        !m_takebackGrace.armed) {
        return false;
    }
    if (m_undoHistoryFieldCount !=
            m_takebackGrace.historyFieldCountAfter ||
        m_undoHistoryRevision !=
            m_takebackGrace.historyRevisionAfter ||
        ReadTimerSetting() != m_takebackGrace.timerSetting ||
        ReadBoardRotate(m_settings) !=
            m_takebackGrace.boardRotate ||
        ReadFirstPlayerIsP2(m_settings) !=
            m_takebackGrace.firstPlayerIsP2) {
        ClearTakebackGrace();
        return false;
    }

    const uint32_t elapsed =
        event.tMs - m_takebackGrace.armedAtMs;
    if (!m_takebackGrace.ambiguousCapture &&
        !m_takebackGrace.duckChessFirstGuidance &&
        elapsed >= m_takebackGrace.durationMs) {
        ClearTakebackGrace();
        return false;
    }

    return true;
}

void GamePersistenceCoordinator::ShowTakebackGraceDuckGuidance(
    const runtime::AppEvent& event,
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    RequestCancelAllThinking();
    m_stateMachine.ClearHintMove();
    m_takebackGrace.hintOwned = false;
    m_stateMachine.CancelMovePreviewForUndo();
    m_stateMachine.HandleGuidedBoardObservation(
        changedMask, occupiedMask, event.tMs, event.seq);
    m_stateMachine.ReconcileUndoOccupancy();
}

bool GamePersistenceCoordinator::ContinueTakebackGraceDuckGuidance(
    const runtime::AppEvent& event,
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    const uint64_t inverseOrigins = SquareMask(m_takebackGrace.from) |
                                    SquareMask(m_takebackGrace.secondaryFrom);
    if ((changedMask & occupiedMask & inverseOrigins) != 0ULL) {
        m_takebackGrace.duckInverseSelected = true;
    }
    m_stateMachine.HandleBoardObservation(
        changedMask, occupiedMask, event.tMs, event.seq);
    if (occupiedMask == m_takebackGrace.afterOccupiedMask &&
        m_stateMachine.GetChessInteractionModel().phase.tag ==
            runtime::ChessInteractionPhase::Idle) {
        m_takebackGrace.duckChessFirstGuidance = false;
        if (m_takebackGrace.duckInverseSelected) {
            (void)BeginTakebackGraceUndo(event, true);
        }
    }
    return true;
}

bool GamePersistenceCoordinator::HandleTakebackGraceDuckFirst(
    const runtime::AppEvent& event,
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    const uint64_t removed = changedMask & ~occupiedMask;
    const uint64_t placed = changedMask & occupiedMask;
    const uint64_t primaryDestination =
        SquareMask(m_takebackGrace.to);
    const uint64_t secondaryDestination =
        SquareMask(m_takebackGrace.secondaryTo);
    const uint64_t inverseOrigins =
        SquareMask(m_takebackGrace.from) |
        SquareMask(m_takebackGrace.secondaryFrom);
    const uint64_t duckDestination =
        SquareMask(m_takebackGrace.duckTo);

    if ((removed & duckDestination) != 0ULL &&
        (changedMask & ~(duckDestination |
                         SquareMask(m_takebackGrace.duckFrom))) == 0ULL) {
        return BeginTakebackGraceUndo(event);
    }
    if (TryStageTakebackGraceCaptureAmbiguity(
            m_stateMachine.GetChessInteractionModel(), removed, placed)) {
        return false;
    }
    if (!IsTakebackGraceControlledChange(
            changedMask, primaryDestination | secondaryDestination)) {
        ClearTakebackGrace();
        return false;
    }
    if ((removed & (primaryDestination |
                    secondaryDestination)) != 0ULL ||
        (placed & inverseOrigins) != 0ULL) {
        m_takebackGrace.duckChessFirstGuidance = true;
        m_takebackGrace.duckInverseSelected = (placed & inverseOrigins) != 0ULL;
        ShowTakebackGraceDuckGuidance(
            event, changedMask, occupiedMask);
        return true;
    }
    ClearTakebackGrace();
    return false;
}

bool GamePersistenceCoordinator::HandleTakebackGraceCaptureAmbiguity(
    const runtime::AppEvent& event,
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    const runtime::ChessInteractionModel& model =
        m_stateMachine.GetChessInteractionModel();
    const runtime::TaggedInteractionState& phase = model.phase;
    if (phase.tag != runtime::ChessInteractionPhase::CaptureStaged ||
        phase.selectedSquare != m_takebackGrace.to) {
        ClearTakebackGrace();
        return false;
    }
    const uint64_t placed = changedMask & occupiedMask;
    if ((placed & SquareMask(m_takebackGrace.from)) != 0ULL) {
        if (m_takebackGrace.requiresDuckFirst) {
            m_takebackGrace.duckChessFirstGuidance = true;
            m_takebackGrace.duckInverseSelected = true;
            ShowTakebackGraceDuckGuidance(
                event, changedMask, occupiedMask);
            return true;
        }
        return BeginTakebackGraceUndo(event);
    }
    ClearTakebackGrace();
    return false;
}

bool GamePersistenceCoordinator::TryStageTakebackGraceCaptureAmbiguity(
    const runtime::ChessInteractionModel& model,
    const uint64_t removedMask,
    const uint64_t placedMask) {
    const uint64_t destination = SquareMask(m_takebackGrace.to);
    const bool onlyDestinationLifted =
        removedMask == destination && placedMask == 0ULL;
    const bool regularCapture =
        model.acknowledged.legalCaptureTargetsValid &&
        (model.acknowledged.legalCaptureTargetsMask &
         destination) != 0ULL;
    const bool enPassantCapture =
        model.acknowledged.enPassantTargetSquare < 64U &&
        model.acknowledged.enPassantCaptureSquare ==
            m_takebackGrace.to;
    // A finished game has no next capture competing with the takeback.
    if (!onlyDestinationLifted || IsGameOverVariantAware() ||
        (!regularCapture && !enPassantCapture)) {
        return false;
    }
    m_takebackGrace.ambiguousCapture = true;
    m_stateMachine.SetHintMoveSquares(
        m_takebackGrace.to, m_takebackGrace.from);
    m_takebackGrace.hintOwned = true;
    return true;
}

bool GamePersistenceCoordinator::IsTakebackGraceControlledChange(
    const uint64_t changedMask,
    const uint64_t destinationMask) const {
    const uint64_t inverseOrigins =
        SquareMask(m_takebackGrace.from) |
        SquareMask(m_takebackGrace.secondaryFrom);
    const uint64_t controlled =
        destinationMask | inverseOrigins |
        SquareMask(m_takebackGrace.restorationSquare);
    return (changedMask & ~controlled) == 0ULL;
}

bool GamePersistenceCoordinator::HandleTakebackGraceIdleInverse(
    const runtime::AppEvent& event,
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    const runtime::ChessInteractionModel& model =
        m_stateMachine.GetChessInteractionModel();
    if (model.phase.tag != runtime::ChessInteractionPhase::Idle ||
        model.board.occupiedMask !=
            m_takebackGrace.afterOccupiedMask) {
        ClearTakebackGrace();
        return false;
    }

    const uint64_t removed = changedMask & ~occupiedMask;
    const uint64_t placed = changedMask & occupiedMask;
    const uint64_t primaryDestination =
        SquareMask(m_takebackGrace.to);
    const uint64_t secondaryDestination =
        SquareMask(m_takebackGrace.secondaryTo);
    const uint64_t destinations =
        primaryDestination | secondaryDestination;
    if ((removed & destinations) == 0ULL) {
        ClearTakebackGrace();
        return false;
    }

    if (TryStageTakebackGraceCaptureAmbiguity(
            model, removed, placed)) {
        return false;
    }
    if (!IsTakebackGraceControlledChange(
            changedMask, destinations)) {
        ClearTakebackGrace();
        return false;
    }
    return BeginTakebackGraceUndo(event);
}

bool GamePersistenceCoordinator::TryHandleTakebackGraceBoardEvent(
    const runtime::AppEvent& event,
    const uint64_t changedMask,
    const uint64_t occupiedMask) {
    if (!ValidateTakebackGraceBoardEvent(event)) {
        return false;
    }
    if (m_takebackGrace.duckChessFirstGuidance) {
        return ContinueTakebackGraceDuckGuidance(
            event, changedMask, occupiedMask);
    }
    if (m_takebackGrace.ambiguousCapture) {
        return HandleTakebackGraceCaptureAmbiguity(
            event, changedMask, occupiedMask);
    }
    if (m_takebackGrace.requiresDuckFirst) {
        return HandleTakebackGraceDuckFirst(
            event, changedMask, occupiedMask);
    }
    return HandleTakebackGraceIdleInverse(
        event, changedMask, occupiedMask);
}

} // namespace modules
