#pragma once
// Narrow native boundary for the installed NimBLE host-event lifecycle.
struct ble_npl_event;
using ble_npl_event_fn = void(ble_npl_event*);
struct ble_npl_event { bool queued = false; ble_npl_event_fn* fn = nullptr; void* arg = nullptr; };
struct ble_npl_eventq {};
inline void ble_npl_event_init(ble_npl_event* event, ble_npl_event_fn* fn, void* arg) {
    *event = {false, fn, arg};
}
inline void* ble_npl_event_get_arg(ble_npl_event* event) { return event->arg; }
void ble_npl_eventq_put(ble_npl_eventq*, ble_npl_event*);
