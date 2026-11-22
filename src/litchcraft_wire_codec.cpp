#include "modules/litchcraft_wire_codec.hpp"

#include "foxie_embedded/byte_order.hpp"

namespace modules::litchcraft_wire_codec {
namespace {

constexpr uint8_t REPEAT = 0U;
constexpr uint8_t SPARSE = 1U;
constexpr uint8_t RAW = FrameCodec::RAW_TAG;
constexpr uint8_t INDEXED = 3U;
constexpr uint8_t RGB_RAW = 4U;
constexpr uint8_t RGB_BITMAP = 5U;
constexpr size_t HEADER_BYTES = 3U;
constexpr size_t CHANGE_BITMAP_BYTES = 32U;
constexpr size_t SPARSE_EMITTER_BYTES = 5U;
constexpr size_t INDEXED_EMITTER_BYTES = 6U;
constexpr size_t RAW_EFFECT_BYTES = (256U * 11U + 7U) / 8U;
constexpr size_t RGB_RAW_BYTES =
    HEADER_BYTES + litchcraft_frame_store::EMITTER_COUNT * 3U;

bool ValidEmitter(const litchcraft_frame_store::Emitter& emitter) {
    return emitter.effect <= 8U && emitter.effectDurationMs % 100U == 0U &&
           ((emitter.effect == 0U && emitter.effectDurationMs == 0U) ||
            (emitter.effect != 0U && emitter.effectDurationMs >= 500U &&
             emitter.effectDurationMs <= 10000U));
}

bool EqualEmitter(const litchcraft_frame_store::Emitter& left,
                  const litchcraft_frame_store::Emitter& right) {
    return left.color.r == right.color.r && left.color.g == right.color.g &&
           left.color.b == right.color.b && left.effect == right.effect &&
           left.effectDurationMs == right.effectDurationMs;
}

bool EqualEmitters(const litchcraft_frame_store::Frame& left,
                   const litchcraft_frame_store::Frame& right) {
    for (size_t index = 0U; index < left.emitters.size(); ++index) {
        const auto& a = left.emitters[index];
        const auto& b = right.emitters[index];
        if (!EqualEmitter(a, b)) return false;
    }
    return true;
}

bool AllEffectsOff(const litchcraft_frame_store::Frame& frame) {
    for (const auto& emitter : frame.emitters) {
        if (emitter.effect != 0U) return false;
    }
    return true;
}

bool ValidDuration(const uint16_t durationMs) {
    return durationMs >= 50U && durationMs <= 5000U;
}

uint16_t PackedEffect(const litchcraft_frame_store::Emitter& emitter) {
    return static_cast<uint16_t>(emitter.effect) |
           static_cast<uint16_t>(emitter.effectDurationMs / 100U) << 4U;
}

void DecodeEmitter(const uint8_t* const bytes,
                   litchcraft_frame_store::Emitter& emitter) {
    emitter.color = {bytes[0], bytes[1], bytes[2]};
    const uint16_t packed = fxe::ReadU16LE(bytes + 3U);
    emitter.effect = static_cast<uint8_t>(packed & 0x0fU);
    emitter.effectDurationMs = static_cast<uint16_t>((packed >> 4U) * 100U);
}

void EncodeRaw(const litchcraft_frame_store::Frame& frame, uint8_t* output) {
    output[0] = RAW;
    fxe::WriteU16LE(frame.durationMs, output + 1U);
    size_t offset = HEADER_BYTES;
    for (const auto& emitter : frame.emitters) {
        output[offset++] = emitter.color.r;
        output[offset++] = emitter.color.g;
        output[offset++] = emitter.color.b;
    }
    for (size_t index = 0U; index < RAW_EFFECT_BYTES; ++index) {
        output[offset + index] = 0U;
    }
    for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        const uint16_t packed = PackedEffect(frame.emitters[index]);
        const size_t firstBit = index * 11U;
        for (uint8_t bit = 0U; bit < 11U; ++bit) {
            if ((packed & (1U << bit)) == 0U) continue;
            const size_t destination = firstBit + bit;
            output[offset + destination / 8U] |=
                static_cast<uint8_t>(1U << (destination % 8U));
        }
    }
}

void EncodeRgbRaw(const litchcraft_frame_store::Frame& frame,
                  uint8_t* output) {
    output[0] = RGB_RAW;
    fxe::WriteU16LE(frame.durationMs, output + 1U);
    size_t offset = HEADER_BYTES;
    for (const auto& emitter : frame.emitters) {
        output[offset++] = emitter.color.r;
        output[offset++] = emitter.color.g;
        output[offset++] = emitter.color.b;
    }
}

void DecodeRgbRaw(const uint8_t* const bytes,
                  litchcraft_frame_store::Frame& frame) {
    size_t offset = HEADER_BYTES;
    for (auto& emitter : frame.emitters) {
        emitter.color = {bytes[offset], bytes[offset + 1U], bytes[offset + 2U]};
        emitter.effect = 0U;
        emitter.effectDurationMs = 0U;
        offset += 3U;
    }
}

bool DecodeRaw(const uint8_t* const bytes,
               litchcraft_frame_store::Frame& frame) {
    size_t offset = HEADER_BYTES;
    for (auto& emitter : frame.emitters) {
        emitter.color = {bytes[offset], bytes[offset + 1U], bytes[offset + 2U]};
        offset += 3U;
    }
    for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        uint16_t packed = 0U;
        const size_t firstBit = index * 11U;
        for (uint8_t bit = 0U; bit < 11U; ++bit) {
            const size_t source = firstBit + bit;
            if ((bytes[offset + source / 8U] &
                 (1U << (source % 8U))) != 0U) {
                packed |= static_cast<uint16_t>(1U << bit);
            }
        }
        frame.emitters[index].effect = static_cast<uint8_t>(packed & 0x0fU);
        frame.emitters[index].effectDurationMs =
            static_cast<uint16_t>((packed >> 4U) * 100U);
        if (!ValidEmitter(frame.emitters[index])) return false;
    }
    return true;
}

bool ValidRawEffects(const uint8_t* const bytes) {
    const size_t offset = HEADER_BYTES +
        litchcraft_frame_store::EMITTER_COUNT * 3U;
    for (size_t index = 0U;
         index < litchcraft_frame_store::EMITTER_COUNT; ++index) {
        uint16_t packed = 0U;
        const size_t firstBit = index * 11U;
        for (uint8_t bit = 0U; bit < 11U; ++bit) {
            const size_t source = firstBit + bit;
            if ((bytes[offset + source / 8U] &
                 (1U << (source % 8U))) != 0U) {
                packed |= static_cast<uint16_t>(1U << bit);
            }
        }
        litchcraft_frame_store::Emitter emitter{};
        emitter.effect = static_cast<uint8_t>(packed & 0x0fU);
        emitter.effectDurationMs =
            static_cast<uint16_t>((packed >> 4U) * 100U);
        if (!ValidEmitter(emitter)) return false;
    }
    return true;
}

} // namespace

bool FrameCodec::Encode(const litchcraft_frame_store::Frame* const previous,
                        const litchcraft_frame_store::Frame& frame,
                        uint8_t* const output, const size_t capacity,
                        size_t& outputSize,
                        const RgbOnlyMode rgbOnlyMode) {
    outputSize = 0U;
    if (output == nullptr || capacity < HEADER_BYTES ||
        !ValidDuration(frame.durationMs)) {
        return false;
    }
    for (const auto& emitter : frame.emitters) {
        if (!ValidEmitter(emitter)) return false;
    }
    if (previous == nullptr) {
        if (rgbOnlyMode == RgbOnlyMode::Enabled && AllEffectsOff(frame)) {
            if (capacity < RGB_RAW_BYTES) return false;
            EncodeRgbRaw(frame, output);
            outputSize = RGB_RAW_BYTES;
            return true;
        }
        if (capacity < MAX_ENCODED_FRAME_BYTES) return false;
        EncodeRaw(frame, output);
        outputSize = MAX_ENCODED_FRAME_BYTES;
        return true;
    }
    if (EqualEmitters(*previous, frame)) {
        output[0] = REPEAT;
        fxe::WriteU16LE(frame.durationMs, output + 1U);
        outputSize = HEADER_BYTES;
        return true;
    }
    size_t changed = 0U;
    for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        if (!EqualEmitter(previous->emitters[index], frame.emitters[index])) {
            ++changed;
        }
    }
    const size_t required = HEADER_BYTES + CHANGE_BITMAP_BYTES +
                            changed * SPARSE_EMITTER_BYTES;
    const size_t indexedRequired = HEADER_BYTES + 1U +
                                   changed * INDEXED_EMITTER_BYTES;
    if (rgbOnlyMode == RgbOnlyMode::Enabled && AllEffectsOff(frame)) {
        const size_t legacySize = indexedRequired < required
            ? indexedRequired
            : (required <= MAX_ENCODED_FRAME_BYTES
                ? required : MAX_ENCODED_FRAME_BYTES);
        const size_t rgbBitmapSize = HEADER_BYTES + CHANGE_BITMAP_BYTES +
                                     changed * 3U;
        if (rgbBitmapSize < legacySize && rgbBitmapSize < RGB_RAW_BYTES) {
            if (capacity < rgbBitmapSize) return false;
            output[0] = RGB_BITMAP;
            fxe::WriteU16LE(frame.durationMs, output + 1U);
            for (size_t index = 0U; index < CHANGE_BITMAP_BYTES; ++index) {
                output[HEADER_BYTES + index] = 0U;
            }
            size_t offset = HEADER_BYTES + CHANGE_BITMAP_BYTES;
            for (size_t index = 0U; index < frame.emitters.size(); ++index) {
                const auto& emitter = frame.emitters[index];
                if (EqualEmitter(previous->emitters[index], emitter)) continue;
                output[HEADER_BYTES + index / 8U] |=
                    static_cast<uint8_t>(1U << (index % 8U));
                output[offset++] = emitter.color.r;
                output[offset++] = emitter.color.g;
                output[offset++] = emitter.color.b;
            }
            outputSize = rgbBitmapSize;
            return true;
        }
        if (RGB_RAW_BYTES < legacySize) {
            if (capacity < RGB_RAW_BYTES) return false;
            EncodeRgbRaw(frame, output);
            outputSize = RGB_RAW_BYTES;
            return true;
        }
    }
    if (indexedRequired < required) {
        if (indexedRequired > capacity || changed > 255U) return false;
        output[0] = INDEXED;
        fxe::WriteU16LE(frame.durationMs, output + 1U);
        output[HEADER_BYTES] = static_cast<uint8_t>(changed);
        size_t offset = HEADER_BYTES + 1U;
        for (size_t index = 0U; index < frame.emitters.size(); ++index) {
            const auto& emitter = frame.emitters[index];
            if (EqualEmitter(previous->emitters[index], emitter)) continue;
            output[offset++] = static_cast<uint8_t>(index);
            output[offset++] = emitter.color.r;
            output[offset++] = emitter.color.g;
            output[offset++] = emitter.color.b;
            fxe::WriteU16LE(PackedEffect(emitter), output + offset);
            offset += 2U;
        }
        outputSize = indexedRequired;
        return true;
    }
    if (required > MAX_ENCODED_FRAME_BYTES) {
        if (capacity < MAX_ENCODED_FRAME_BYTES) return false;
        EncodeRaw(frame, output);
        outputSize = MAX_ENCODED_FRAME_BYTES;
        return true;
    }
    if (required > capacity) return false;
    output[0] = SPARSE;
    fxe::WriteU16LE(frame.durationMs, output + 1U);
    for (size_t index = 0U; index < CHANGE_BITMAP_BYTES; ++index) {
        output[HEADER_BYTES + index] = 0U;
    }
    size_t offset = HEADER_BYTES + CHANGE_BITMAP_BYTES;
    for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        const auto& emitter = frame.emitters[index];
        if (EqualEmitter(previous->emitters[index], emitter)) continue;
        output[HEADER_BYTES + index / 8U] |=
            static_cast<uint8_t>(1U << (index % 8U));
        output[offset++] = emitter.color.r;
        output[offset++] = emitter.color.g;
        output[offset++] = emitter.color.b;
        fxe::WriteU16LE(PackedEffect(emitter), output + offset);
        offset += 2U;
    }
    outputSize = required;
    return true;
}

bool FrameCodec::Decode(const litchcraft_frame_store::Frame* const previous,
                        const uint8_t* const bytes, const size_t size,
                        litchcraft_frame_store::Frame& output,
                        const RgbOnlyMode rgbOnlyMode) {
    if (bytes == nullptr || size < HEADER_BYTES) {
        return false;
    }
    const uint16_t durationMs = fxe::ReadU16LE(bytes + 1U);
    if (!ValidDuration(durationMs)) return false;
    if (bytes[0] == REPEAT) {
        if (previous == nullptr || size != HEADER_BYTES) return false;
    } else if (bytes[0] == SPARSE) {
        if (previous == nullptr ||
            size < HEADER_BYTES + CHANGE_BITMAP_BYTES) return false;
        size_t offset = HEADER_BYTES + CHANGE_BITMAP_BYTES;
        for (size_t index = 0U; index < output.emitters.size(); ++index) {
            if ((bytes[HEADER_BYTES + index / 8U] &
                 (1U << (index % 8U))) == 0U) continue;
            if (offset + SPARSE_EMITTER_BYTES > size) return false;
            litchcraft_frame_store::Emitter emitter{};
            DecodeEmitter(bytes + offset, emitter);
            if (!ValidEmitter(emitter)) return false;
            offset += SPARSE_EMITTER_BYTES;
        }
        if (offset != size) return false;
    } else if (bytes[0] == INDEXED) {
        if (previous == nullptr || size < HEADER_BYTES + 1U) return false;
        const size_t changed = bytes[HEADER_BYTES];
        if (changed == 0U ||
            size != HEADER_BYTES + 1U + changed * INDEXED_EMITTER_BYTES) {
            return false;
        }
        size_t offset = HEADER_BYTES + 1U;
        size_t priorIndex = 0U;
        for (size_t change = 0U; change < changed; ++change) {
            const size_t index = bytes[offset++];
            if (change != 0U && index <= priorIndex) return false;
            priorIndex = index;
            litchcraft_frame_store::Emitter emitter{};
            DecodeEmitter(bytes + offset, emitter);
            if (!ValidEmitter(emitter)) return false;
            offset += SPARSE_EMITTER_BYTES;
        }
    } else if (bytes[0] == RAW) {
        if (size != MAX_ENCODED_FRAME_BYTES || !ValidRawEffects(bytes)) {
            return false;
        }
    } else if (bytes[0] == RGB_RAW) {
        if (rgbOnlyMode != RgbOnlyMode::Enabled || size != RGB_RAW_BYTES) {
            return false;
        }
    } else if (bytes[0] == RGB_BITMAP) {
        if (rgbOnlyMode != RgbOnlyMode::Enabled || previous == nullptr ||
            size < HEADER_BYTES + CHANGE_BITMAP_BYTES) return false;
        size_t offset = HEADER_BYTES + CHANGE_BITMAP_BYTES;
        for (size_t index = 0U; index < output.emitters.size(); ++index) {
            if ((bytes[HEADER_BYTES + index / 8U] &
                 (1U << (index % 8U))) == 0U) {
                const auto& inherited = previous->emitters[index];
                if (inherited.effect != 0U ||
                    inherited.effectDurationMs != 0U) return false;
                continue;
            }
            if (offset + 3U > size) return false;
            offset += 3U;
        }
        if (offset != size) return false;
    } else {
        return false;
    }

    output = previous == nullptr ? litchcraft_frame_store::Frame{} : *previous;
    output.durationMs = durationMs;
    if (bytes[0] == SPARSE) {
        size_t offset = HEADER_BYTES + CHANGE_BITMAP_BYTES;
        for (size_t index = 0U; index < output.emitters.size(); ++index) {
            if ((bytes[HEADER_BYTES + index / 8U] &
                 (1U << (index % 8U))) == 0U) continue;
            DecodeEmitter(bytes + offset, output.emitters[index]);
            offset += SPARSE_EMITTER_BYTES;
        }
    } else if (bytes[0] == INDEXED) {
        const size_t changed = bytes[HEADER_BYTES];
        size_t offset = HEADER_BYTES + 1U;
        for (size_t change = 0U; change < changed; ++change) {
            const size_t index = bytes[offset++];
            DecodeEmitter(bytes + offset, output.emitters[index]);
            offset += SPARSE_EMITTER_BYTES;
        }
    } else if (bytes[0] == RAW) {
        (void)DecodeRaw(bytes, output);
    } else if (bytes[0] == RGB_RAW) {
        DecodeRgbRaw(bytes, output);
    } else if (bytes[0] == RGB_BITMAP) {
        size_t offset = HEADER_BYTES + CHANGE_BITMAP_BYTES;
        for (size_t index = 0U; index < output.emitters.size(); ++index) {
            if ((bytes[HEADER_BYTES + index / 8U] &
                 (1U << (index % 8U))) == 0U) continue;
            auto& emitter = output.emitters[index];
            emitter.color = {bytes[offset], bytes[offset + 1U],
                             bytes[offset + 2U]};
            emitter.effect = 0U;
            emitter.effectDurationMs = 0U;
            offset += 3U;
        }
    }
    return true;
}

} // namespace modules::litchcraft_wire_codec
