#include "hardware/lis2dw12_wire.hpp"

#ifdef ARDUINO

#include <Arduino.h>

namespace hardware {

Lis2dw12Transport Lis2dw12Wire::Transport() {
    return {this, &Read, &Write, &Delay};
}

bool Lis2dw12Wire::Read(void* context,
                       const uint8_t address,
                       const uint8_t reg,
                       uint8_t* const data,
                       const size_t length) {
    auto* const self = static_cast<Lis2dw12Wire*>(context);
    if (self == nullptr || self->m_wire == nullptr || data == nullptr ||
        length == 0U || length > 255U) {
        return false;
    }

    self->m_wire->beginTransmission(address);
    if (self->m_wire->write(reg) != 1U ||
        self->m_wire->endTransmission(false) != 0U) {
        return false;
    }

    const uint8_t requested = static_cast<uint8_t>(length);
    if (self->m_wire->requestFrom(address, requested) != requested) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        if (self->m_wire->available() <= 0) {
            return false;
        }
        data[index] = static_cast<uint8_t>(self->m_wire->read());
    }
    return true;
}

bool Lis2dw12Wire::Write(void* context,
                        const uint8_t address,
                        const uint8_t reg,
                        const uint8_t* const data,
                        const size_t length) {
    auto* const self = static_cast<Lis2dw12Wire*>(context);
    if (self == nullptr || self->m_wire == nullptr || data == nullptr ||
        length == 0U) {
        return false;
    }

    self->m_wire->beginTransmission(address);
    if (self->m_wire->write(reg) != 1U ||
        self->m_wire->write(data, length) != length) {
        (void)self->m_wire->endTransmission(true);
        return false;
    }
    return self->m_wire->endTransmission(true) == 0U;
}

void Lis2dw12Wire::Delay(void*, const uint32_t milliseconds) {
    delay(milliseconds);
}

} // namespace hardware

#endif
