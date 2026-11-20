#pragma once

#include <cstddef>
#include <cstdint>

namespace fxe {

// CRC of concatenated byte sequences from their finalized CRCs and the second
// sequence length. Zero second length preserves firstCrc; CRC of empty is 0.
// Fixed flash-resident shift powers keep each combination bounded without heap.
uint32_t CombineCrc32(uint32_t firstCrc, uint32_t secondCrc, size_t secondBytes);

inline uint8_t Checksum8(const uint8_t* data, const size_t size, const uint8_t seed = 0) {
    if (!data || size == 0) {
        return seed;
    }

    uint8_t sum = seed;
    for (size_t i = 0; i < size; ++i) {
        sum = static_cast<uint8_t>(sum + data[i]);
    }
    return sum;
}

inline uint16_t Crc16Ccitt(const uint8_t* data, const size_t size, const uint16_t seed = 0xFFFF) {
    if (!data || size == 0) {
        return seed;
    }

    uint16_t crc = seed;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000U) != 0) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021U);
            } else {
                crc = static_cast<uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

inline uint32_t Crc32(const uint8_t* data, const size_t size, const uint32_t seed = 0xFFFFFFFFU) {
    if (!data || size == 0) {
        return seed;
    }

    uint32_t crc = seed;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (crc & 1U) ? 0xEDB88320U : 0U;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

} // namespace fxe
