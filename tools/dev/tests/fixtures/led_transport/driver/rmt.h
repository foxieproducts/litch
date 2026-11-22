#pragma once
#include "rmt_fixture.hpp"
using gpio_num_t = unsigned;
constexpr unsigned RMT_CHANNEL_0 = 0U, RMT_CHANNEL_1 = 1U, RMT_MODE_TX = 0U;
constexpr unsigned ESP_OK = 0U;
inline unsigned rmt_set_pin(unsigned, unsigned, gpio_num_t pin) {
    fixture::events.push_back({fixture::EventKind::Route, pin});
    return ESP_OK;
}
