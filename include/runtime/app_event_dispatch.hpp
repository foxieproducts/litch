#pragma once

#include "runtime/app_event.hpp"
#include "runtime/game_state_machine.hpp"

namespace runtime {

// Dispatches an AppEvent into the current core state machine.
// This is intentionally narrow today; it grows as more subsystems become event-driven.
inline void DispatchAppEventToStateMachine(GameStateMachine& sm, const AppEvent& evt,
                                           const CaptureLegality* captureLegality = nullptr) {
    switch (evt.type) {
        case AppEventType::Tick:
            sm.Tick();
            return;
        case AppEventType::SensorDelta:
            sm.HandleSensorChange(evt.square,
                                  evt.nowOccupied,
                                  evt.tMs,
                                  evt.seq);
            return;
        case AppEventType::BoardObservation:
            sm.HandleBoardObservation(evt.changedMask,
                                      evt.occupiedMask,
                                      evt.tMs,
                                      evt.seq,
                                      evt.deferredDuckDestinationSquare);
            return;
        case AppEventType::Button:
            sm.HandleButtonEvent(evt.button, evt.event);
            return;
        case AppEventType::PositionAcknowledged:
            sm.HandleInteractionPositionAcknowledged(
                evt.occupiedMask,
                evt.kingMask,
                evt.rookMask,
                evt.moverMask,
                evt.opponentMask,
                evt.pawnMask,
                evt.enPassantTargetSquare,
                evt.enPassantCaptureSquare,
                evt.sideToMoveValid,
                evt.sideToMoveBlack,
                evt.legalCaptureTargetsValid,
                evt.legalCaptureTargetsMask,
                evt.legalCastlingTargetsValid,
                evt.legalCastlingKingTargetsMask,
                captureLegality);
            return;
        case AppEventType::ActionResult:
            sm.HandleInteractionActionResult(evt.token, evt.succeeded);
            return;
        case AppEventType::SearchResult:
            sm.HandleInteractionSearchResult(evt.succeeded);
            return;
        case AppEventType::PersistenceResult:
        case AppEventType::RestoreResult:
            sm.HandleInteractionAdapterResult(evt.succeeded);
            return;
        case AppEventType::UndoRequested:
            // Engine history lives in GamePersistenceCoordinator. Its ordered
            // control handler consumes this semantic event when present.
            return;
        case AppEventType::HintToggleRequested:
            sm.HandleHintButtonTap();
            return;
        case AppEventType::ResetRequested:
            return;
    }
}

} // namespace runtime
