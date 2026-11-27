#pragma once

#include <algorithm>
#include <array>
#include "ble_gap.h"

#define MYNEWT_VAL(name) MYNEWT_VAL_ ## name
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS 3
struct ble_store_value_rpa_rec { ble_addr_t peer_rpa_addr{}, peer_addr{}; };

namespace owner_native_fixture {
inline std::array<ble_addr_t, 6U> peers{};
inline int peerCount = 0;
inline int inventoryResult = 1;
inline unsigned inventoryReads = 0U;
inline unsigned snapshotReads = 0U;
inline std::array<ble_store_value_rpa_rec, 3U> rpas{};
inline int rpaCount = 0, snapshotResult = 0;
inline int metadataResult = 0;
inline bool otherMetadata = false;
inline void Reset() {
    connection = {}; connectionResult = 1; liveHandle = 7U;
    connectionLookups = 0U;
    peers = {}; peerCount = 0; inventoryResult = 1;
    inventoryReads = 0U;
    snapshotReads = 0U;
    rpas = {}; rpaCount = 0; snapshotResult = 0;
    metadataResult = 0; otherMetadata = false;
}
}
inline int LitchSdkReadPeerMetadata(int* present) {
    if (owner_native_fixture::metadataResult != 0) return owner_native_fixture::metadataResult;
    *present = owner_native_fixture::rpaCount != 0 || owner_native_fixture::otherMetadata;
    return 0;
}
inline int LitchSdkReadAddRpaSnapshot(ble_store_value_rpa_rec* out, int* count, int capacity) {
    ++owner_native_fixture::snapshotReads;
    if (owner_native_fixture::snapshotResult != 0) return owner_native_fixture::snapshotResult;
    if (owner_native_fixture::rpaCount < 0 || owner_native_fixture::rpaCount > capacity) return 6;
    std::copy_n(owner_native_fixture::rpas.begin(), owner_native_fixture::rpaCount, out);
    *count = owner_native_fixture::rpaCount;
    return 0;
}
inline int ble_store_util_bonded_peers(ble_addr_t* out, int* count, int capacity) {
    ++owner_native_fixture::inventoryReads;
    if (owner_native_fixture::inventoryResult != 0) return owner_native_fixture::inventoryResult;
    if (owner_native_fixture::peerCount < 0 || owner_native_fixture::peerCount > capacity) return 6;
    std::copy_n(owner_native_fixture::peers.begin(), owner_native_fixture::peerCount, out);
    *count = owner_native_fixture::peerCount;
    return 0;
}
