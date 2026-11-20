#pragma once

#include <cstdint>

// Native SDK boundary declarations; each actual-owner fixture records calls.
bool tud_disconnect();
bool tud_connect();
bool tud_ready();
uint8_t tud_cdc_n_get_line_state(uint8_t itf);
uint32_t tud_cdc_n_write_available(uint8_t itf);
uint32_t tud_cdc_n_write(uint8_t itf, const void* buffer, uint32_t size);
uint32_t tud_cdc_n_write_flush(uint8_t itf);
bool tud_mounted();

struct tusb_control_request_t {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};
static_assert(sizeof(tusb_control_request_t) == 8U);
constexpr uint8_t CONTROL_STAGE_IDLE = 0U;
constexpr uint8_t CONTROL_STAGE_SETUP = 1U;
constexpr uint8_t CONTROL_STAGE_DATA = 2U;
constexpr uint8_t CONTROL_STAGE_ACK = 3U;
constexpr uint8_t TUSB_REQ_CLEAR_FEATURE = 1U;
constexpr uint16_t TUSB_REQ_FEATURE_EDPT_HALT = 0U;
