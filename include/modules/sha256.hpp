#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(LITCH_ESP32S3) && defined(ARDUINO_ARCH_ESP32) && \
    defined(__XTENSA__) && !defined(LITCH_NATIVE)
#include <mbedtls/sha256.h>
#endif

namespace modules {

class Sha256 final {
  public:
    using Digest = std::array<uint8_t, 32U>;

    Sha256() { Reset(); }
#if defined(LITCH_ESP32S3) && defined(ARDUINO_ARCH_ESP32) && \
    defined(__XTENSA__) && !defined(LITCH_NATIVE)
    ~Sha256();
#endif
    void Reset();
    bool Update(const uint8_t* bytes, size_t length);
    Digest Finish();

  private:
#if defined(LITCH_ESP32S3) && defined(ARDUINO_ARCH_ESP32) && \
    defined(__XTENSA__) && !defined(LITCH_NATIVE)
    // The pinned S3 SDK context owns only values; copies own independent streams.
    mbedtls_sha256_context m_context{};
    bool m_failed = false;
#else
    void Transform();

    std::array<uint32_t, 8U> m_state{};
    std::array<uint8_t, 64U> m_block{};
    size_t m_blockLength = 0U;
#endif
    uint64_t m_byteLength = 0U;
    bool m_finished = false;
};

} // namespace modules
