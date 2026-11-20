#pragma once

#include <cstddef>
#include <cstdint>

#include "foxie_embedded/rgb.hpp"

namespace fxe {

static constexpr uint64_t FNV1A64_OFFSET_BASIS = 14695981039346656037ULL;
static constexpr uint64_t FNV1A64_PRIME = 1099511628211ULL;

inline uint64_t Fnv1a64Update(uint64_t h, const uint8_t* data, const size_t len) {
    if (!data || len == 0) {
        return h;
    }

    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= FNV1A64_PRIME;
    }
    return h;
}

inline uint64_t HashRgbFrame(const Rgb* frame, const size_t count) {
    uint64_t h = FNV1A64_OFFSET_BASIS;
    if (!frame) {
        return h;
    }

    for (size_t i = 0; i < count; ++i) {
        const Rgb c = frame[i];
        const uint8_t bytes[3] = {c.r, c.g, c.b};
        h = Fnv1a64Update(h, bytes, sizeof(bytes));
    }
    return h;
}

inline uint64_t HashCombineU64(const uint64_t a, const uint64_t b) {
    uint64_t h = FNV1A64_OFFSET_BASIS;
    const uint8_t bytes[16] = {
        static_cast<uint8_t>(a >> 0),
        static_cast<uint8_t>(a >> 8),
        static_cast<uint8_t>(a >> 16),
        static_cast<uint8_t>(a >> 24),
        static_cast<uint8_t>(a >> 32),
        static_cast<uint8_t>(a >> 40),
        static_cast<uint8_t>(a >> 48),
        static_cast<uint8_t>(a >> 56),
        static_cast<uint8_t>(b >> 0),
        static_cast<uint8_t>(b >> 8),
        static_cast<uint8_t>(b >> 16),
        static_cast<uint8_t>(b >> 24),
        static_cast<uint8_t>(b >> 32),
        static_cast<uint8_t>(b >> 40),
        static_cast<uint8_t>(b >> 48),
        static_cast<uint8_t>(b >> 56),
    };
    h = Fnv1a64Update(h, bytes, sizeof(bytes));
    return h;
}

} // namespace fxe
