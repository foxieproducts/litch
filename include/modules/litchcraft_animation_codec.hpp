#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/litchcraft_animation.hpp"

namespace modules::litchcraft_animation {

inline constexpr size_t ENCODED_HEADER_BYTES = 32U;
inline constexpr size_t ENCODED_EMITTER_BYTES = 6U;
inline constexpr size_t ENCODED_FRAME_BYTES = 1538U;
inline constexpr size_t MAX_ENCODED_BYTES = 12336U;

struct EncodedDocument final {
    std::array<uint8_t, MAX_ENCODED_BYTES> bytes{};
    size_t size = 0U;
    uint32_t crc32 = 0U;
};

struct DecodeWork final {
    uint32_t checksumCalls = 0U;
    size_t bytesDecoded = 0U;
};

bool EncodeDocument(const ValidatedDocument& document, EncodedDocument& out);
bool EncodeDocumentToBuffer(const ValidatedDocument& document, uint8_t* bytes,
                            size_t capacity, size_t& outSize,
                            uint32_t& outCrc32);
bool EncodeDocumentToBuffer(const Document& document, uint8_t* bytes,
                            size_t capacity, size_t& outSize,
                            uint32_t& outCrc32);

// Validates canonical bytes without decoding or mutating an authority. Receipt
// verification uses this path so an older committed body cannot replace a
// newer live edit.
bool ValidateEncodedDocument(const uint8_t* bytes, size_t size,
                             uint32_t expectedCrc32,
                             DecodeWork* work = nullptr);

// The activation owner supplies one reusable staging body. Admission validates
// bounds before checksum/decode; a failure leaves both authority and staging
// unchanged. Success swaps staging into authority, retaining the prior body in
// staging for reuse, so ingress never creates a fourth document body.
bool DecodeDocument(const uint8_t* bytes, size_t size, uint32_t expectedCrc32,
                    Document& staging, ValidatedDocument& out,
                    DecodeWork* work = nullptr);

} // namespace modules::litchcraft_animation
