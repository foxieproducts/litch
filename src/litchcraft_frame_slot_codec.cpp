#include "modules/litchcraft_frame_slot_codec.hpp"

#include <algorithm>

#include "foxie_embedded/byte_order.hpp"

namespace modules::litchcraft_frame_slot_codec {
namespace {

constexpr uint8_t SLOT_TAG = 1U;
constexpr size_t COLOR_OFFSET = 3U;
constexpr size_t EFFECT_OFFSET = COLOR_OFFSET +
    litchcraft_frame_store::EMITTER_COUNT * 3U;
constexpr uint16_t EFFECT_DURATION_STEPS = 96U;
constexpr uint16_t MAX_EFFECT_CODE = 8U * EFFECT_DURATION_STEPS;

bool ValidDuration(const uint16_t durationMs) {
    return durationMs >= 50U && durationMs <= 5000U;
}

bool EncodeEffect(const litchcraft_frame_store::Emitter& emitter,
                  uint16_t& code) {
    if (emitter.effect == 0U) {
        if (emitter.effectDurationMs != 0U) return false;
        code = 0U;
        return true;
    }
    if (emitter.effect > 8U || emitter.effectDurationMs < 500U ||
        emitter.effectDurationMs > 10000U ||
        emitter.effectDurationMs % 100U != 0U) {
        return false;
    }
    code = static_cast<uint16_t>(
        1U + (emitter.effect - 1U) * EFFECT_DURATION_STEPS +
        (emitter.effectDurationMs - 500U) / 100U);
    return true;
}

uint16_t ReadEffectCode(const uint8_t* const bytes, const size_t index) {
    uint16_t code = 0U;
    const size_t firstBit = index * 10U;
    for (uint8_t bit = 0U; bit < 10U; ++bit) {
        const size_t source = firstBit + bit;
        if ((bytes[EFFECT_OFFSET + source / 8U] &
             static_cast<uint8_t>(1U << (source % 8U))) != 0U) {
            code |= static_cast<uint16_t>(1U << bit);
        }
    }
    return code;
}

} // namespace

static_assert(EFFECT_OFFSET + litchcraft_frame_store::EMITTER_COUNT * 10U / 8U ==
                  PACKED_FRAME_BYTES,
              "packed slot contains one exact independent frame");

bool FrameSlotCodec::Encode(const litchcraft_frame_store::Frame& frame,
                            uint8_t* const output, const size_t capacity,
                            size_t& outputSize) {
    outputSize = 0U;
    if (output == nullptr || capacity < PACKED_FRAME_BYTES ||
        !ValidDuration(frame.durationMs)) {
        return false;
    }
    for (const auto& emitter : frame.emitters) {
        uint16_t code = 0U;
        if (!EncodeEffect(emitter, code)) return false;
    }
    output[0] = SLOT_TAG;
    fxe::WriteU16LE(frame.durationMs, output + 1U);
    std::fill_n(output + EFFECT_OFFSET,
                PACKED_FRAME_BYTES - EFFECT_OFFSET, 0U);
    for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        const auto& emitter = frame.emitters[index];
        const size_t color = COLOR_OFFSET + index * 3U;
        output[color] = emitter.color.r;
        output[color + 1U] = emitter.color.g;
        output[color + 2U] = emitter.color.b;
        uint16_t code = 0U;
        (void)EncodeEffect(emitter, code);
        const size_t firstBit = index * 10U;
        for (uint8_t bit = 0U; bit < 10U; ++bit) {
            if ((code & static_cast<uint16_t>(1U << bit)) == 0U) continue;
            const size_t destination = firstBit + bit;
            output[EFFECT_OFFSET + destination / 8U] |=
                static_cast<uint8_t>(1U << (destination % 8U));
        }
    }
    outputSize = PACKED_FRAME_BYTES;
    return true;
}

bool FrameSlotCodec::Decode(const uint8_t* const bytes, const size_t size,
                            litchcraft_frame_store::Frame& output) {
    if (bytes == nullptr || size != PACKED_FRAME_BYTES ||
        bytes[0] != SLOT_TAG) {
        return false;
    }
    const uint16_t durationMs = fxe::ReadU16LE(bytes + 1U);
    if (!ValidDuration(durationMs)) return false;
    for (size_t index = 0U; index < litchcraft_frame_store::EMITTER_COUNT;
         ++index) {
        if (ReadEffectCode(bytes, index) > MAX_EFFECT_CODE) return false;
    }
    output.durationMs = durationMs;
    for (size_t index = 0U; index < output.emitters.size(); ++index) {
        auto& emitter = output.emitters[index];
        const size_t color = COLOR_OFFSET + index * 3U;
        emitter.color = {bytes[color], bytes[color + 1U], bytes[color + 2U]};
        const uint16_t code = ReadEffectCode(bytes, index);
        if (code == 0U) {
            emitter.effect = 0U;
            emitter.effectDurationMs = 0U;
        } else {
            const uint16_t value = code - 1U;
            emitter.effect = static_cast<uint8_t>(
                value / EFFECT_DURATION_STEPS + 1U);
            emitter.effectDurationMs = static_cast<uint16_t>(
                500U + (value % EFFECT_DURATION_STEPS) * 100U);
        }
    }
    return true;
}

} // namespace modules::litchcraft_frame_slot_codec
