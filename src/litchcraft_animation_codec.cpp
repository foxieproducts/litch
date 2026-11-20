#include "modules/litchcraft_animation_codec.hpp"

#include "foxie_embedded/checksum.hpp"

namespace modules::litchcraft_animation {
namespace {

void WriteLe16(uint8_t* const out, const uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8U);
}

void WriteLe32(uint8_t* const out, const uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8U);
    out[2] = static_cast<uint8_t>(value >> 16U);
    out[3] = static_cast<uint8_t>(value >> 24U);
}

uint16_t ReadLe16(const uint8_t* const bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
}

uint32_t ReadLe32(const uint8_t* const bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) |
           (static_cast<uint32_t>(bytes[3]) << 24U);
}

size_t EncodedSize(const uint8_t frameCount) {
    return ENCODED_HEADER_BYTES + static_cast<size_t>(frameCount) * ENCODED_FRAME_BYTES;
}

bool ValidateBytes(const uint8_t* const bytes, const size_t size,
                   const uint32_t expectedCrc32, DecodeWork* const work) {
    if (bytes == nullptr || size < ENCODED_HEADER_BYTES ||
        size > MAX_ENCODED_BYTES ||
        !IsValidDocumentHeader(bytes[0], bytes[1], bytes[2], bytes[3],
                               ReadLe32(bytes + 4U)) ||
        size != EncodedSize(bytes[1])) {
        return false;
    }
    if (work != nullptr) {
        ++work->checksumCalls;
    }
    if (fxe::Crc32(bytes, size) != expectedCrc32) {
        return false;
    }
    size_t offset = ENCODED_HEADER_BYTES;
    for (uint8_t frameIndex = 0U; frameIndex < bytes[1]; ++frameIndex) {
        if (!IsValidFrameDuration(ReadLe16(bytes + offset))) {
            return false;
        }
        offset += 2U;
        for (size_t emitterIndex = 0U; emitterIndex < EMITTER_COUNT;
             ++emitterIndex) {
            const uint8_t effectId = bytes[offset + 3U];
            if (!IsValidEffect(effectId) ||
                !IsValidEffectDuration(static_cast<Effect>(effectId),
                                       ReadLe16(bytes + offset + 4U))) {
                return false;
            }
            offset += ENCODED_EMITTER_BYTES;
        }
    }
    return offset == size;
}

void ResetStaging(Document& staging) {
    staging.frameCount = 1U;
    staging.topology = 3U;
    staging.playbackOnEntry = false;
    staging.sparkleSeed = 1U;
    for (Rgb& favorite : staging.favorites) {
        favorite = RGB_BLACK;
    }
    for (Frame& frame : staging.frames) {
        frame.durationMs = DEFAULT_FRAME_DURATION_MS;
        for (Emitter& emitter : frame.emitters) {
            emitter.color = RGB_BLACK;
            emitter.effect = Effect::Off;
            emitter.durationMs = 0U;
        }
    }
}

} // namespace

bool EncodeDocumentToBuffer(const Document& document,
                            uint8_t* const bytes, const size_t capacity,
                            size_t& outSize, uint32_t& outCrc32) {
    if (!IsValidDocument(document)) return false;
    const size_t size = EncodedSize(document.frameCount);
    if (bytes == nullptr || capacity < size) return false;
    bytes[0] = SCHEMA;
    bytes[1] = document.frameCount;
    bytes[2] = document.topology;
    bytes[3] = document.playbackOnEntry ? 1U : 0U;
    WriteLe32(bytes + 4U, document.sparkleSeed);
    size_t offset = 8U;
    for (const Rgb favorite : document.favorites) {
        bytes[offset++] = favorite.r;
        bytes[offset++] = favorite.g;
        bytes[offset++] = favorite.b;
    }
    for (uint8_t frameIndex = 0U; frameIndex < document.frameCount; ++frameIndex) {
        const Frame& frame = document.frames[frameIndex];
        WriteLe16(bytes + offset, frame.durationMs);
        offset += 2U;
        for (const Emitter& emitter : frame.emitters) {
            bytes[offset++] = emitter.color.r;
            bytes[offset++] = emitter.color.g;
            bytes[offset++] = emitter.color.b;
            bytes[offset++] = static_cast<uint8_t>(emitter.effect);
            WriteLe16(bytes + offset, emitter.durationMs);
            offset += 2U;
        }
    }
    outSize = size;
    outCrc32 = fxe::Crc32(bytes, size);
    return true;
}

bool EncodeDocumentToBuffer(const ValidatedDocument& adopted,
                            uint8_t* const bytes, const size_t capacity,
                            size_t& outSize, uint32_t& outCrc32) {
    return adopted.IsAdopted() &&
        EncodeDocumentToBuffer(adopted.Value(), bytes, capacity, outSize,
                               outCrc32);
}

bool EncodeDocument(const ValidatedDocument& adopted, EncodedDocument& out) {
    return EncodeDocumentToBuffer(adopted, out.bytes.data(), out.bytes.size(),
                                  out.size, out.crc32);
}

bool ValidateEncodedDocument(const uint8_t* const bytes, const size_t size,
                             const uint32_t expectedCrc32,
                             DecodeWork* const work) {
    if (work != nullptr) {
        *work = DecodeWork{};
    }
    return ValidateBytes(bytes, size, expectedCrc32, work);
}

bool DecodeDocument(const uint8_t* const bytes, const size_t size,
                    const uint32_t expectedCrc32, Document& staging,
                    ValidatedDocument& out, DecodeWork* const work) {
    if (!ValidateEncodedDocument(bytes, size, expectedCrc32, work)) {
        return false;
    }
    ResetStaging(staging);
    staging.frameCount = bytes[1];
    staging.topology = bytes[2];
    staging.playbackOnEntry = (bytes[3] & 1U) != 0U;
    staging.sparkleSeed = ReadLe32(bytes + 4U);
    size_t offset = 8U;
    for (Rgb& favorite : staging.favorites) {
        favorite = {bytes[offset], bytes[offset + 1U], bytes[offset + 2U]};
        offset += 3U;
    }
    for (uint8_t frameIndex = 0U; frameIndex < staging.frameCount; ++frameIndex) {
        Frame& frame = staging.frames[frameIndex];
        frame.durationMs = ReadLe16(bytes + offset);
        offset += 2U;
        for (Emitter& emitter : frame.emitters) {
            emitter.color = {bytes[offset], bytes[offset + 1U], bytes[offset + 2U]};
            emitter.effect = static_cast<Effect>(bytes[offset + 3U]);
            emitter.durationMs = ReadLe16(bytes + offset + 4U);
            offset += ENCODED_EMITTER_BYTES;
        }
    }
    if (work != nullptr) {
        work->bytesDecoded = size;
    }
    return ValidatedDocument::TryAdoptBySwap(staging, out);
}

} // namespace modules::litchcraft_animation
