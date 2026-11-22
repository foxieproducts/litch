/* Included by the guarded config TU after its actual native table owners.
 * Caller owns quiescence; this worker alone may mutate native custody here.
 * The journal contains identities and RPA metadata only, never native keys. */
#include <stddef.h>

static int
litch_add_contains(const ble_addr_t *saved, int count, const ble_addr_t *peer)
{
    for (int i = 0; i < count; ++i) if (!ble_addr_cmp(&saved[i], peer)) return 1;
    return 0;
}

static void
litch_add_retain(void *records, int *count, size_t size, const ble_addr_t *saved, int saved_count)
{
    unsigned char *bytes = records;
    int retained = 0;
    for (int i = 0; i < *count; ++i) {
        /* Security, CCCD and CSFC all begin with their typed peer address. */
        if (!litch_add_contains(saved, saved_count, (const ble_addr_t *)(bytes + i * size))) continue;
        if (retained != i) memmove(bytes + retained * size, bytes + i * size, size);
        ++retained;
    }
    *count = retained;
}

int
LitchSdkReadAddRpaSnapshot(struct ble_store_value_rpa_rec *out, int *count, int capacity)
{
    extern int litch_nimble_store_restore_status;
    int rc = BLE_HS_ESTORE_FAIL;
    if (out == NULL || count == NULL || capacity < MYNEWT_VAL(BLE_STORE_MAX_BONDS)) return BLE_HS_EINVAL;
    ble_hs_lock();
    if (litch_nimble_store_restore_status == 0 && ble_store_config_num_rpa_recs >= 0 &&
        ble_store_config_num_rpa_recs <= MYNEWT_VAL(BLE_STORE_MAX_BONDS)) {
        memcpy(out, ble_store_config_rpa_recs, ble_store_config_num_rpa_recs * sizeof *out);
        *count = ble_store_config_num_rpa_recs;
        rc = 0;
    }
    ble_hs_unlock();
    return rc;
}

int
LitchSdkRollbackAddedPeers(const ble_addr_t *saved, int saved_count,
                          const struct ble_store_value_rpa_rec *rpas, int rpa_count)
{
    extern int litch_nimble_store_restore_status;
    int rc = BLE_HS_ESTORE_FAIL;
    if (saved == NULL || saved_count < 1 || saved_count >= MYNEWT_VAL(BLE_STORE_MAX_BONDS) ||
        rpa_count < 0 || rpa_count > MYNEWT_VAL(BLE_STORE_MAX_BONDS) || (rpa_count && rpas == NULL))
        return BLE_HS_EINVAL;
    for (int i = 0; i < saved_count; ++i) {
        if (saved[i].type > 1 || litch_add_contains(saved, i, &saved[i])) return BLE_HS_EINVAL;
    }
    for (int i = 0; i < rpa_count; ++i) {
        if (rpas[i].peer_rpa_addr.type > 3 || rpas[i].peer_addr.type > 3) return BLE_HS_EINVAL;
        for (int j = 0; j < i; ++j)
            if (!ble_addr_cmp(&rpas[i].peer_rpa_addr, &rpas[j].peer_rpa_addr)) return BLE_HS_EINVAL;
    }
    ble_hs_lock();
    if (litch_nimble_store_restore_status != 0 ||
        ble_store_config_num_our_secs < 0 || ble_store_config_num_our_secs > MYNEWT_VAL(BLE_STORE_MAX_BONDS) ||
        ble_store_config_num_peer_secs < 0 || ble_store_config_num_peer_secs > MYNEWT_VAL(BLE_STORE_MAX_BONDS) ||
        ble_store_config_num_cccds < 0 || ble_store_config_num_cccds > MYNEWT_VAL(BLE_STORE_MAX_CCCDS) ||
        ble_store_config_num_csfcs < 0 || ble_store_config_num_csfcs > MYNEWT_VAL(BLE_STORE_MAX_CSFCS)) goto done;
    /* Missing baseline custody is unavailable, not authority to erase or invent
     * a retained key. Check the complete native union before any mutation. */
    for (int i = 0; i < saved_count; ++i) {
        int found = 0;
        for (int j = 0; j < ble_store_config_num_our_secs; ++j)
            found |= !ble_addr_cmp(&saved[i], &ble_store_config_our_secs[j].peer_addr);
        for (int j = 0; j < ble_store_config_num_peer_secs; ++j)
            found |= !ble_addr_cmp(&saved[i], &ble_store_config_peer_secs[j].peer_addr);
        if (!found) goto done;
    }
    litch_add_retain(ble_store_config_our_secs, &ble_store_config_num_our_secs,
                     sizeof *ble_store_config_our_secs, saved, saved_count);
    litch_add_retain(ble_store_config_peer_secs, &ble_store_config_num_peer_secs,
                     sizeof *ble_store_config_peer_secs, saved, saved_count);
    litch_add_retain(ble_store_config_cccds, &ble_store_config_num_cccds,
                     sizeof *ble_store_config_cccds, saved, saved_count);
    litch_add_retain(ble_store_config_csfcs, &ble_store_config_num_csfcs,
                     sizeof *ble_store_config_csfcs, saved, saved_count);
    /* Restore every pre-existing RPA mapping, including unrelated orphan
     * metadata and same-key mappings changed by this attempt. No IRK is copied. */
    if (rpa_count) memcpy(ble_store_config_rpa_recs, rpas, rpa_count * sizeof *rpas);
    ble_store_config_num_rpa_recs = rpa_count;
    rc = ble_store_config_persist_our_secs(); if (rc != 0) goto done;
    rc = ble_store_config_persist_peer_secs(); if (rc != 0) goto done;
    rc = ble_store_config_persist_cccds(); if (rc != 0) goto done;
    rc = ble_store_config_persist_csfcs(); if (rc != 0) goto done;
    rc = ble_store_config_persist_rpa_recs();
done:
    ble_hs_unlock();
    return rc;
}

int
LitchSdkRestoreRetainedPeerPrivacy(void)
{
    int rc = LitchSdkResetPeerPrivacy();
    if (rc != 0) return rc;
    /* The SDK's restore_irks callback logs add-entry errors and returns zero.
     * Read each retained entry through the same owner and preserve every error. */
    for (int i = 0; i < MYNEWT_VAL(BLE_STORE_MAX_BONDS); ++i) {
        union ble_store_key key;
        union ble_store_value value;
        memset(&key, 0, sizeof key);
        key.sec.idx = i;
        ble_hs_lock();
        rc = ble_store_config_read(BLE_STORE_OBJ_TYPE_PEER_SEC, &key, &value);
        ble_hs_unlock();
        if (rc == BLE_HS_ENOENT) return 0;
        if (rc != 0) return rc;
        if (value.sec.irk_present) {
            rc = ble_hs_pvcy_add_entry(value.sec.peer_addr.val, value.sec.peer_addr.type, value.sec.irk);
            if (rc != 0) return rc;
        }
    }
    return 0;
}
