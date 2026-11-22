#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/litchcraft_frame_store.hpp"

namespace modules::litchcraft_wire_codec {

inline constexpr size_t MAX_ENCODED_FRAME_BYTES = 1123U;

enum class RgbOnlyMode : uint8_t { Disabled, Enabled };

class FrameCodec final {
  public:
    static constexpr uint8_t RAW_TAG = 2U;
    static bool Encode(const litchcraft_frame_store::Frame* previous,
                       const litchcraft_frame_store::Frame& frame,
                       uint8_t* output, size_t capacity, size_t& outputSize,
                       RgbOnlyMode rgbOnlyMode = RgbOnlyMode::Disabled);
    static bool Decode(const litchcraft_frame_store::Frame* previous,
                       const uint8_t* bytes, size_t size,
                       litchcraft_frame_store::Frame& output,
                       RgbOnlyMode rgbOnlyMode = RgbOnlyMode::Disabled);
};

} // namespace modules::litchcraft_wire_codec
