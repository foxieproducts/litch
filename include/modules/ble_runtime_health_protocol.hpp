#pragma once

#include <cstddef>
#include <cstdint>

#include "runtime/runtime_health.hpp"

namespace modules::ble_runtime_health_protocol {

inline constexpr char HEALTH_REQUEST[] = "{\"cmd\":\"health\"}";
inline constexpr size_t HEALTH_REQUEST_BYTES = sizeof(HEALTH_REQUEST) - 1U;
inline constexpr char HEALTH_RECHECK_REQUEST[] =
    "{\"cmd\":\"health_chk\"}";
inline constexpr size_t HEALTH_RECHECK_REQUEST_BYTES =
    sizeof(HEALTH_RECHECK_REQUEST) - 1U;
inline constexpr size_t HEALTH_RESPONSE_BYTES = 17U;
inline constexpr char HEALTH_RECHECK_SUCCEEDED[] = "{\"c\":\"ok\"}";
inline constexpr char HEALTH_RECHECK_FAILED[] = "{\"c\":\"fail\"}";
static_assert(HEALTH_RECHECK_REQUEST_BYTES == 20U,
              "health recheck must fit the minimum ATT payload exactly");
static_assert(sizeof(HEALTH_RECHECK_SUCCEEDED) - 1U <= 20U);
static_assert(sizeof(HEALTH_RECHECK_FAILED) - 1U <= 20U);

constexpr bool IsHealthRequest(const char* const data, const size_t len) {
    if (data == nullptr || len != HEALTH_REQUEST_BYTES) {
        return false;
    }

    for (size_t index = 0U; index < HEALTH_REQUEST_BYTES; ++index) {
        if (data[index] != HEALTH_REQUEST[index]) {
            return false;
        }
    }
    return true;
}

constexpr bool IsHealthRecheckRequest(const char* const data,
                                      const size_t len) {
    if (data == nullptr || len != HEALTH_RECHECK_REQUEST_BYTES) {
        return false;
    }

    for (size_t index = 0U; index < HEALTH_RECHECK_REQUEST_BYTES; ++index) {
        if (data[index] != HEALTH_RECHECK_REQUEST[index]) {
            return false;
        }
    }
    return true;
}

constexpr char HexDigit(const uint8_t value) {
    return value < 10U ? static_cast<char>('0' + value)
                       : static_cast<char>('A' + (value - 10U));
}

inline bool FormatHealthResponse(const runtime::RuntimeHealthSnapshot& snapshot,
                                 char* const out,
                                 const size_t outCapacity) {
    if (out == nullptr || outCapacity < HEALTH_RESPONSE_BYTES + 1U) {
        return false;
    }

    const uint16_t revision = snapshot.revision;
    const uint16_t causes = snapshot.causes;
    out[0] = '{';
    out[1] = '"';
    out[2] = 'h';
    out[3] = '"';
    out[4] = ':';
    out[5] = '"';
    out[6] = HexDigit(static_cast<uint8_t>(revision >> 12U));
    out[7] = HexDigit(static_cast<uint8_t>((revision >> 8U) & 0x0FU));
    out[8] = HexDigit(static_cast<uint8_t>((revision >> 4U) & 0x0FU));
    out[9] = HexDigit(static_cast<uint8_t>(revision & 0x0FU));
    out[10] = static_cast<char>('0' + static_cast<uint8_t>(snapshot.state));
    out[11] = HexDigit(static_cast<uint8_t>(causes >> 12U));
    out[12] = HexDigit(static_cast<uint8_t>((causes >> 8U) & 0x0FU));
    out[13] = HexDigit(static_cast<uint8_t>((causes >> 4U) & 0x0FU));
    out[14] = HexDigit(static_cast<uint8_t>(causes & 0x0FU));
    out[15] = '"';
    out[16] = '}';
    out[17] = '\0';
    return true;
}

} // namespace modules::ble_runtime_health_protocol
