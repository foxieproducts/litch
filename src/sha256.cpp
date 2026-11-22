#include "modules/sha256.hpp"

#include <algorithm>

namespace modules {

#if defined(LITCH_ESP32S3) && defined(ARDUINO_ARCH_ESP32) && \
    defined(__XTENSA__) && !defined(LITCH_NATIVE)

Sha256::~Sha256() {
    mbedtls_sha256_free(&m_context);
}

void Sha256::Reset() {
    mbedtls_sha256_init(&m_context);
    m_failed = mbedtls_sha256_starts_ret(&m_context, 0) != 0;
    m_byteLength = 0U;
    m_finished = false;
}

bool Sha256::Update(const uint8_t* const bytes, const size_t length) {
    if (m_finished || m_failed || (bytes == nullptr && length != 0U) ||
        length > UINT64_MAX - m_byteLength) {
        return false;
    }
    if (length != 0U &&
        mbedtls_sha256_update_ret(&m_context, bytes, length) != 0) {
        m_failed = true;
        return false;
    }
    m_byteLength += length;
    return true;
}

Sha256::Digest Sha256::Finish() {
    Digest digest{};
    if (m_finished || m_failed || m_byteLength > UINT64_MAX / 8U) return digest;
    m_finished = true;
    if (mbedtls_sha256_finish_ret(&m_context, digest.data()) != 0) {
        m_failed = true;
        digest.fill(0U);
    }
    return digest;
}

#else

namespace {

constexpr std::array<uint32_t, 64U> ROUND_CONSTANTS = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
    0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
    0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
    0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
    0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

uint32_t RotateRight(const uint32_t value, const uint8_t count) {
    return (value >> count) | (value << (32U - count));
}

} // namespace

void Sha256::Reset() {
    m_state = {0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
               0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U};
    m_block.fill(0U);
    m_byteLength = 0U;
    m_blockLength = 0U;
    m_finished = false;
}

bool Sha256::Update(const uint8_t* const bytes, const size_t length) {
    if (m_finished || (bytes == nullptr && length != 0U) ||
        length > UINT64_MAX - m_byteLength) {
        return false;
    }
    size_t offset = 0U;
    while (offset < length) {
        const size_t size = std::min(length - offset, m_block.size() - m_blockLength);
        std::copy_n(bytes + offset, size, m_block.data() + m_blockLength);
        m_blockLength += size;
        offset += size;
        if (m_blockLength == m_block.size()) {
            Transform();
            m_blockLength = 0U;
        }
    }
    m_byteLength += length;
    return true;
}

Sha256::Digest Sha256::Finish() {
    Digest digest{};
    if (m_finished || m_byteLength > UINT64_MAX / 8U) return digest;
    const uint64_t bitLength = m_byteLength * 8U;
    m_block[m_blockLength++] = 0x80U;
    if (m_blockLength > 56U) {
        std::fill(m_block.begin() + m_blockLength, m_block.end(), 0U);
        Transform();
        m_blockLength = 0U;
    }
    std::fill(m_block.begin() + m_blockLength, m_block.begin() + 56U, 0U);
    for (uint8_t index = 0U; index < 8U; ++index) {
        m_block[63U - index] = static_cast<uint8_t>(bitLength >> (index * 8U));
    }
    Transform();
    for (size_t word = 0U; word < m_state.size(); ++word) {
        for (uint8_t byte = 0U; byte < 4U; ++byte) {
            digest[word * 4U + byte] = static_cast<uint8_t>(
                m_state[word] >> (24U - byte * 8U));
        }
    }
    m_finished = true;
    return digest;
}

void Sha256::Transform() {
    std::array<uint32_t, 64U> words{};
    for (size_t index = 0U; index < 16U; ++index) {
        const size_t offset = index * 4U;
        words[index] = (static_cast<uint32_t>(m_block[offset]) << 24U) |
            (static_cast<uint32_t>(m_block[offset + 1U]) << 16U) |
            (static_cast<uint32_t>(m_block[offset + 2U]) << 8U) |
            static_cast<uint32_t>(m_block[offset + 3U]);
    }
    for (size_t index = 16U; index < words.size(); ++index) {
        const uint32_t s0 = RotateRight(words[index - 15U], 7U) ^
            RotateRight(words[index - 15U], 18U) ^
            (words[index - 15U] >> 3U);
        const uint32_t s1 = RotateRight(words[index - 2U], 17U) ^
            RotateRight(words[index - 2U], 19U) ^
            (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    uint32_t a = m_state[0U];
    uint32_t b = m_state[1U];
    uint32_t c = m_state[2U];
    uint32_t d = m_state[3U];
    uint32_t e = m_state[4U];
    uint32_t f = m_state[5U];
    uint32_t g = m_state[6U];
    uint32_t h = m_state[7U];
    for (size_t index = 0U; index < words.size(); ++index) {
        const uint32_t sum1 = RotateRight(e, 6U) ^ RotateRight(e, 11U) ^
            RotateRight(e, 25U);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temporary1 =
            h + sum1 + choice + ROUND_CONSTANTS[index] + words[index];
        const uint32_t sum0 = RotateRight(a, 2U) ^ RotateRight(a, 13U) ^
            RotateRight(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    m_state[0U] += a;
    m_state[1U] += b;
    m_state[2U] += c;
    m_state[3U] += d;
    m_state[4U] += e;
    m_state[5U] += f;
    m_state[6U] += g;
    m_state[7U] += h;
}

#endif

} // namespace modules
