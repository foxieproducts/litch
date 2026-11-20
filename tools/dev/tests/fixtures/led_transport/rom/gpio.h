#pragma once
#include "rmt_fixture.hpp"
inline void gpio_matrix_out(unsigned pin, unsigned, bool, bool) {
    fixture::events.push_back({fixture::EventKind::DataLow, pin});
}
