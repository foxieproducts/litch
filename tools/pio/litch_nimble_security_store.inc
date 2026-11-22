/* Private to the guarded NimBLE NVS translation unit; no new store or format.
 * SDK callbacks are installed before restore. Keep them closed until its
 * complete inventory succeeds; the config TU is the sole consumer. */
int litch_nimble_store_restore_status = BLE_HS_ESTORE_FAIL;
#if MYNEWT_VAL(BLE_STORE_MAX_BONDS) || MYNEWT_VAL(BLE_STORE_MAX_CCCDS) || MYNEWT_VAL(BLE_STORE_MAX_CSFCS)
#define LITCH_NATIVE_MAX_AUX (MYNEWT_VAL(BLE_STORE_MAX_CCCDS) > MYNEWT_VAL(BLE_STORE_MAX_CSFCS) \
    ? MYNEWT_VAL(BLE_STORE_MAX_CCCDS) : MYNEWT_VAL(BLE_STORE_MAX_CSFCS))
#define LITCH_NATIVE_MAX_RECORDS (MYNEWT_VAL(BLE_STORE_MAX_BONDS) > LITCH_NATIVE_MAX_AUX \
    ? MYNEWT_VAL(BLE_STORE_MAX_BONDS) : LITCH_NATIVE_MAX_AUX)

static bool
litch_native_identity_equal(int obj_type, const void *a, const void *b)
{
    switch (obj_type) {
    case BLE_STORE_OBJ_TYPE_PEER_ADDR:
        return !ble_addr_cmp(&((const struct ble_store_value_rpa_rec *)a)->peer_rpa_addr,
                             &((const struct ble_store_value_rpa_rec *)b)->peer_rpa_addr);
    case BLE_STORE_OBJ_TYPE_CCCD: {
        const struct ble_store_value_cccd *left = a, *right = b;
        return !ble_addr_cmp(&left->peer_addr, &right->peer_addr) &&
            left->chr_val_handle == right->chr_val_handle;
    }
    case BLE_STORE_OBJ_TYPE_CSFC:
        return !ble_addr_cmp(&((const struct ble_store_value_csfc *)a)->peer_addr,
                             &((const struct ble_store_value_csfc *)b)->peer_addr);
    default:
        return !ble_addr_cmp(&((const struct ble_store_value_sec *)a)->peer_addr,
                             &((const struct ble_store_value_sec *)b)->peer_addr);
    }
}

static int
litch_read_native_slot(int obj_type, int slot, void *value,
                       size_t expected, bool *present)
{
    nvs_handle_t handle;
    char key[NIMBLE_NVS_STR_NAME_MAX_LEN];
    size_t size = expected;
    esp_err_t err;
    *present = false;
    if (LitchSdkNvsBeginAccess() != 0) return BLE_HS_ESTORE_FAIL;
    err = nvs_open(NIMBLE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        LitchSdkNvsEndAccess();
        return err == ESP_ERR_NVS_NOT_FOUND ? 0 : BLE_HS_ESTORE_FAIL;
    }
    get_nvs_key_string(obj_type, slot, key);
    err = nvs_get_blob(handle, key, value, &size);
    nvs_close(handle);
    LitchSdkNvsEndAccess();
    if (err == ESP_ERR_NVS_NOT_FOUND) return 0;
    if (err != ESP_OK || size != expected) return BLE_HS_ESTORE_FAIL;
    *present = true;
    return 0;
}

static int
litch_persist_native(int obj_type, const void *values, int count,
                     size_t value_size, int capacity)
{
    /* Share one bounded table buffer without charging eight security records
     * to the stack merely because CCCD has the largest record count. */
    union {
#if MYNEWT_VAL(BLE_STORE_MAX_BONDS)
        struct ble_store_value_sec secs[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
#endif
#if MYNEWT_VAL(BLE_STORE_MAX_CCCDS)
        struct ble_store_value_cccd cccds[MYNEWT_VAL(BLE_STORE_MAX_CCCDS)];
#endif
#if MYNEWT_VAL(BLE_STORE_MAX_CSFCS)
        struct ble_store_value_csfc csfcs[MYNEWT_VAL(BLE_STORE_MAX_CSFCS)];
#endif
    } stored;
    union ble_store_value verified;
    uint8_t *stored_records = (uint8_t *)&stored;
    const uint8_t *records = values;
    bool present[LITCH_NATIVE_MAX_RECORDS] = {false};
    int desired_slot[LITCH_NATIVE_MAX_RECORDS] = {0};
    bool seen[LITCH_NATIVE_MAX_RECORDS] = {false};
    char key[NIMBLE_NVS_STR_NAME_MAX_LEN];
    int slot, i, j, rc;
    bool found;

    if (capacity < 0 || capacity > LITCH_NATIVE_MAX_RECORDS ||
        count < 0 || count > capacity || value_size > sizeof verified)
        return BLE_HS_ESTORE_FAIL;
    /* Read and validate the complete bounded inventory before changing it. */
    for (i = 0; i < count; ++i) {
        for (j = 0; j < i; ++j) {
            if (litch_native_identity_equal(obj_type, records + i * value_size,
                                             records + j * value_size))
                return BLE_HS_ESTORE_FAIL;
        }
    }
    for (slot = 0; slot < capacity; ++slot) {
        rc = litch_read_native_slot(obj_type, slot + 1, stored_records + slot * value_size,
                                    value_size, &present[slot]);
        if (rc != 0) return rc;
        if (!present[slot]) continue;
        for (j = 0; j < slot; ++j) {
            if (present[j] && litch_native_identity_equal(obj_type, stored_records + slot * value_size,
                                                          stored_records + j * value_size))
                return BLE_HS_ESTORE_FAIL;
        }
        for (i = 0; i < count; ++i) {
            if (litch_native_identity_equal(obj_type, stored_records + slot * value_size,
                                             records + i * value_size))
                desired_slot[i] = slot + 1;
        }
    }
    /* Keep retained identities in their existing slots, including RAM reorder. */
    for (slot = 0; slot < capacity; ++slot) {
        if (!present[slot]) continue;
        found = false;
        for (i = 0; i < count; ++i) found |= desired_slot[i] == slot + 1;
        if (found) continue;
        rc = ble_nvs_delete_value(obj_type, slot + 1);
        if (rc != 0) return rc;
        present[slot] = false;
    }
    for (i = 0; i < count; ++i) {
        slot = desired_slot[i] - 1;
        if (slot < 0) {
            for (slot = 0; slot < capacity; ++slot)
                if (!present[slot]) break;
            if (slot == capacity) return BLE_HS_ESTORE_CAP;
        } else if (!memcmp(stored_records + slot * value_size, records + i * value_size, value_size)) {
            continue;
        }
        get_nvs_key_string(obj_type, slot + 1, key);
        rc = ble_nvs_write_key_value(key, records + i * value_size, value_size);
        if (rc != 0) return rc;
        present[slot] = true;
    }
    /* Commit success plus a newly opened read-only inventory is required.
     * An error may already have changed flash; callers must keep it a failure. */
    for (slot = 0; slot < capacity; ++slot) {
        rc = litch_read_native_slot(obj_type, slot + 1, &verified, value_size, &found);
        if (rc != 0) return rc;
        if (!found) continue;
        for (i = 0; i < count; ++i) {
            if (!memcmp(&verified, records + i * value_size, value_size)) break;
        }
        if (i == count || seen[i]) return BLE_HS_ESTORE_FAIL;
        seen[i] = true;
    }
    for (i = 0; i < count; ++i) if (!seen[i]) return BLE_HS_ESTORE_FAIL;
    return 0;
}

#if MYNEWT_VAL(BLE_STORE_MAX_BONDS)
static int
litch_persist_security(int obj_type, const struct ble_store_value_sec *values,
                       int count)
{
    return litch_persist_native(obj_type, values, count, sizeof *values,
                                MYNEWT_VAL(BLE_STORE_MAX_BONDS));
}
#endif
#undef LITCH_NATIVE_MAX_RECORDS
#undef LITCH_NATIVE_MAX_AUX
#endif
