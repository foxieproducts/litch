#include "modules/ble_service.hpp"
#include "modules/ble_service_internal.hpp"
#include "hardware/rmt_flash_arbiter.hpp"

#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#if (defined(ARDUINO) && defined(LITCH_ESP32S3)) || defined(LITCH_OWNER_NATIVE_TEST)
#include <cstring>
#include <nimble/nimble/host/include/host/ble_gap.h>
#include <nimble/nimble/host/include/host/ble_store.h>
#endif
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nimble/nimble/host/include/host/ble_hs.h>
#include "firmware/board_owner_persistence_runtime.hpp"
extern "C" int LitchSdkReconcileEmptyPeerStore(void);
extern "C" int LitchSdkClearPeerStore(void);
extern "C" int LitchSdkResetPeerPrivacy(void);
extern "C" int LitchSdkReadAddRpaSnapshot(ble_store_value_rpa_rec*, int*, int);
extern "C" int LitchSdkReadPeerMetadata(int*);
extern "C" int LitchSdkRollbackAddedPeers(const ble_addr_t*, int, const ble_store_value_rpa_rec*, int);
extern "C" int LitchSdkRestoreRetainedPeerPrivacy(void);
// These pinned native leaves use the host lock and fixed SDK connection list.
// The SDK exposes the callback type publicly but omits its function declaration.
extern "C" void ble_gap_conn_foreach_handle(ble_gap_conn_foreach_handle_fn*, void*);
extern "C" void ble_hs_lock(void);
extern "C" void ble_hs_unlock(void);

extern "C" int LitchSdkNvsBeginAccess(void) {
    auto* service = modules::ble_internal::g_bleService;
    return service != nullptr && service->BeginNativeStoreFlashAccess()
        ? 0 : BLE_HS_ESTORE_FAIL;
}

extern "C" void LitchSdkNvsEndAccess(void) {
    modules::ble_internal::g_bleService->EndNativeStoreFlashAccess();
}

extern "C" int LitchSdkPairingAdmission(uint16_t connHandle) {
    const auto* service = modules::ble_internal::g_bleService;
    return !modules::OWNER_PAIRING_ENABLED || (service != nullptr &&
        service->NativePairingAllowed(connHandle)) ? 0 : BLE_HS_EAUTHEN;
}

extern "C" uint32_t LitchSdkPairingAddRpaToken(uint16_t connHandle) {
    const auto* service = modules::ble_internal::g_bleService;
    return service != nullptr ? service->NativeAddRpaToken(connHandle) : 0U;
}
#endif

namespace modules {
namespace {
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
struct NativeLinks {
    std::array<uint16_t, MYNEWT_VAL(BLE_MAX_CONNECTIONS)> handles{};
    size_t count = 0U;
    bool complete = true;
};
NativeLinks ReadNativeLinks() {
    NativeLinks result{};
    ble_hs_lock();
    ble_gap_conn_foreach_handle([](uint16_t handle, void* context) {
        auto& links = *static_cast<NativeLinks*>(context);
        if (links.count == links.handles.size()) { links.complete = false; return 1; }
        links.handles[links.count++] = handle;
        return 0;
    }, &result);
    ble_hs_unlock();
    return result;
}
int OwnerGapEvent(ble_gap_event* event, void* context) {
    if (event != nullptr && event->type == BLE_GAP_EVENT_ENC_CHANGE)
        static_cast<BleService*>(context)->OnNativeEncryptionChange(
            event->enc_change.conn_handle, event->enc_change.status);
    if (event != nullptr && event->type == BLE_GAP_EVENT_DATA_LEN_CHG)
        static_cast<BleService*>(context)->OnNativeDataLengthChange(
            event->data_len_chg.conn_handle,
            {event->data_len_chg.max_tx_octets, event->data_len_chg.max_tx_time,
             event->data_len_chg.max_rx_octets, event->data_len_chg.max_rx_time});
    return 0;
}
#endif
#if (defined(ARDUINO) && defined(LITCH_ESP32S3)) || defined(LITCH_OWNER_NATIVE_TEST)
BoardOwnerPairingController::BondInventory ReadInventory(ble_addr_t* peers, int& count) {
    count = 0;
    // Pinned guarded SDK owner returns the deduplicated OUR_SEC/PEER_SEC union
    // and preserves errors. NimBLE's public count wrapper loses those errors.
    const int rc = ble_store_util_bonded_peers(peers, &count,
        2 * MYNEWT_VAL(BLE_STORE_MAX_BONDS));
    if (rc != 0 || count < 0 || count > 2 * MYNEWT_VAL(BLE_STORE_MAX_BONDS)) return {};
    BoardOwnerPairingController::BondInventory result{true, static_cast<uint8_t>(count)};
    int metadata = 0;
    if (LitchSdkReadPeerMetadata(&metadata) != 0) return {};
    result.remoteMetadataPresent = metadata != 0;
    if (count <= MAX_SAVED_PHONES) {
        result.identities.count = static_cast<uint8_t>(count);
        for (int i = 0; i < count; ++i) {
            if (peers[i].type > BLE_ADDR_RANDOM_ID) return {};
            auto& identity = result.identities.peers[i];
            identity.type = peers[i].type & 1U;
            std::copy_n(peers[i].val, identity.address.size(), identity.address.begin());
        }
        if (count != 0 && !result.identities.Valid()) return {};
    }
    return result;
}

void ReadNativePeerEvidence(BleOwnerSecurityEvent& event) {
    ble_addr_t peers[2 * MYNEWT_VAL(BLE_STORE_MAX_BONDS)]{};
    int count = 0;
    const auto inventory = ReadInventory(peers, count);
    event.inventoryAvailable = inventory.available;
    event.peerCount = inventory.count;
    event.savedPeers = inventory.identities;
    ble_gap_conn_desc connection{};
    if (ble_gap_conn_find(event.connHandle, &connection) != 0) return;
    event.peerIdentityAvailable = true;
    event.encrypted = connection.sec_state.encrypted != 0U;
    event.bonded = connection.sec_state.bonded != 0U;
    event.peerAddressType = connection.peer_id_addr.type;
    std::memcpy(event.peerAddress, connection.peer_id_addr.val, sizeof(event.peerAddress));
    // NimBLE can return the unresolved private OTA address as peer_id_addr.
    // A distinct identity is evidence of native resolution, not a mismatch.
    event.peerIdentityResolved = connection.peer_id_addr.type == BLE_ADDR_PUBLIC_ID ||
        connection.peer_id_addr.type == BLE_ADDR_RANDOM_ID ||
        connection.peer_id_addr.type != connection.peer_ota_addr.type ||
        std::memcmp(connection.peer_id_addr.val, connection.peer_ota_addr.val,
                    sizeof(event.peerAddress)) != 0;
    BoardOwnerPeer peer{};
    peer.type = connection.peer_id_addr.type & 1U;
    std::copy_n(connection.peer_id_addr.val, peer.address.size(), peer.address.begin());
    event.peerMatches = inventory.available && connection.peer_id_addr.type <= BLE_ADDR_RANDOM_ID &&
        inventory.identities.Contains(peer);
}
#endif
}

bool BleService::BeginNativeStoreFlashAccess() {
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    if (m_nativeStoreFlashArbiter == nullptr) return false;
    auto& arbiter = *m_nativeStoreFlashArbiter;
    constexpr uint32_t FLASH_ACQUIRE_TIMEOUT_MS = 100U;
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(FLASH_ACQUIRE_TIMEOUT_MS);
    arbiter.RequestFlash();
    while (!arbiter.TryBeginRequestedFlash()) {
        if (static_cast<TickType_t>(xTaskGetTickCount() - started) >= timeout) {
            arbiter.CancelFlashRequest();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    return true;
#else
    return false;
#endif
}

void BleService::EndNativeStoreFlashAccess() {
    m_nativeStoreFlashArbiter->EndFlash();
}

bool BleService::InitOwnerPairingNative() {
    if (!OwnerPairingEnabled()) return true;
    RevokeOwnerAdmission();
    m_connectionSession.SetProductAccess(false);
    m_bleSettings.SetAccessPolicy([](void* context, uint16_t handle, bool bootstrap) {
        return static_cast<BleService*>(context)->OwnerCallbackAllowed(handle, bootstrap);
    }, this);
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    ble_hs_cfg.sm_sc = 1U;
    ble_hs_cfg.sm_sc_only = 1U;
    if (!NimBLEDevice::setCustomGapHandler(&OwnerGapEvent, this)) return false;
    if (!m_ownerBooted && m_ownerRuntime != nullptr) {
        ble_addr_t peers[2 * MYNEWT_VAL(BLE_STORE_MAX_BONDS)]{};
        int count = 0;
        const auto inventory = ReadInventory(peers, count);
        const auto record = m_ownerPersistence == nullptr ? BoardOwnerRecord::Invalid :
            m_ownerPersistence->DurableOwnerRecord();
        m_ownerRuntime->Boot(record, inventory, m_ownerIdentity != nullptr && m_ownerIdentity->IsDurable());
        m_ownerBooted = inventory.available;
    }
#endif
    return true;
}

bool BleService::ReadIdentifyNativePeer(ble_identify::PeerIdentity& peer) const {
#if (defined(ARDUINO) && defined(LITCH_ESP32S3)) || defined(LITCH_OWNER_NATIVE_TEST)
    ble_gap_conn_desc connection{};
    const auto handle = m_ownerNativeHandle.load(std::memory_order_acquire);
    if (handle == ble_internal::INVALID_CONN_HANDLE || ble_gap_conn_find(handle, &connection) != 0 ||
        connection.peer_id_addr.type > BLE_ADDR_RANDOM_ID) return false;
    // The SDK supplies a resolved identity when available, otherwise its typed
    // native address. An unresolved RPA can change across links; never replace
    // native identity with app-provided bytes or claim RPA persistence.
    peer.type = connection.peer_id_addr.type & 1U;
    std::copy_n(connection.peer_id_addr.val, peer.address.size(), peer.address.begin());
    peer.available = true;
    return true;
#else
    (void)peer;
    return false;
#endif
}

bool BleService::StartOwnerNativeSecurity(uint16_t connHandle, int* nativeResult) {
#if defined(ARDUINO) || defined(LITCH_OWNER_NATIVE_TEST)
    return NimBLEDevice::startSecurity(connHandle, nativeResult);
#else
    (void)connHandle;
    if (nativeResult != nullptr) *nativeResult = 0;
    return true;
#endif
}

BleService::OwnerStoreDiagnostics BleService::ReadOwnerStoreDiagnostics() const {
#if (defined(ARDUINO) && defined(LITCH_ESP32S3)) || defined(LITCH_OWNER_NATIVE_TEST)
    if (!m_initialized || !OwnerPairingEnabled() || !m_ownerBooted) return {};
    ble_addr_t peers[2 * MYNEWT_VAL(BLE_STORE_MAX_BONDS)]{};
    int peerCount = 0;
    const auto inventory = ReadInventory(peers, peerCount);
    if (!inventory.available || inventory.count > MAX_SAVED_PHONES) return {};
    ble_store_value_rpa_rec rpas[MAX_SAVED_PHONES]{};
    int rpaCount = 0;
    if (LitchSdkReadAddRpaSnapshot(rpas, &rpaCount, MAX_SAVED_PHONES) != 0 ||
        rpaCount < 0 || rpaCount > MAX_SAVED_PHONES) return {};
    uint8_t orphanRpas = 0U;
    for (int i = 0; i < rpaCount; ++i) {
        if (rpas[i].peer_addr.type > BLE_ADDR_RANDOM_ID ||
            rpas[i].peer_rpa_addr.type > BLE_ADDR_RANDOM_ID) return {};
        BoardOwnerPeer identity{};
        identity.type = rpas[i].peer_addr.type & 1U;
        std::copy_n(rpas[i].peer_addr.val, identity.address.size(), identity.address.begin());
        if (!inventory.identities.Contains(identity)) ++orphanRpas;
    }
    return {true, inventory.count, static_cast<uint8_t>(rpaCount), orphanRpas};
#else
    return {};
#endif
}

bool BleService::OwnerNativePairingCandidate(uint16_t handle) const {
#if (defined(ARDUINO) && defined(LITCH_ESP32S3)) || defined(LITCH_OWNER_NATIVE_TEST)
    BleOwnerSecurityEvent evidence{};
    evidence.connHandle = handle;
    ReadNativePeerEvidence(evidence);
    // SMP rechecks after identity exchange, before either key table can write.
    // An unresolved old phone must not replace its saved key under fresh consent.
    if (!evidence.inventoryAvailable || !evidence.peerIdentityAvailable ||
        evidence.peerCount >= MAX_SAVED_PHONES || evidence.peerMatches) return false;
    const bool identityKnown = evidence.peerIdentityResolved || evidence.peerAddressType == BLE_ADDR_PUBLIC ||
        (evidence.peerAddressType == BLE_ADDR_RANDOM && (evidence.peerAddress[5U] & 0xc0U) == 0xc0U);
    if (!identityKnown) return true;
    ble_gap_conn_desc connection{};
    ble_store_value_rpa_rec rpas[MAX_SAVED_PHONES]{};
    int rpaCount = 0;
    if (ble_gap_conn_find(handle, &connection) != 0 ||
        LitchSdkReadAddRpaSnapshot(rpas, &rpaCount, MAX_SAVED_PHONES) != 0 ||
        rpaCount < 0 || rpaCount > MAX_SAVED_PHONES) return false;
    for (int i = 0; i < rpaCount; ++i) {
        const auto& record = rpas[i];
        if ((record.peer_rpa_addr.type & 1U) == (connection.peer_ota_addr.type & 1U) &&
            std::memcmp(record.peer_rpa_addr.val, connection.peer_ota_addr.val, 6U) == 0 &&
            ((record.peer_addr.type & 1U) != (connection.peer_id_addr.type & 1U) ||
             std::memcmp(record.peer_addr.val, connection.peer_id_addr.val, 6U) != 0)) return false;
    }
    return true;
#else
    (void)handle;
    return false;
#endif
}

bool BleService::OwnerNativeLinksQuiescent() const {
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    const auto links = ReadNativeLinks();
    return links.complete && links.count == 0U;
#else
    return m_server != nullptr && m_server->getConnectedCount() == 0U;
#endif
}

bool BleService::OwnerNativeGapQuiescent() const {
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    return ble_gap_adv_active() == 0 && ble_gap_disc_active() == 0;
#else
    return m_advertising == nullptr || !m_advertising->isAdvertising();
#endif
}

bool BleService::QuiesceOwnerNativeGap() {
    if (!StopAdvertising()) return false;
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    if (ble_gap_disc_active()) {
        const int rc = ble_gap_disc_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY) return false;
    }
#endif
    return OwnerNativeGapQuiescent();
}

void BleService::DisconnectOwnerNativeLinks() {
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    const auto links = ReadNativeLinks();
    for (size_t index = 0U; index < links.count; ++index)
        (void)ble_gap_terminate(links.handles[index], BLE_ERR_REM_USER_CONN_TERM);
#else
    if (m_server != nullptr && m_ownerNativeHandle.load(std::memory_order_acquire) != ble_internal::INVALID_CONN_HANDLE)
        m_server->disconnect(m_ownerNativeHandle.load(std::memory_order_acquire));
#endif
}

void BleService::ObserveOwnerNativePeer(uint16_t handle) {
#if (defined(ARDUINO) && defined(LITCH_ESP32S3)) || defined(LITCH_OWNER_NATIVE_TEST)
    if (!OwnerPairingEnabled() || handle != m_ownerNativeHandle.load(std::memory_order_acquire)) return;
    BleOwnerSecurityEvent event{};
    event.kind = BleOwnerSecurityEvent::Kind::Identity;
    event.connHandle = handle;
    event.connectionEpoch = m_ownerNativeEpoch.load(std::memory_order_acquire);
    ReadNativePeerEvidence(event);
    OnOwnerSecurity(event);
#else
    (void)handle;
#endif
}

void BleService::OnNativeEncryptionChange(uint16_t handle, int32_t nativeStatus) {
#if (defined(ARDUINO) && defined(LITCH_ESP32S3)) || defined(LITCH_OWNER_NATIVE_TEST)
    if (!OwnerPairingEnabled() || handle != m_ownerNativeHandle.load(std::memory_order_acquire)) return;
    BleOwnerSecurityEvent event{};
    event.connHandle = handle;
    event.connectionEpoch = m_ownerNativeEpoch.load(std::memory_order_acquire);
    event.admissionToken = m_ownerAdmissionToken.load(std::memory_order_acquire);
    event.sessionToken = m_ownerAdmissionSession.load(std::memory_order_acquire);
    event.nativeStatus = nativeStatus;
    ReadNativePeerEvidence(event);
    OnOwnerSecurity(event);
#else
    (void)handle; (void)nativeStatus;
#endif
}

bool BleService::ClearOwnerNativeStore(void* context) {
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    auto& self = *static_cast<BleService*>(context);
    if (!self.m_ownerQuiescing.load(std::memory_order_acquire) ||
        self.m_ownerNativeHandle.load(std::memory_order_acquire) != ble_internal::INVALID_CONN_HANDLE ||
        !self.OwnerNativeLinksQuiescent() || !self.OwnerNativeGapQuiescent()) return false;
    // Each native NVS leaf owns flash/RMT exclusion. Store operations hold the
    // host lock; the SDK privacy reset must run outside it. Main-loop
    // GAP starts/stops stay excluded until this worker's terminal outcome.
    if (LitchSdkClearPeerStore() != 0 || LitchSdkReconcileEmptyPeerStore() != 0) return false;
    return LitchSdkResetPeerPrivacy() == 0;
#else
    (void)context;
    return false;
#endif
}

bool BleService::ReadOwnerAddBaseline(BoardOwnerAddBaseline& baseline) const {
#if (defined(ARDUINO) && defined(LITCH_ESP32S3)) || defined(LITCH_OWNER_NATIVE_TEST)
    ble_addr_t peers[2 * MYNEWT_VAL(BLE_STORE_MAX_BONDS)]{};
    int count = 0;
    const auto inventory = ReadInventory(peers, count);
    if (!inventory.available || count < 1 || count >= MAX_SAVED_PHONES) return false;
    ble_store_value_rpa_rec rpas[MAX_SAVED_PHONES]{};
    int rpaCount = 0;
    if (LitchSdkReadAddRpaSnapshot(rpas, &rpaCount, MAX_SAVED_PHONES) != 0 ||
        rpaCount < 0 || rpaCount > MAX_SAVED_PHONES) return false;
    BoardOwnerAddBaseline snapshot{};
    snapshot.saved = inventory.identities;
    snapshot.rpaCount = static_cast<uint8_t>(rpaCount);
    for (int i = 0; i < rpaCount; ++i) {
        snapshot.rpas[i].rpa.type = rpas[i].peer_rpa_addr.type;
        snapshot.rpas[i].identity.type = rpas[i].peer_addr.type;
        std::copy_n(rpas[i].peer_rpa_addr.val, 6U, snapshot.rpas[i].rpa.address.begin());
        std::copy_n(rpas[i].peer_addr.val, 6U, snapshot.rpas[i].identity.address.begin());
    }
    if (!snapshot.Valid()) return false;
    baseline = snapshot;
    return true;
#else
    (void)baseline;
    return false;
#endif
}

bool BleService::RollbackOwnerNativeStore(void* context, const BoardOwnerAddBaseline& baseline) {
#if defined(ARDUINO) && defined(LITCH_ESP32S3)
    auto& self = *static_cast<BleService*>(context);
    if (!baseline.Valid() || !self.m_ownerQuiescing.load(std::memory_order_acquire) ||
        self.m_ownerNativeHandle.load(std::memory_order_acquire) != ble_internal::INVALID_CONN_HANDLE ||
        !self.OwnerNativeLinksQuiescent() || !self.OwnerNativeGapQuiescent()) return false;
    ble_addr_t peers[MAX_SAVED_PHONES]{};
    ble_store_value_rpa_rec rpas[MAX_SAVED_PHONES]{};
    for (uint8_t i = 0U; i < baseline.saved.count; ++i) {
        peers[i].type = baseline.saved.peers[i].type;
        std::copy_n(baseline.saved.peers[i].address.begin(), 6U, peers[i].val);
    }
    for (uint8_t i = 0U; i < baseline.rpaCount; ++i) {
        rpas[i].peer_rpa_addr.type = baseline.rpas[i].rpa.type;
        rpas[i].peer_addr.type = baseline.rpas[i].identity.type;
        std::copy_n(baseline.rpas[i].rpa.address.begin(), 6U, rpas[i].peer_rpa_addr.val);
        std::copy_n(baseline.rpas[i].identity.address.begin(), 6U, rpas[i].peer_addr.val);
    }
    return LitchSdkRollbackAddedPeers(peers, baseline.saved.count, rpas, baseline.rpaCount) == 0 &&
        LitchSdkRestoreRetainedPeerPrivacy() == 0;
#else
    (void)context; (void)baseline;
    return false;
#endif
}
} // namespace modules
#endif
