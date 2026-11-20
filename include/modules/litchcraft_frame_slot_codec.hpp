#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/litchcraft_frame_store.hpp"

namespace modules::litchcraft_frame_slot_codec {

// Independent, fixed-size storage representation. Wire frames keep their
// existing predecessor compression and are never interpreted as slot bytes.
inline constexpr size_t PACKED_FRAME_BYTES = 1091U;

class FrameSlotCodec final {
  public:
    static bool Encode(const litchcraft_frame_store::Frame& frame,
                       uint8_t* output, size_t capacity, size_t& outputSize);
    // A malformed slot leaves output untouched so recovery can try another root.
    static bool Decode(const uint8_t* bytes, size_t size,
                       litchcraft_frame_store::Frame& output);
};

} // namespace modules::litchcraft_frame_slot_codec
