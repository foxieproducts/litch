#include <cstdio>

#include "foxie_embedded/test_support/test_assert.hpp"
#include "runtime/button_input_event_queue.hpp"

int main() {
    fxe::test_support::TestResults results("BUTTON_INPUT_EVENT_QUEUE");
    results.Start();

    runtime::ButtonInputEventQueue queue;
    results.CheckEqInt(queue.Size(), 0, "queue starts empty");

    const runtime::ButtonInputEvent first{10, modules::UserButton::Left, fxe::Button::Event::Press,
                                          modules::UiOwner::P1};
    const runtime::ButtonInputEvent second{20, modules::UserButton::Right, fxe::Button::Event::Repeat,
                                           modules::UiOwner::P2};
    results.Check(queue.Enqueue(first), "enqueue first event");
    results.Check(queue.Enqueue(second), "enqueue second event");

    runtime::ButtonInputEvent out;
    results.Check(queue.TryDequeue(out), "dequeue first event");
    results.CheckEqU32(out.tMs, 10U, "first timestamp preserved");
    results.CheckEqInt(static_cast<int>(out.button), static_cast<int>(modules::UserButton::Left),
                       "first button preserved");
    results.CheckEqInt(static_cast<int>(out.event),
                       static_cast<int>(fxe::Button::Event::Press),
                       "first edge preserved");
    results.CheckEqInt(static_cast<int>(out.owner),
                       static_cast<int>(modules::UiOwner::P1),
                       "first owner preserved");
    results.Check(queue.TryDequeue(out), "dequeue second event");
    results.CheckEqU32(out.tMs, 20U, "second timestamp preserved");
    results.CheckEqInt(static_cast<int>(out.button), static_cast<int>(modules::UserButton::Right),
                       "second button preserved");
    results.CheckEqInt(static_cast<int>(out.event),
                       static_cast<int>(fxe::Button::Event::Repeat),
                       "second edge preserved");
    results.CheckEqInt(static_cast<int>(out.owner),
                       static_cast<int>(modules::UiOwner::P2),
                       "second owner preserved");

    const runtime::ButtonInputEvent simultaneousP2{
        30, modules::UserButton::P2X, fxe::Button::Event::Press,
        modules::UiOwner::P2};
    const runtime::ButtonInputEvent simultaneousP1{
        30, modules::UserButton::Up, fxe::Button::Event::Release,
        modules::UiOwner::P1};
    results.Check(queue.Enqueue(simultaneousP2),
                  "enqueue first simultaneous owner event");
    results.Check(queue.Enqueue(simultaneousP1),
                  "enqueue second simultaneous owner event");
    results.Check(queue.TryDequeue(out),
                  "dequeue first simultaneous owner event");
    results.CheckEqU32(out.tMs, 30U,
                       "same-timestamp FIFO keeps first timestamp");
    results.CheckEqInt(static_cast<int>(out.button),
                       static_cast<int>(modules::UserButton::P2X),
                       "same-timestamp FIFO keeps first button");
    results.CheckEqInt(static_cast<int>(out.owner),
                       static_cast<int>(modules::UiOwner::P2),
                       "same-timestamp FIFO keeps first owner");
    results.Check(queue.TryDequeue(out),
                  "dequeue second simultaneous owner event");
    results.CheckEqU32(out.tMs, 30U,
                       "same-timestamp FIFO keeps second timestamp");
    results.CheckEqInt(static_cast<int>(out.button),
                       static_cast<int>(modules::UserButton::Up),
                       "same-timestamp FIFO keeps second button");
    results.CheckEqInt(static_cast<int>(out.event),
                       static_cast<int>(fxe::Button::Event::Release),
                       "same-timestamp FIFO keeps second edge");
    results.CheckEqInt(static_cast<int>(out.owner),
                       static_cast<int>(modules::UiOwner::P1),
                       "same-timestamp FIFO keeps second owner");

    const runtime::ButtonInputEvent deferred{
        40, modules::UserButton::P1X, fxe::Button::Event::Release,
        modules::UiOwner::P1};
    const runtime::ButtonInputEvent replay{
        39, modules::UserButton::P1X, fxe::Button::Event::Press,
        modules::UiOwner::P1, true};
    results.Check(queue.Enqueue(deferred), "enqueue deferred current edge");
    results.Check(queue.EnqueueFront(replay), "prepend replayed first edge");
    results.Check(queue.TryDequeue(out) && out.tMs == 39U &&
                      out.litchcraftEntryArbitrated,
                  "prepended arbitration replay keeps original time and priority");
    results.Check(queue.TryDequeue(out) && out.tMs == 40U,
                  "deferred current edge follows arbitration replay");

    queue.Clear();
    for (uint8_t index = 0; index < runtime::ButtonInputEventQueue::CAPACITY; ++index) {
        results.Check(queue.Enqueue(runtime::ButtonInputEvent{index,
                                                              modules::UserButton::P1X,
                                                              fxe::Button::Event::Press,
                                                              modules::UiOwner::P1}),
                      "fill queue to capacity");
    }
    results.CheckEqInt(queue.Size(), runtime::ButtonInputEventQueue::CAPACITY,
                       "queue reports full capacity");
    results.Check(!queue.Enqueue(runtime::ButtonInputEvent{99,
                                                           modules::UserButton::Down,
                                                           fxe::Button::Event::Release,
                                                           modules::UiOwner::P2}),
                  "overflow drops newest event");

    results.Check(queue.TryDequeue(out), "dequeue oldest after overflow");
    results.CheckEqU32(out.tMs, 0U, "overflow keeps oldest queued event");
    while (queue.TryDequeue(out)) {
    }
    results.CheckEqInt(queue.Size(), 0, "queue drains fully");

    return results.Finish();
}
