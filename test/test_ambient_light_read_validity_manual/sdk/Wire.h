#pragma once
#include <cstdint>
struct TwoWire {
    unsigned readTransaction=0, failTransaction=0, shortTransaction=0;
    uint8_t reg=0, byte=0;
    void beginTransmission(uint8_t) { byte=0; }
    void write(uint8_t v) { reg=v; }
    uint8_t endTransmission(bool = true) { return (++readTransaction==failTransaction)?4:0; }
    uint8_t requestFrom(uint8_t,uint8_t count) { byte=0;return readTransaction==shortTransaction?1:count; }
    int read() { const uint16_t value=reg==4?100:0x1900;return byte++==0?value&255:value>>8; }
};
inline TwoWire Wire;
