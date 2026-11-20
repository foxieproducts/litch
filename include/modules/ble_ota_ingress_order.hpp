#pragma once

#include <cstdint>

namespace modules {

enum class BleOtaIngressSource : uint8_t {
    None = 0,
    Control,
    Chunk,
};

struct BleOtaIngressHead {
    bool hasControl = false;
    uint32_t controlOrdinal = 0U;
    bool hasChunk = false;
    uint32_t chunkOrdinal = 0U;
};

inline bool IsBleOtaOrdinalEarlier(const uint32_t lhs, const uint32_t rhs) {
    return static_cast<int32_t>(lhs - rhs) < 0;
}

inline BleOtaIngressSource SelectNextBleOtaIngress(const BleOtaIngressHead& head) {
    if (!head.hasControl) {
        return head.hasChunk ? BleOtaIngressSource::Chunk : BleOtaIngressSource::None;
    }
    if (!head.hasChunk) {
        return BleOtaIngressSource::Control;
    }
    return IsBleOtaOrdinalEarlier(head.chunkOrdinal, head.controlOrdinal)
               ? BleOtaIngressSource::Chunk
               : BleOtaIngressSource::Control;
}

} // namespace modules
