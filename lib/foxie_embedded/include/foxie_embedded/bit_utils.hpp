#pragma once

#include <cstdint>

namespace fxe {

template <typename T>
constexpr T BitMask(const uint8_t bit) {
    return static_cast<T>(static_cast<T>(1) << bit);
}

template <typename T>
constexpr bool IsBitSet(const T value, const uint8_t bit) {
    return (value & BitMask<T>(bit)) != 0;
}

template <typename T>
constexpr T SetBit(const T value, const uint8_t bit) {
    return static_cast<T>(value | BitMask<T>(bit));
}

template <typename T>
constexpr T ClearBit(const T value, const uint8_t bit) {
    return static_cast<T>(value & static_cast<T>(~BitMask<T>(bit)));
}

template <typename T>
constexpr T ToggleBit(const T value, const uint8_t bit) {
    return static_cast<T>(value ^ BitMask<T>(bit));
}

} // namespace fxe
