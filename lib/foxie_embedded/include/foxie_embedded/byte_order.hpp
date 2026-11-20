#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fxe {

// \brief Read unsigned 8-bit values (endianness is a no-op for 1 byte).
inline uint8_t ReadU8LE(const uint8_t* data) {
    return data[0];
}
inline uint8_t ReadU8BE(const uint8_t* data) {
    return data[0];
}

// \brief Read signed 8-bit values (endianness is a no-op for 1 byte).
inline int8_t ReadI8LE(const uint8_t* data) {
    return static_cast<int8_t>(data[0]);
}
inline int8_t ReadI8BE(const uint8_t* data) {
    return static_cast<int8_t>(data[0]);
}

// \brief Read unsigned 16-bit values.
inline uint16_t ReadU16LE(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

inline uint16_t ReadU16BE(const uint8_t* data) {
    return static_cast<uint16_t>(data[1]) | (static_cast<uint16_t>(data[0]) << 8);
}

// \brief Read signed 16-bit values.
inline int16_t ReadI16LE(const uint8_t* data) {
    return static_cast<int16_t>(ReadU16LE(data));
}
inline int16_t ReadI16BE(const uint8_t* data) {
    return static_cast<int16_t>(ReadU16BE(data));
}

// \brief Read unsigned 32-bit values.
inline uint32_t ReadU32LE(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

inline uint32_t ReadU32BE(const uint8_t* data) {
    return static_cast<uint32_t>(data[3]) | (static_cast<uint32_t>(data[2]) << 8) |
           (static_cast<uint32_t>(data[1]) << 16) | (static_cast<uint32_t>(data[0]) << 24);
}

// \brief Read signed 32-bit values.
inline int32_t ReadI32LE(const uint8_t* data) {
    return static_cast<int32_t>(ReadU32LE(data));
}
inline int32_t ReadI32BE(const uint8_t* data) {
    return static_cast<int32_t>(ReadU32BE(data));
}

// \brief Read unsigned 64-bit values.
inline uint64_t ReadU64LE(const uint8_t* data) {
    return static_cast<uint64_t>(data[0]) | (static_cast<uint64_t>(data[1]) << 8) |
           (static_cast<uint64_t>(data[2]) << 16) | (static_cast<uint64_t>(data[3]) << 24) |
           (static_cast<uint64_t>(data[4]) << 32) | (static_cast<uint64_t>(data[5]) << 40) |
           (static_cast<uint64_t>(data[6]) << 48) | (static_cast<uint64_t>(data[7]) << 56);
}

inline uint64_t ReadU64BE(const uint8_t* data) {
    return static_cast<uint64_t>(data[7]) | (static_cast<uint64_t>(data[6]) << 8) |
           (static_cast<uint64_t>(data[5]) << 16) | (static_cast<uint64_t>(data[4]) << 24) |
           (static_cast<uint64_t>(data[3]) << 32) | (static_cast<uint64_t>(data[2]) << 40) |
           (static_cast<uint64_t>(data[1]) << 48) | (static_cast<uint64_t>(data[0]) << 56);
}

// \brief Read signed 64-bit values.
inline int64_t ReadI64LE(const uint8_t* data) {
    return static_cast<int64_t>(ReadU64LE(data));
}
inline int64_t ReadI64BE(const uint8_t* data) {
    return static_cast<int64_t>(ReadU64BE(data));
}

// \brief Write unsigned 8-bit values (endianness is a no-op for 1 byte).
inline void WriteU8LE(const uint8_t value, uint8_t* out) {
    out[0] = value;
}
inline void WriteU8BE(const uint8_t value, uint8_t* out) {
    out[0] = value;
}

// \brief Write signed 8-bit values (endianness is a no-op for 1 byte).
inline void WriteI8LE(const int8_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>(value);
}
inline void WriteI8BE(const int8_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>(value);
}

// \brief Write unsigned 16-bit values.
inline void WriteU16LE(const uint16_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>(value & 0xFFU);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

inline void WriteU16BE(const uint16_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    out[1] = static_cast<uint8_t>(value & 0xFFU);
}

// \brief Write signed 16-bit values.
inline void WriteI16LE(const int16_t value, uint8_t* out) {
    WriteU16LE(static_cast<uint16_t>(value), out);
}

inline void WriteI16BE(const int16_t value, uint8_t* out) {
    WriteU16BE(static_cast<uint16_t>(value), out);
}

// \brief Write unsigned 32-bit values.
inline void WriteU32LE(const uint32_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>(value & 0xFFU);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

inline void WriteU32BE(const uint32_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 24) & 0xFFU);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xFFU);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    out[3] = static_cast<uint8_t>(value & 0xFFU);
}

// \brief Write signed 32-bit values.
inline void WriteI32LE(const int32_t value, uint8_t* out) {
    WriteU32LE(static_cast<uint32_t>(value), out);
}

inline void WriteI32BE(const int32_t value, uint8_t* out) {
    WriteU32BE(static_cast<uint32_t>(value), out);
}

// \brief Write unsigned 64-bit values.
inline void WriteU64LE(const uint64_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>(value & 0xFFU);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
    out[4] = static_cast<uint8_t>((value >> 32) & 0xFFU);
    out[5] = static_cast<uint8_t>((value >> 40) & 0xFFU);
    out[6] = static_cast<uint8_t>((value >> 48) & 0xFFU);
    out[7] = static_cast<uint8_t>((value >> 56) & 0xFFU);
}

inline void WriteU64BE(const uint64_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 56) & 0xFFU);
    out[1] = static_cast<uint8_t>((value >> 48) & 0xFFU);
    out[2] = static_cast<uint8_t>((value >> 40) & 0xFFU);
    out[3] = static_cast<uint8_t>((value >> 32) & 0xFFU);
    out[4] = static_cast<uint8_t>((value >> 24) & 0xFFU);
    out[5] = static_cast<uint8_t>((value >> 16) & 0xFFU);
    out[6] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    out[7] = static_cast<uint8_t>(value & 0xFFU);
}

// \brief Write signed 64-bit values.
inline void WriteI64LE(const int64_t value, uint8_t* out) {
    WriteU64LE(static_cast<uint64_t>(value), out);
}

inline void WriteI64BE(const int64_t value, uint8_t* out) {
    WriteU64BE(static_cast<uint64_t>(value), out);
}

// Array overloads (fixed-size buffers).
template <size_t N>
inline uint8_t ReadU8LE(const uint8_t (&data)[N]) {
    static_assert(N >= 1, "ReadU8LE requires at least 1 byte");
    return ReadU8LE(&data[0]);
}

template <size_t N>
inline uint8_t ReadU8BE(const uint8_t (&data)[N]) {
    static_assert(N >= 1, "ReadU8BE requires at least 1 byte");
    return ReadU8BE(&data[0]);
}

template <size_t N>
inline int8_t ReadI8LE(const uint8_t (&data)[N]) {
    static_assert(N >= 1, "ReadI8LE requires at least 1 byte");
    return ReadI8LE(&data[0]);
}

template <size_t N>
inline int8_t ReadI8BE(const uint8_t (&data)[N]) {
    static_assert(N >= 1, "ReadI8BE requires at least 1 byte");
    return ReadI8BE(&data[0]);
}

template <size_t N>
inline uint16_t ReadU16LE(const uint8_t (&data)[N]) {
    static_assert(N >= 2, "ReadU16LE requires at least 2 bytes");
    return ReadU16LE(&data[0]);
}

template <size_t N>
inline uint16_t ReadU16BE(const uint8_t (&data)[N]) {
    static_assert(N >= 2, "ReadU16BE requires at least 2 bytes");
    return ReadU16BE(&data[0]);
}

template <size_t N>
inline int16_t ReadI16LE(const uint8_t (&data)[N]) {
    static_assert(N >= 2, "ReadI16LE requires at least 2 bytes");
    return ReadI16LE(&data[0]);
}

template <size_t N>
inline int16_t ReadI16BE(const uint8_t (&data)[N]) {
    static_assert(N >= 2, "ReadI16BE requires at least 2 bytes");
    return ReadI16BE(&data[0]);
}

template <size_t N>
inline uint32_t ReadU32LE(const uint8_t (&data)[N]) {
    static_assert(N >= 4, "ReadU32LE requires at least 4 bytes");
    return ReadU32LE(&data[0]);
}

template <size_t N>
inline uint32_t ReadU32BE(const uint8_t (&data)[N]) {
    static_assert(N >= 4, "ReadU32BE requires at least 4 bytes");
    return ReadU32BE(&data[0]);
}

template <size_t N>
inline int32_t ReadI32LE(const uint8_t (&data)[N]) {
    static_assert(N >= 4, "ReadI32LE requires at least 4 bytes");
    return ReadI32LE(&data[0]);
}

template <size_t N>
inline int32_t ReadI32BE(const uint8_t (&data)[N]) {
    static_assert(N >= 4, "ReadI32BE requires at least 4 bytes");
    return ReadI32BE(&data[0]);
}

template <size_t N>
inline uint64_t ReadU64LE(const uint8_t (&data)[N]) {
    static_assert(N >= 8, "ReadU64LE requires at least 8 bytes");
    return ReadU64LE(&data[0]);
}

template <size_t N>
inline uint64_t ReadU64BE(const uint8_t (&data)[N]) {
    static_assert(N >= 8, "ReadU64BE requires at least 8 bytes");
    return ReadU64BE(&data[0]);
}

template <size_t N>
inline int64_t ReadI64LE(const uint8_t (&data)[N]) {
    static_assert(N >= 8, "ReadI64LE requires at least 8 bytes");
    return ReadI64LE(&data[0]);
}

template <size_t N>
inline int64_t ReadI64BE(const uint8_t (&data)[N]) {
    static_assert(N >= 8, "ReadI64BE requires at least 8 bytes");
    return ReadI64BE(&data[0]);
}

template <size_t N>
inline void WriteU8LE(const uint8_t value, uint8_t (&out)[N]) {
    static_assert(N >= 1, "WriteU8LE requires at least 1 byte");
    WriteU8LE(value, &out[0]);
}

template <size_t N>
inline void WriteU8BE(const uint8_t value, uint8_t (&out)[N]) {
    static_assert(N >= 1, "WriteU8BE requires at least 1 byte");
    WriteU8BE(value, &out[0]);
}

template <size_t N>
inline void WriteI8LE(const int8_t value, uint8_t (&out)[N]) {
    static_assert(N >= 1, "WriteI8LE requires at least 1 byte");
    WriteI8LE(value, &out[0]);
}

template <size_t N>
inline void WriteI8BE(const int8_t value, uint8_t (&out)[N]) {
    static_assert(N >= 1, "WriteI8BE requires at least 1 byte");
    WriteI8BE(value, &out[0]);
}

template <size_t N>
inline void WriteU16LE(const uint16_t value, uint8_t (&out)[N]) {
    static_assert(N >= 2, "WriteU16LE requires at least 2 bytes");
    WriteU16LE(value, &out[0]);
}

template <size_t N>
inline void WriteU16BE(const uint16_t value, uint8_t (&out)[N]) {
    static_assert(N >= 2, "WriteU16BE requires at least 2 bytes");
    WriteU16BE(value, &out[0]);
}

template <size_t N>
inline void WriteI16LE(const int16_t value, uint8_t (&out)[N]) {
    static_assert(N >= 2, "WriteI16LE requires at least 2 bytes");
    WriteI16LE(value, &out[0]);
}

template <size_t N>
inline void WriteI16BE(const int16_t value, uint8_t (&out)[N]) {
    static_assert(N >= 2, "WriteI16BE requires at least 2 bytes");
    WriteI16BE(value, &out[0]);
}

template <size_t N>
inline void WriteU32LE(const uint32_t value, uint8_t (&out)[N]) {
    static_assert(N >= 4, "WriteU32LE requires at least 4 bytes");
    WriteU32LE(value, &out[0]);
}

template <size_t N>
inline void WriteU32BE(const uint32_t value, uint8_t (&out)[N]) {
    static_assert(N >= 4, "WriteU32BE requires at least 4 bytes");
    WriteU32BE(value, &out[0]);
}

template <size_t N>
inline void WriteI32LE(const int32_t value, uint8_t (&out)[N]) {
    static_assert(N >= 4, "WriteI32LE requires at least 4 bytes");
    WriteI32LE(value, &out[0]);
}

template <size_t N>
inline void WriteI32BE(const int32_t value, uint8_t (&out)[N]) {
    static_assert(N >= 4, "WriteI32BE requires at least 4 bytes");
    WriteI32BE(value, &out[0]);
}

template <size_t N>
inline void WriteU64LE(const uint64_t value, uint8_t (&out)[N]) {
    static_assert(N >= 8, "WriteU64LE requires at least 8 bytes");
    WriteU64LE(value, &out[0]);
}

template <size_t N>
inline void WriteU64BE(const uint64_t value, uint8_t (&out)[N]) {
    static_assert(N >= 8, "WriteU64BE requires at least 8 bytes");
    WriteU64BE(value, &out[0]);
}

template <size_t N>
inline void WriteI64LE(const int64_t value, uint8_t (&out)[N]) {
    static_assert(N >= 8, "WriteI64LE requires at least 8 bytes");
    WriteI64LE(value, &out[0]);
}

template <size_t N>
inline void WriteI64BE(const int64_t value, uint8_t (&out)[N]) {
    static_assert(N >= 8, "WriteI64BE requires at least 8 bytes");
    WriteI64BE(value, &out[0]);
}

template <size_t N>
inline uint8_t ReadU8LE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 1, "ReadU8LE requires at least 1 byte");
    return ReadU8LE(data.data());
}

template <size_t N>
inline uint8_t ReadU8BE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 1, "ReadU8BE requires at least 1 byte");
    return ReadU8BE(data.data());
}

template <size_t N>
inline int8_t ReadI8LE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 1, "ReadI8LE requires at least 1 byte");
    return ReadI8LE(data.data());
}

template <size_t N>
inline int8_t ReadI8BE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 1, "ReadI8BE requires at least 1 byte");
    return ReadI8BE(data.data());
}

template <size_t N>
inline uint16_t ReadU16LE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 2, "ReadU16LE requires at least 2 bytes");
    return ReadU16LE(data.data());
}

template <size_t N>
inline uint16_t ReadU16BE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 2, "ReadU16BE requires at least 2 bytes");
    return ReadU16BE(data.data());
}

template <size_t N>
inline int16_t ReadI16LE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 2, "ReadI16LE requires at least 2 bytes");
    return ReadI16LE(data.data());
}

template <size_t N>
inline int16_t ReadI16BE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 2, "ReadI16BE requires at least 2 bytes");
    return ReadI16BE(data.data());
}

template <size_t N>
inline uint32_t ReadU32LE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 4, "ReadU32LE requires at least 4 bytes");
    return ReadU32LE(data.data());
}

template <size_t N>
inline uint32_t ReadU32BE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 4, "ReadU32BE requires at least 4 bytes");
    return ReadU32BE(data.data());
}

template <size_t N>
inline int32_t ReadI32LE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 4, "ReadI32LE requires at least 4 bytes");
    return ReadI32LE(data.data());
}

template <size_t N>
inline int32_t ReadI32BE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 4, "ReadI32BE requires at least 4 bytes");
    return ReadI32BE(data.data());
}

template <size_t N>
inline uint64_t ReadU64LE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 8, "ReadU64LE requires at least 8 bytes");
    return ReadU64LE(data.data());
}

template <size_t N>
inline uint64_t ReadU64BE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 8, "ReadU64BE requires at least 8 bytes");
    return ReadU64BE(data.data());
}

template <size_t N>
inline int64_t ReadI64LE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 8, "ReadI64LE requires at least 8 bytes");
    return ReadI64LE(data.data());
}

template <size_t N>
inline int64_t ReadI64BE(const std::array<uint8_t, N>& data) {
    static_assert(N >= 8, "ReadI64BE requires at least 8 bytes");
    return ReadI64BE(data.data());
}

template <size_t N>
inline void WriteU8LE(const uint8_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 1, "WriteU8LE requires at least 1 byte");
    WriteU8LE(value, out.data());
}

template <size_t N>
inline void WriteU8BE(const uint8_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 1, "WriteU8BE requires at least 1 byte");
    WriteU8BE(value, out.data());
}

template <size_t N>
inline void WriteI8LE(const int8_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 1, "WriteI8LE requires at least 1 byte");
    WriteI8LE(value, out.data());
}

template <size_t N>
inline void WriteI8BE(const int8_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 1, "WriteI8BE requires at least 1 byte");
    WriteI8BE(value, out.data());
}

template <size_t N>
inline void WriteU16LE(const uint16_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 2, "WriteU16LE requires at least 2 bytes");
    WriteU16LE(value, out.data());
}

template <size_t N>
inline void WriteU16BE(const uint16_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 2, "WriteU16BE requires at least 2 bytes");
    WriteU16BE(value, out.data());
}

template <size_t N>
inline void WriteI16LE(const int16_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 2, "WriteI16LE requires at least 2 bytes");
    WriteI16LE(value, out.data());
}

template <size_t N>
inline void WriteI16BE(const int16_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 2, "WriteI16BE requires at least 2 bytes");
    WriteI16BE(value, out.data());
}

template <size_t N>
inline void WriteU32LE(const uint32_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 4, "WriteU32LE requires at least 4 bytes");
    WriteU32LE(value, out.data());
}

template <size_t N>
inline void WriteU32BE(const uint32_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 4, "WriteU32BE requires at least 4 bytes");
    WriteU32BE(value, out.data());
}

template <size_t N>
inline void WriteI32LE(const int32_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 4, "WriteI32LE requires at least 4 bytes");
    WriteI32LE(value, out.data());
}

template <size_t N>
inline void WriteI32BE(const int32_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 4, "WriteI32BE requires at least 4 bytes");
    WriteI32BE(value, out.data());
}

template <size_t N>
inline void WriteU64LE(const uint64_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 8, "WriteU64LE requires at least 8 bytes");
    WriteU64LE(value, out.data());
}

template <size_t N>
inline void WriteU64BE(const uint64_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 8, "WriteU64BE requires at least 8 bytes");
    WriteU64BE(value, out.data());
}

template <size_t N>
inline void WriteI64LE(const int64_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 8, "WriteI64LE requires at least 8 bytes");
    WriteI64LE(value, out.data());
}

template <size_t N>
inline void WriteI64BE(const int64_t value, std::array<uint8_t, N>& out) {
    static_assert(N >= 8, "WriteI64BE requires at least 8 bytes");
    WriteI64BE(value, out.data());
}

} // namespace fxe
