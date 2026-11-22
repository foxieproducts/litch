#pragma once

#include <cstdint>

inline constexpr uint8_t BLE_ADDR_PUBLIC = 0U;
inline constexpr uint8_t BLE_ADDR_RANDOM = 1U;
inline constexpr uint8_t BLE_ADDR_PUBLIC_ID = 2U;
inline constexpr uint8_t BLE_ADDR_RANDOM_ID = 3U;
struct ble_addr_t { uint8_t type = 0U; uint8_t val[6U]{}; };
struct ble_gap_conn_desc {
    struct { uint8_t encrypted = 0U; uint8_t bonded = 0U; } sec_state;
    ble_addr_t peer_id_addr{};
    ble_addr_t peer_ota_addr{};
};

namespace owner_native_fixture {
inline ble_gap_conn_desc connection{};
inline int connectionResult = 1;
inline uint16_t liveHandle = 7U;
inline unsigned connectionLookups = 0U;
}
inline int ble_gap_conn_find(uint16_t handle, ble_gap_conn_desc* out) {
    ++owner_native_fixture::connectionLookups;
    if (owner_native_fixture::connectionResult != 0 || handle != owner_native_fixture::liveHandle)
        return 1;
    *out = owner_native_fixture::connection;
    return 0;
}
