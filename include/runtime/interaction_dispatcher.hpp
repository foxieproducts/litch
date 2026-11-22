#pragma once

#include "modules/game_persistence_coordinator.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/interaction_domain_types.hpp"
#include "runtime/ordered_app_event_ingress.hpp"

namespace runtime {

// Thin ingress seam between normalized interaction events and the current
// state-machine/coordinator flow.
class InteractionDispatcher {
  public:
    explicit InteractionDispatcher(GameStateMachine& stateMachine)
        : m_stateMachine(stateMachine),
          m_gamePersistence(nullptr),
          m_eventIngress(stateMachine) {}

    InteractionDispatcher(GameStateMachine& stateMachine,
                          AppEventQueue& eventQueue)
        : m_stateMachine(stateMachine),
          m_gamePersistence(nullptr),
          m_eventIngress(stateMachine, &eventQueue) {}

    InteractionDispatcher(GameStateMachine& stateMachine,
                          modules::GamePersistenceCoordinator& gamePersistence)
        : m_stateMachine(stateMachine),
          m_gamePersistence(&gamePersistence),
          m_eventIngress(stateMachine) {}

    InteractionDispatcher(GameStateMachine& stateMachine,
                          modules::GamePersistenceCoordinator& gamePersistence,
                          AppEventQueue& eventQueue)
        : m_stateMachine(stateMachine),
          m_gamePersistence(&gamePersistence),
          m_eventIngress(stateMachine, &eventQueue) {}

    void DispatchSensorChange(const SensorChangeEvent& event) {
        if (m_gamePersistence && m_gamePersistence->Litchcraft().IsActive()) {
            auto& art = m_gamePersistence->Litchcraft();
            const auto mask = UINT64_C(1) << event.logicalSquare;
            art.ObserveOccupancy(event.nowOccupied ? art.Occupancy() | mask
                                                   : art.Occupancy() & ~mask);
            return;
        }
        (void)m_eventIngress.Dispatch(
            AppEvent::MakeSensorDelta(event.tMs,
                                      event.logicalSquare,
                                      event.nowOccupied),
            m_gamePersistence,
            &TryDispatchCoordinatorBoardPolicy);
    }

    uint32_t DispatchBoardObservation(
        const BoardObservationEvent& event,
        uint8_t* const deferredDuckDestination = nullptr) {
        if (m_gamePersistence && m_gamePersistence->Litchcraft().IsActive()) {
            m_gamePersistence->Litchcraft().ObserveOccupancy(event.occupiedMask);
            return event.sequence == 0U ? 1U : event.sequence;
        }
        AppEvent appEvent = AppEvent::MakeBoardObservation(event.tMs,
                                                            event.changedMask,
                                                            event.occupiedMask,
            m_stateMachine.GetDeferredDuckDestinationForObservation(
                event.changedMask, event.occupiedMask));
        if (deferredDuckDestination != nullptr) {
            *deferredDuckDestination = appEvent.deferredDuckDestinationSquare;
        }
        appEvent.seq = event.sequence;
        return m_eventIngress.DispatchInPlace(
            appEvent,
            m_gamePersistence,
            &TryDispatchCoordinatorBoardPolicy);
    }

    uint32_t DispatchBoardResynchronization(
        const BoardObservationEvent& event) {
        if (m_gamePersistence && m_gamePersistence->Litchcraft().IsActive()) {
            m_gamePersistence->Litchcraft().ResynchronizeOccupancy(event.occupiedMask);
            return event.sequence == 0U ? 1U : event.sequence;
        }
        AppEvent appEvent = AppEvent::MakeBoardObservation(
            event.tMs, event.changedMask, event.occupiedMask);
        appEvent.seq = event.sequence;
        return m_eventIngress.DispatchInPlace(
            appEvent,
            m_gamePersistence,
            &TryDispatchRecoveryBoardPolicy);
    }

    void DispatchButtonEvent(const ButtonInputEvent& event) {
        if (event.button == modules::UserButton::Press) return;
        if (m_gamePersistence) m_gamePersistence->StopBenchmark();
        (void)m_eventIngress.Dispatch(AppEvent::MakeButton(
            event.tMs,
            event.button,
            event.event));
    }

    void DispatchTick(const uint32_t tMs) {
        if (m_gamePersistence && m_gamePersistence->Litchcraft().IsActive()) return;
        (void)m_eventIngress.Dispatch(AppEvent::MakeTick(tMs));
    }

    void DispatchHintButtonTap() {
        if (m_gamePersistence != nullptr) {
            m_gamePersistence->RequestHintToggle();
            return;
        }
        (void)m_eventIngress.Dispatch(
            AppEvent::MakeHintToggleRequested(fxe::Millis()));
    }

    void HandleUndoTap(const uint8_t undoCount) {
        if (m_gamePersistence != nullptr) {
            m_gamePersistence->RequestUndo(undoCount);
            return;
        }
        (void)m_eventIngress.Dispatch(
            AppEvent::MakeUndoRequested(fxe::Millis(), undoCount));
    }

    bool ResetEngine() {
        if (m_gamePersistence == nullptr) {
            return false;
        }

        return m_gamePersistence->ResetEngine();
    }

    const GameStateMachine& GetStateMachine() const {
        return m_stateMachine;
    }

    // Shared by production ingress and deterministic trace replay so exact
    // coordinator-owned board policies have identical ordering.
    static bool TryDispatchCoordinatorBoardPolicy(
        void* context,
        GameStateMachine& stateMachine,
        const AppEvent& appEvent) {
        auto* coordinator = static_cast<
            modules::GamePersistenceCoordinator*>(context);
        if (coordinator == nullptr) {
            return false;
        }

        uint64_t changedMask = 0ULL;
        uint64_t occupiedMask = 0ULL;
        if (!TryGetObservationMasks(
                stateMachine, appEvent,
                changedMask, occupiedMask)) {
            return false;
        }
        if (changedMask != 0ULL) coordinator->StopBenchmark();
        if (coordinator->TryHandleEmptyBoardSetupObservation(
                appEvent, changedMask, occupiedMask)) {
            return true;
        }
        if (coordinator->ShouldValidateInitialVariantTransition(
                occupiedMask)) {
            coordinator->MarkInitialVariantValidationPending();
            stateMachine.HandleVariantValidationBoardObservation(
                changedMask, occupiedMask,
                appEvent.tMs, appEvent.seq);
            return true;
        }
        return coordinator->TryHandleTakebackGraceBoardEvent(
            appEvent, changedMask, occupiedMask);
    }

    // Variant detection retains first priority because an exact fresh-board
    // Duck observation is setup classification, never a game takeback.
    static bool TryDispatchInitialVariantValidation(
        void* context,
        GameStateMachine& stateMachine,
        const AppEvent& appEvent) {
        auto* coordinator = static_cast<
            modules::GamePersistenceCoordinator*>(context);
        if (coordinator == nullptr) {
            return false;
        }

        uint64_t changedMask = 0ULL;
        uint64_t occupiedMask = 0ULL;
        if (!TryGetObservationMasks(
                stateMachine, appEvent, changedMask, occupiedMask)) {
            return false;
        }
        if (!coordinator->ShouldValidateInitialVariantTransition(
                occupiedMask)) {
            return false;
        }
        coordinator->MarkInitialVariantValidationPending();
        stateMachine.HandleVariantValidationBoardObservation(
            changedMask, occupiedMask, appEvent.tMs, appEvent.seq);
        return true;
    }

  private:
    static bool TryDispatchRecoveryBoardPolicy(
        void* context,
        GameStateMachine& stateMachine,
        const AppEvent& appEvent) {
        if (TryDispatchCoordinatorBoardPolicy(
                context, stateMachine, appEvent)) {
            return true;
        }
        if (appEvent.type != AppEventType::BoardObservation) {
            return false;
        }
        stateMachine.HandleRecoveryBoardObservation(
            appEvent.changedMask,
            appEvent.occupiedMask,
            appEvent.tMs,
            appEvent.seq);
        return true;
    }

    static bool TryGetObservationMasks(
        const GameStateMachine& stateMachine,
        const AppEvent& appEvent,
        uint64_t& outChangedMask,
        uint64_t& outOccupiedMask) {
        if (appEvent.type == AppEventType::BoardObservation) {
            outChangedMask = appEvent.changedMask;
            outOccupiedMask = appEvent.occupiedMask;
            return true;
        }
        if (appEvent.type != AppEventType::SensorDelta ||
            appEvent.square >= 64U) {
            return false;
        }
        outChangedMask = 1ULL << appEvent.square;
        outOccupiedMask = stateMachine.GetPhysicalOccupancyMask();
        if (appEvent.nowOccupied) {
            outOccupiedMask |= outChangedMask;
        } else {
            outOccupiedMask &= ~outChangedMask;
        }
        return true;
    }

    GameStateMachine& m_stateMachine;
    modules::GamePersistenceCoordinator* m_gamePersistence;
    OrderedAppEventIngress m_eventIngress;
};

} // namespace runtime
