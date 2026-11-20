#include <cstdio>

#include "runtime/app_event_dispatch.hpp"
#include "runtime/clock.hpp"
#include "runtime/event_bus.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

bool CountIngressDispatch(void* context,
                          runtime::GameStateMachine&,
                          const runtime::AppEvent&) {
    auto* const count = static_cast<uint32_t*>(context);
    ++(*count);
    return true;
}

} // namespace

int main() {
    std::puts("[EVENT_BUS] start");

    runtime::EventBus<4> bus;
    bus.Reset();

    Check(bus.Count() == 0, "starts empty");

    Check(bus.Push(runtime::AppEvent::MakeTick(10)), "push tick");
    Check(bus.Push(runtime::AppEvent::MakeSensorDelta(10, 12, false)), "push sensor");
    Check(bus.Push(runtime::AppEvent::MakeTick(11)), "push tick2");
    Check(bus.Count() == 3, "count after pushes");

    runtime::AppEvent e;
    Check(bus.TryPop(e), "pop1");
    Check(e.seq == 1, "seq increments 1");
    Check(e.tMs == 10, "tMs preserved 1");
    Check(e.type == runtime::AppEventType::Tick, "type tick 1");

    Check(bus.TryPop(e), "pop2");
    Check(e.seq == 2, "seq increments 2");
    Check(e.type == runtime::AppEventType::SensorDelta, "type sensor 2");
    Check(e.square == 12, "sensor square");

    Check(bus.TryPop(e), "pop3");
    Check(e.seq == 3, "seq increments 3");
    Check(e.tMs == 11, "tMs preserved 3");

    Check(!bus.TryPop(e), "empty after pops");

    Check(bus.AdvanceSequencePast(7U),
          "external sequence can rebase queue");
    Check(bus.Push(runtime::AppEvent::MakeTick(12U)),
          "push after external sequence");
    Check(bus.TryPop(e) && e.seq == 8U,
          "queue continues strictly after external sequence");
    Check(!bus.AdvanceSequencePast(UINT32_MAX),
          "exhausted external sequence is rejected");

    runtime::EventBus<1> exhaustedBus;
    Check(exhaustedBus.AdvanceSequencePast(UINT32_MAX - 1U),
          "queue can reserve the final nonzero sequence");
    Check(exhaustedBus.Push(runtime::AppEvent::MakeTick(13U)),
          "queue can emit UINT32_MAX exactly once");
    Check(exhaustedBus.TryPop(e) && e.seq == UINT32_MAX,
          "final queue event retains UINT32_MAX");
    Check(!exhaustedBus.Push(runtime::AppEvent::MakeTick(14U)),
          "queue refuses to wrap its failure sentinel to zero");
    Check(!exhaustedBus.AdvanceSequencePast(1U),
          "an exhausted queue cannot silently revive without reset");

    // Overflow should fail (CAPACITY=4).
    bus.Reset();
    Check(bus.Push(runtime::AppEvent::MakeTick(0)), "push a");
    Check(bus.Push(runtime::AppEvent::MakeTick(0)), "push b");
    Check(bus.Push(runtime::AppEvent::MakeTick(0)), "push c");
    Check(bus.Push(runtime::AppEvent::MakeTick(0)), "push d");
    Check(!bus.Push(runtime::AppEvent::MakeTick(0)), "overflow fails");

    // Integration: dispatch via AppEvent into the state machine.
    runtime::ManualClock clock(1000);
    runtime::GameStateMachineConfig cfg;
    cfg.settleDurationMs = 50;
    cfg.cpuIdleDelayMs = 999999;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "sm starts idle");

    runtime::EventBus<8> bus2;
    bus2.Reset();
    (void)bus2.Push(runtime::AppEvent::MakeSensorDelta(clock.NowMs(), 1, false));
    (void)bus2.Push(runtime::AppEvent::MakeTick(clock.NowMs()));

    while (bus2.TryPop(e)) {
        runtime::DispatchAppEventToStateMachine(sm, e);
    }

    Check(sm.IsHumanPieceInHand(), "sensor delta drives state");

    runtime::GameStateMachine queuedSm(cfg);
    queuedSm.Reset();
    runtime::AppEventQueue productionQueue;
    productionQueue.Reset();
    Check(runtime::AppEventQueue::Capacity() == 1U,
          "production ingress needs one synchronously drained slot");
    runtime::InteractionDispatcher dispatcher(queuedSm, productionQueue);

    const uint32_t firstObservationSequence = dispatcher.DispatchBoardObservation({
        .tMs = 1010,
        .changedMask = (1ULL << 60),
        .occupiedMask = 0,
    });
    dispatcher.DispatchButtonEvent({
        .tMs = 1010,
        .button = static_cast<modules::UserButton>(4U),
        .event = fxe::Button::Event::Press,
    });
    dispatcher.DispatchButtonEvent({
        .tMs = 1011,
        .button = modules::UserButton::Up,
        .event = fxe::Button::Event::Release,
    });
    const uint32_t secondObservationSequence = dispatcher.DispatchBoardObservation({
        .tMs = 1012,
        .changedMask = (1ULL << 63),
        .occupiedMask = 0,
    });

    Check(firstObservationSequence == 1,
          "production queue assigns first global sequence");
    Check(secondObservationSequence == 3,
          "button event participates in global sequence ordering");
    Check(queuedSm.GetLastBoardObservationSequence() == 3,
          "state machine receives queue-assigned observation sequence");
    Check(productionQueue.Count() == 0,
          "synchronous dispatcher drains the bounded queue");

    dispatcher.DispatchHintButtonTap();
    dispatcher.HandleUndoTap(1U);
    const uint32_t afterControlsSequence = dispatcher.DispatchBoardObservation({
        .tMs = 1013,
        .changedMask = 0,
        .occupiedMask = 0,
    });
    Check(afterControlsSequence == 6,
          "semantic hint and undo controls share global ordering");
    Check(queuedSm.IsHintDisplayRequested(),
          "ordered hint control reaches state machine");

    runtime::GameStateMachine exhaustedDirectSm(cfg);
    exhaustedDirectSm.Reset();
    runtime::InteractionDispatcher exhaustedDirect(exhaustedDirectSm);
    const uint32_t finalDirectSequence =
        exhaustedDirect.DispatchBoardObservation({
            .tMs = 2000U,
            .sequence = UINT32_MAX,
            .changedMask = 1ULL << 1U,
            .occupiedMask = 1ULL << 1U,
        });
    Check(finalDirectSequence == UINT32_MAX &&
              exhaustedDirectSm.GetLastBoardObservationSequence() ==
                  UINT32_MAX,
          "direct ingress can dispatch the final nonzero sequence");
    const uint32_t rejectedWrappedSequence =
        exhaustedDirect.DispatchBoardObservation({
            .tMs = 2001U,
            .changedMask = 1ULL << 2U,
            .occupiedMask = 1ULL << 2U,
        });
    Check(rejectedWrappedSequence == 0U,
          "direct ingress reports sequence exhaustion with zero");
    Check(exhaustedDirectSm.GetLastBoardObservationSequence() ==
              UINT32_MAX &&
              exhaustedDirectSm.GetLastBoardObservationOccupancyMask() ==
                  (1ULL << 1U),
          "direct sequence exhaustion cannot dispatch a zero-sequence event");

    runtime::GameStateMachine exhaustedOverrideSm(cfg);
    exhaustedOverrideSm.Reset();
    runtime::OrderedAppEventIngress exhaustedIngress(exhaustedOverrideSm);
    uint32_t overrideCalls = 0U;
    runtime::AppEvent finalIngressEvent = runtime::AppEvent::MakeTick(3000U);
    finalIngressEvent.seq = UINT32_MAX;
    Check(exhaustedIngress.Dispatch(
              finalIngressEvent, &overrideCalls, &CountIngressDispatch) ==
              UINT32_MAX &&
              overrideCalls == 1U,
          "direct override receives the final nonzero sequence once");
    Check(exhaustedIngress.Dispatch(
              runtime::AppEvent::MakeTick(3001U),
              &overrideCalls,
              &CountIngressDispatch) == 0U &&
              overrideCalls == 1U,
          "direct exhaustion rejects before invoking coordinator overrides");

    if (g_failures == 0) {
        std::puts("[EVENT_BUS] ok");
        return 0;
    }

    std::printf("[EVENT_BUS] failures=%d\n", g_failures);
    return 1;
}
