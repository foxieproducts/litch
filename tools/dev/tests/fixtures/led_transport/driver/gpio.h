#pragma once
#include "rmt_fixture.hpp"
using gpio_num_t = unsigned;
inline void gpio_set_level(gpio_num_t pin, unsigned high) {
    fixture::events.push_back({high ? fixture::EventKind::High : fixture::EventKind::Low, pin});
}
