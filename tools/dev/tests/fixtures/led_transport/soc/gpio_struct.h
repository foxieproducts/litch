#pragma once
#include "rmt_fixture.hpp"
struct FixtureGpioRegister {
    bool high;
    void operator=(uint32_t mask) {
        for (unsigned pin = 0; pin < 32U; ++pin) {
            if ((mask & (1UL << pin)) != 0U)
                fixture::events.push_back({high ? fixture::EventKind::High : fixture::EventKind::Low, pin});
        }
    }
};
inline struct { FixtureGpioRegister out_w1ts{true}, out_w1tc{false}; } GPIO;
