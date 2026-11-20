#pragma once
#include "rmt_fixture.hpp"
constexpr unsigned OUTPUT = 1U, LOW = 0U;
inline void delayMicroseconds(unsigned us) { fixture::Advance(us); }
inline void delay(unsigned long ms) { fixture::Advance(ms * 1000U); }
inline void pinMode(unsigned, unsigned) {}
inline void digitalWrite(unsigned pin, unsigned high) {
    fixture::events.push_back({high ? fixture::EventKind::High : fixture::EventKind::Low, pin});
}
