#pragma once

#include <cstdint>
#include <tusb.h>

// Only the fields observed by the diagnostic callback, not a USB simulator.
constexpr uint8_t DCD_EVENT_BUS_RESET = 1U;
constexpr uint8_t DCD_EVENT_UNPLUGGED = 2U;
constexpr uint8_t DCD_EVENT_SUSPEND = 4U;
constexpr uint8_t DCD_EVENT_RESUME = 5U;
constexpr uint8_t DCD_EVENT_SETUP_RECEIVED = 6U;
constexpr uint8_t DCD_EVENT_XFER_COMPLETE = 7U;
constexpr uint8_t USBD_EVENT_FUNC_CALL = 8U;
struct dcd_event_t {
    uint8_t rhport = 0U;
    uint8_t event_id = 0U;
    tusb_control_request_t setup_received{};
    struct {
        uint8_t ep_addr = 0U;
        uint8_t result = 0U;
        uint32_t len = 0U;
    } xfer_complete;
    struct { void (*func)(void*) = nullptr; void* param = nullptr; } func_call;
};

extern "C" void dcd_edpt_clear_stall(uint8_t rhport, uint8_t epAddr);

extern "C" void dcd_event_handler(const dcd_event_t*, bool);
extern "C" void dcd_init(uint8_t);
