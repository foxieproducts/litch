#pragma once

#include <cstdint>

#include "runtime/app_event_dispatch.hpp"
#include "runtime/event_bus.hpp"

namespace runtime {

// One bounded, heap-free sequencing seam shared by production adapters. The
// queue is drained synchronously today; retaining it as a real queue keeps
// ordering and overflow semantics explicit as more adapters become event based.
class OrderedAppEventIngress final {
  public:
    using Override = bool (*)(void* context,
                              GameStateMachine& stateMachine,
                              const AppEvent& appEvent);

    explicit OrderedAppEventIngress(GameStateMachine& stateMachine,
                                    AppEventQueue* eventQueue = nullptr)
        : m_stateMachine(stateMachine),
          m_eventQueue(eventQueue) {}

    uint32_t Dispatch(AppEvent appEvent,
                      void* context = nullptr,
                      const Override override = nullptr) {
        return DispatchInPlace(appEvent, context, override);
    }

    // Reuse the producer's event as the queue's output. The queue still owns
    // its copy, and releases its only slot before a handler can dispatch again.
    uint32_t DispatchInPlace(AppEvent& appEvent,
                             void* context = nullptr,
                             const Override override = nullptr) {
        if (!Admit(appEvent)) {
            return 0;
        }

        DispatchOne(appEvent, context, override);
        return appEvent.seq;
    }

    uint32_t DispatchPositionAcknowledged(AppEvent& event,
                                         const CaptureLegality& captureLegality) {
        // Admission either refuses an occupied slot or pops this exact event
        // before dispatch. The caller's rows stay outside the queue and live
        // through synchronous handling, including any nested dispatch.
        static_assert(AppEventQueue::Capacity() == 1U);
        if (event.type != AppEventType::PositionAcknowledged || !Admit(event)) {
            return 0U;
        }
        DispatchAppEventToStateMachine(m_stateMachine, event, &captureLegality);
        return event.seq;
    }

  private:
    void DispatchOne(const AppEvent& appEvent,
                     void* context,
                     const Override override) {
        if (override != nullptr && override(context, m_stateMachine, appEvent)) {
            return;
        }
        DispatchAppEventToStateMachine(m_stateMachine, appEvent);
    }

    bool Admit(AppEvent& appEvent) {
        if (m_eventQueue != nullptr) {
            return m_eventQueue->Push(appEvent) && m_eventQueue->TryPop(appEvent);
        }
        if (appEvent.seq == 0U) {
            if (m_directSequence == UINT32_MAX) {
                return false;
            }
            appEvent.seq = ++m_directSequence;
        } else if (appEvent.seq > m_directSequence) {
            m_directSequence = appEvent.seq;
        }
        return true;
    }

    GameStateMachine& m_stateMachine;
    AppEventQueue* m_eventQueue;
    uint32_t m_directSequence = 0;
};

} // namespace runtime
