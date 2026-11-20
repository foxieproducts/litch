"""Compile a pinned NimBLE copy that never silently replaces an existing bond.

The dependency remains unchanged. The S3 middleware replaces only its server
translation units, security-store and CSFC persistence, empty-store restore,
unpair error returns, pairing admission/results and ATT observation sites with copies
in the build output directory. The runtime separately validates pairing,
persisted bonds and device access.
"""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

ORIGINAL_SHA256 = "d47abbf20a7b42b83cf216d634d6914ffc8ccec511789f017f2c734caa3bfffd"
ORIGINAL_CASE = """        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            /* We already have a bond with the peer, but it is attempting to
             * establish a new secure link.  This app sacrifices security for
             * convenience: just throw away the old bond and accept the new link.
             */

            /* Delete the old bond. */
            rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &peerInfo.m_desc);
            if (rc != 0) {
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }

            ble_store_util_delete_peer(&peerInfo.m_desc.peer_id_addr);

            /* Return BLE_GAP_REPEAT_PAIRING_RETRY to indicate that the host should
             * continue with the pairing operation.
             */
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        } // BLE_GAP_EVENT_REPEAT_PAIRING"""
HARDENED_CASE = """        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // Litch ownership is cleared deliberately through its board owner.
            // A peer losing its key must not delete or replace the saved bond.
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        } // BLE_GAP_EVENT_REPEAT_PAIRING"""
FAILED_CONNECT_RESTART = """# if !CONFIG_BT_NIMBLE_EXT_ADV && CONFIG_BT_NIMBLE_ROLE_BROADCASTER
                NimBLEDevice::startAdvertising();
# endif"""
MAIN_LOOP_CONNECT_RESTART = """                // Litch's main loop restarts advertising through its
                // quiescence/lifecycle gate, including failed connections.
                // A host callback must not race an admitted native clear."""


def HardenServerSource(source: bytes) -> bytes:
    if hashlib.sha256(source).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Unsupported NimBLEServer.cpp; review the pinned SDK before building")
    original = ORIGINAL_CASE.encode("utf-8")
    if source.count(original) != 1:
        raise ValueError("Pinned NimBLE repeat-pairing case is missing or ambiguous")
    source = source.replace(original, HARDENED_CASE.encode("utf-8"), 1)
    restart = FAILED_CONNECT_RESTART.encode("utf-8")
    if source.count(restart) != 1:
        raise ValueError("Pinned failed-connect advertising restart is missing or ambiguous")
    return source.replace(restart, MAIN_LOOP_CONNECT_RESTART.encode("utf-8"), 1)


def WriteHardenedSource(packageDir: Path, buildDir: Path) -> Path:
    properties = dict(
        line.split("=", 1)
        for line in (packageDir / "library.properties").read_text().splitlines()
        if "=" in line and not line.startswith("#")
    )
    if properties.get("name") != "NimBLE-Arduino" or properties.get("version") != "2.3.7":
        raise ValueError("Litch repeat-pairing hardening requires NimBLE-Arduino 2.3.7")
    hardened = HardenServerSource((packageDir / "src/NimBLEServer.cpp").read_bytes())
    destination = buildDir / "litch_sdk/NimBLEServer.cpp"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_bytes() != hardened:
        destination.write_bytes(hardened)
    return destination


def _HardenNode(env, node):
    source = Path(node.srcnode().get_abspath())
    hardened = WriteHardenedSource(source.parent.parent, Path(env.subst("$BUILD_DIR")))
    return env.File(str(hardened))


NVS_ORIGINAL_SHA256 = "e4d61d3b6403e263d40f2f498c4d6ac64ee1727ccd3fd6f040ba0e1908c26144"
NVS_ORIGINAL_COUNTERS = """    ble_store_config_our_bond_count = ble_store_config_our_secs[ble_store_config_num_our_secs - 1].bond_count;
    ble_store_config_peer_bond_count = ble_store_config_peer_secs[ble_store_config_num_peer_secs - 1].bond_count;"""
NVS_HARDENED_COUNTERS = """    ble_store_config_our_bond_count = ble_store_config_num_our_secs > 0
        ? ble_store_config_our_secs[ble_store_config_num_our_secs - 1].bond_count : 0;
    ble_store_config_peer_bond_count = ble_store_config_num_peer_secs > 0
        ? ble_store_config_peer_secs[ble_store_config_num_peer_secs - 1].bond_count : 0;"""


def _ReplacePinnedFunction(source: bytes, name: str, replacement: bytes) -> bytes:
    matches = list(re.finditer(rb"(?m)^(?:static )?int(?:\n| )" + name.encode() + rb"\(", source))
    if len(matches) != 1:
        raise ValueError("Pinned store function is missing or ambiguous: " + name)
    start = matches[0].start()
    end = source.index(b"\n}", source.index(b"{", start)) + 2
    return source[:start] + replacement + source[end:]


NVS_BOUNDED_READ = b"""static int
get_nvs_db_value(int obj_type, char *key_string, union ble_store_value *val)
{
    nvs_handle_t handle;
    size_t expected;
    esp_err_t err;
    switch (obj_type) {
    case BLE_STORE_OBJ_TYPE_OUR_SEC:
    case BLE_STORE_OBJ_TYPE_PEER_SEC: expected = sizeof val->sec; break;
    case BLE_STORE_OBJ_TYPE_CCCD: expected = sizeof val->cccd; break;
    case BLE_STORE_OBJ_TYPE_CSFC: expected = sizeof val->csfc; break;
#if MYNEWT_VAL(ENC_ADV_DATA)
    case BLE_STORE_OBJ_TYPE_ENC_ADV_DATA: expected = sizeof val->ead; break;
#endif
    case BLE_STORE_OBJ_TYPE_LOCAL_IRK: expected = sizeof val->local_irk; break;
    case BLE_STORE_OBJ_TYPE_PEER_ADDR: expected = sizeof val->rpa_rec; break;
    default: return BLE_HS_ESTORE_FAIL;
    }
    err = nvs_open(NIMBLE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NVS_NOT_FOUND;
    if (err != ESP_OK) return BLE_HS_ESTORE_FAIL;
    size_t size = expected;
    err = nvs_get_blob(handle, key_string, val, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return err;
    if (err != ESP_OK || size != expected) return BLE_HS_ESTORE_FAIL;
    return ESP_OK;
}"""

# Reuse the SDK's restore and sorting body, with bounded temporary publication.
# No stored representation, identity, counter or slot is rewritten at startup.
RESTORE_TABLES = (
    ("our_secs", "sec", "BLE_STORE_MAX_BONDS"),
    ("peer_secs", "sec", "BLE_STORE_MAX_BONDS"),
    ("cccds", "cccd", "BLE_STORE_MAX_CCCDS"),
    ("csfcs", "csfc", "BLE_STORE_MAX_CSFCS"),
    ("eads", "ead", "BLE_STORE_MAX_EADS"),
    ("local_irks", "local_irk", "BLE_STORE_MAX_BONDS"),
    ("rpa_recs", "rpa_rec", "BLE_STORE_MAX_BONDS"),
)


def _HardenStartupRestore(source: bytes) -> bytes:
    start = source.index(b"static int\nble_nvs_restore_sec_keys(void)")
    end = source.index(b"\n}", start) + 2
    body = source[start:end].decode()
    declarations, publication, names = [], [], []
    for table, record, limit in RESTORE_TABLES:
        condition = "ENC_ADV_DATA" if table == "eads" else limit
        declarations += ["#if MYNEWT_VAL(" + condition + ")",
                         "        struct ble_store_value_" + record + " " + table + "[MYNEWT_VAL(" + limit + ")];",
                         "#endif", "        int num_" + table + ";"]
        publication += ["#if MYNEWT_VAL(" + condition + ")",
                        "    memcpy(ble_store_config_" + table + ", restored." + table + ", sizeof restored." + table + ");",
                        "    ble_store_config_num_" + table + " = restored.num_" + table + ";", "#endif"]
        names += [table, "num_" + table]
    for kind in ("our", "peer"):
        name = kind + "_bond_count"
        body = body.replace("    extern uint16_t ble_store_config_" + name + ";\n", "")
        declarations += ["        uint16_t " + name + ";"]
        publication += ["    extern uint16_t ble_store_config_" + name + ";",
                        "    ble_store_config_" + name + " = restored." + name + ";"]
        names += [name]
    for name in sorted(names, key=len, reverse=True):
        body = re.sub(r"\bble_store_config_" + name + r"\b", "restored." + name, body)
    body = body.replace("    esp_err_t err;", "    /* Publish only after every enabled native table has been read. */\n    struct {\n" + "\n".join(declarations) + "\n    } restored = {0};\n    esp_err_t err;", 1)
    validation = """#if MYNEWT_VAL(BLE_STORE_MAX_BONDS)
    for (int i = 0; i < restored.num_our_secs; ++i)
        for (int j = 0; j < i; ++j)
            if (!ble_addr_cmp(&restored.our_secs[i].peer_addr, &restored.our_secs[j].peer_addr))
                return BLE_HS_ESTORE_FAIL;
    for (int i = 0; i < restored.num_peer_secs; ++i)
        for (int j = 0; j < i; ++j)
            if (!ble_addr_cmp(&restored.peer_secs[i].peer_addr, &restored.peer_secs[j].peer_addr))
                return BLE_HS_ESTORE_FAIL;
#endif
"""
    body = body.replace("    return 0;\n}", validation + "\n".join(publication) + "\n    return 0;\n}")
    source = source[:start] + body.encode() + source[end:]
    # The SDK's void startup ABI remains intact. Every native callback consumes
    # this status; an incomplete inventory is a storage error, never ENOENT.
    start = source.index(b"void ble_store_config_conf_init(void)")
    end = source.index(b"\n}", start) + 2
    body = source[start:end]
    body = body.replace(b"    int err;", b"    int err;\n    litch_nimble_store_restore_status = BLE_HS_ESTORE_FAIL;", 1)
    body = body.replace(b"        ESP_LOGE(LOG_TAG, \"NVS operation failed, can't retrieve the bonding info\");", b"        ESP_LOGE(LOG_TAG, \"NVS operation failed, can't retrieve the bonding info\");\n        return;")
    body = body.replace(b"        ESP_LOGE(LOG_TAG, \"NVS operation failed, can't retrieve the peer records\");", b"        ESP_LOGE(LOG_TAG, \"NVS operation failed, can't retrieve the peer records\");\n        return;")
    body = body[:-2] + b"\n    litch_nimble_store_restore_status = 0;\n}"
    return source[:start] + body + source[end:]


def _GuardNvsFunction(source: bytes, name: str) -> bytes:
    # Each actual SDK leaf owns one lease, including every error return. Keep
    # admission outside NVS open: a denied read is unavailable, never empty.
    match = re.search(rb"(?m)^static int\n" + name.encode() + rb"\(", source)
    if match is None:
        raise ValueError("Pinned NVS leaf is missing: " + name)
    brace = source.index(b"{", match.start())
    end = source.index(b"\n}", brace) + 2
    body = source[brace + 1:end - 1]
    body = re.sub(rb"\breturn\s+([^;]+);",
                  rb"{ LitchSdkNvsEndAccess(); return \1; }", body)
    admission = b"\n    if (LitchSdkNvsBeginAccess() != 0) return BLE_HS_ESTORE_FAIL;"
    return source[:brace + 1] + admission + body + source[end - 1:]


def HardenNvsSource(source: bytes) -> bytes:
    if hashlib.sha256(source).hexdigest() != NVS_ORIGINAL_SHA256:
        raise ValueError("Unsupported ble_store_nvs.c; review the pinned SDK before building")
    original = NVS_ORIGINAL_COUNTERS.encode("utf-8")
    if source.count(original) != 1:
        raise ValueError("Pinned NVS restore counter site is missing or ambiguous")
    source = source.replace(original, NVS_HARDENED_COUNTERS.encode("utf-8"), 1)
    source = _ReplacePinnedFunction(source, "get_nvs_db_value", NVS_BOUNDED_READ)
    source = _HardenStartupRestore(source)
    # All shared helper dependencies are complete here. Keep its single owner
    # before auxiliary and security persist callers in the actual NVS TU.
    write = source.index(b"\nble_nvs_write_key_value(")
    end = source.index(b"\n}", source.index(b"{", write)) + 2
    source = source[:end] + b'\n\n#include "litch_nimble_security_store.inc"' + source[end:]
    for kind in ("peer", "our"):
        name = "ble_store_config_persist_" + kind + "_secs"
        replacement = (
            "int " + name + "(void)\n{\n"
            "#if MYNEWT_VAL(BLE_STORE_MAX_BONDS)\n"
            "    return litch_persist_security(BLE_STORE_OBJ_TYPE_" + kind.upper() + "_SEC,\n"
            "                                  ble_store_config_" + kind + "_secs,\n"
            "                                  ble_store_config_num_" + kind + "_secs);\n"
            "#else\n    return 0;\n#endif\n}"
        )
        source = _ReplacePinnedFunction(source, name, replacement.encode())
    for kind, limit in (("cccd", "BLE_STORE_MAX_CCCDS"), ("csfc", "BLE_STORE_MAX_CSFCS")):
        name = "ble_store_config_persist_" + kind + "s"
        replacement = (
            "int " + name + "(void)\n{\n"
            "#if MYNEWT_VAL(" + limit + ")\n"
            "    return litch_persist_native(BLE_STORE_OBJ_TYPE_" + kind.upper() + ",\n"
            "                                ble_store_config_" + kind + "s,\n"
            "                                ble_store_config_num_" + kind + "s,\n"
            "                                sizeof *ble_store_config_" + kind + "s,\n"
            "                                MYNEWT_VAL(" + limit + "));\n"
            "#else\n    return 0;\n#endif\n}"
        )
        source = _ReplacePinnedFunction(source, name, replacement.encode())
    source = _ReplacePinnedFunction(source, "ble_store_config_persist_rpa_recs", b"""int ble_store_config_persist_rpa_recs(void)
{
#if MYNEWT_VAL(BLE_STORE_MAX_BONDS)
    return litch_persist_native(BLE_STORE_OBJ_TYPE_PEER_ADDR, ble_store_config_rpa_recs,
        ble_store_config_num_rpa_recs, sizeof *ble_store_config_rpa_recs, MYNEWT_VAL(BLE_STORE_MAX_BONDS));
#else
    return 0;
#endif
}""")
    for name in ("get_nvs_db_value", "get_nvs_peer_record",
                 "ble_nvs_write_key_value", "ble_nvs_delete_value"):
        source = _GuardNvsFunction(source, name)
    return (b"extern int LitchSdkNvsBeginAccess(void);\n"
            b"extern void LitchSdkNvsEndAccess(void);\n" + source)


def WriteHardenedNvsSource(packageDir: Path, buildDir: Path, securitySource: Path) -> Path:
    properties = dict(
        line.split("=", 1)
        for line in (packageDir / "library.properties").read_text().splitlines()
        if "=" in line and not line.startswith("#")
    )
    if properties.get("name") != "NimBLE-Arduino" or properties.get("version") != "2.3.7":
        raise ValueError("Litch empty-store hardening requires NimBLE-Arduino 2.3.7")
    source = packageDir / "src/nimble/nimble/host/store/config/src/ble_store_nvs.c"
    hardened = HardenNvsSource(source.read_bytes())
    include = buildDir / "litch_sdk/litch_nimble_security_store.inc"
    include.parent.mkdir(parents=True, exist_ok=True)
    body = securitySource.read_bytes()
    if not include.exists() or include.read_bytes() != body:
        include.write_bytes(body)
    destination = buildDir / "litch_sdk/ble_store_nvs.c"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_bytes() != hardened:
        destination.write_bytes(hardened)
    return destination


def _HardenNvsNode(env, node):
    source = Path(node.srcnode().get_abspath())
    template = Path(env.subst("$PROJECT_DIR")) / "tools/pio/litch_nimble_security_store.inc"
    hardened = WriteHardenedNvsSource(source.parents[7], Path(env.subst("$BUILD_DIR")), template)
    # The generated copy retains its dependency-owned private header.
    env.AppendUnique(CPPPATH=[str(source.parent)])
    return env.File(str(hardened))


DEVICE_NVS_INIT = b"""        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            err = nvs_flash_erase();
            if (err == ESP_OK) {
                err = nvs_flash_init();
            }
        }
"""


def HardenDeviceSource(source: bytes) -> bytes:
    if source.count(DEVICE_NVS_INIT) != 1:
        raise ValueError("Pinned NimBLE NVS initialization block is missing or ambiguous")
    guarded = (b"        if (LitchSdkNvsBeginAccess() != 0) return false;\n" +
               DEVICE_NVS_INIT + b"        LitchSdkNvsEndAccess();\n")
    return (b'extern "C" int LitchSdkNvsBeginAccess(void);\n'
            b'extern "C" void LitchSdkNvsEndAccess(void);\n' +
            source.replace(DEVICE_NVS_INIT, guarded, 1))


def _HardenDeviceNode(env, node):
    source = Path(node.srcnode().get_abspath())
    properties = (source.parents[1] / "library.properties").read_text()
    if "name=NimBLE-Arduino\n" not in properties or "version=2.3.7\n" not in properties:
        raise ValueError("Native NVS initialization guard requires NimBLE-Arduino 2.3.7")
    destination = Path(env.subst("$BUILD_DIR")) / "litch_sdk/NimBLEDevice.cpp"
    guarded = HardenDeviceSource(source.read_bytes())
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_bytes() != guarded:
        destination.write_bytes(guarded)
    env.AppendUnique(CPPPATH=[str(source.parent)])
    return env.File(str(destination))


CONFIG_ORIGINAL_SHA256 = "58c2ae0008aa8f44c4ed2cad45c3b7f5a4756b97e1f056655c8b215da31218d2"
CONFIG_DELETE_SEC = b"""static int
ble_store_config_delete_sec(const struct ble_store_key_sec *key_sec,
                            struct ble_store_value_sec *value_secs,
                            int *num_value_secs, int (*persist)(void))
{
    int idx = ble_store_config_find_sec(key_sec, value_secs, *num_value_secs);
    int previous_count = *num_value_secs;
    int rc;
    struct ble_store_value_sec previous;
    if (idx == -1) return BLE_HS_ENOENT;
    previous = value_secs[idx];
    rc = ble_store_config_delete_obj(value_secs, sizeof *value_secs, idx, num_value_secs);
    if (rc != 0) return rc;
    rc = persist();
    if (rc != 0) {
        if (idx < *num_value_secs)
            memmove(&value_secs[idx + 1], &value_secs[idx],
                    (*num_value_secs - idx) * sizeof *value_secs);
        value_secs[idx] = previous;
        *num_value_secs = previous_count;
    }
    return rc;
}"""


PRIVACY_ORIGINAL_SHA256 = "83ef670e15211ef7b96bed2574ffab746c3b13009903074a2044b45a5d057c38"

CONFIG_RECONCILE_EMPTY = b"""/* Deliberate peer-only clear; preserve local identity and RPA records.
 * The caller owns quiescence and invokes outside the host lock. Public
 * ble_store_clear also deletes LOCAL_IRK and cannot implement this contract. */
int
LitchSdkClearPeerStore(void)
{
    const int types[] = { BLE_STORE_OBJ_TYPE_OUR_SEC, BLE_STORE_OBJ_TYPE_PEER_SEC,
                          BLE_STORE_OBJ_TYPE_CCCD, BLE_STORE_OBJ_TYPE_CSFC };
    union ble_store_key key;
    int rc = 0;
    memset(&key, 0, sizeof key);
    ble_hs_lock();
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; ++i) {
        do {
            rc = ble_store_config_delete(types[i], &key);
        } while (rc == 0);
        if (rc != BLE_HS_ENOENT) goto done;
    }
    rc = 0;
done:
    ble_hs_unlock();
    return rc;
}

/* The pinned SDK reset checks disable / clear / enable / local entry / mode.
 * Reusing the current IRK preserves RAM and durable identity. Unlike per-peer
 * unpairing, this also removes orphan controller entries after host key loss.
 * No links, advertising or discovery may remain; never hold the host lock. */
int
LitchSdkResetPeerPrivacy(void)
{
    const uint8_t *irk;
    int rc;
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    rc = ble_hs_pvcy_our_irk(&irk);
    return rc != 0 ? rc : ble_hs_pvcy_set_our_irk(irk);
}

/* Deliberate all-bond clear only: the caller must quiesce peer traffic and
 * invoke outside the host lock. RAM emptiness alone cannot prove durable
 * emptiness after a failed write restored its prior RAM snapshot. */
int
LitchSdkReconcileEmptyPeerStore(void)
{
#if defined(ESP_PLATFORM) && MYNEWT_VAL(BLE_STORE_CONFIG_PERSIST)
    extern int litch_nimble_store_restore_status;
    int rc;
    ble_hs_lock();
    if (litch_nimble_store_restore_status != 0) {
        rc = BLE_HS_ESTORE_FAIL;
        goto done;
    }
    if (ble_store_config_num_our_secs != 0 ||
        ble_store_config_num_peer_secs != 0 ||
        ble_store_config_num_cccds != 0 ||
        ble_store_config_num_csfcs != 0) {
        rc = BLE_HS_EBUSY;
        goto done;
    }
#if MYNEWT_VAL(BLE_STORE_MAX_BONDS)
    rc = ble_store_config_persist_our_secs();
    if (rc != 0) goto done;
    rc = ble_store_config_persist_peer_secs();
    if (rc != 0) goto done;
#endif
#if MYNEWT_VAL(BLE_STORE_MAX_CCCDS)
    rc = ble_store_config_persist_cccds();
    if (rc != 0) goto done;
#endif
#if MYNEWT_VAL(BLE_STORE_MAX_CSFCS)
    rc = ble_store_config_persist_csfcs();
    if (rc != 0) goto done;
#endif
    /* Each existing persistence owner just performed a fresh bounded NVS
     * verification. Local IRK and RPA records are outside this contract. */
    rc = 0;
done:
    ble_hs_unlock();
    return rc;
#else
    return BLE_HS_ENOTSUP;
#endif
}

"""


def HardenConfigSource(source: bytes, addRollback: bytes | None = None) -> bytes:
    if hashlib.sha256(source).hexdigest() != CONFIG_ORIGINAL_SHA256:
        raise ValueError("Unsupported ble_store_config.c; review the pinned SDK before building")
    source = _ReplacePinnedFunction(source, "ble_store_config_delete_sec", CONFIG_DELETE_SEC)
    source = _ReplacePinnedFunction(source, "ble_store_config_write_rpa_rec", b"""static int
ble_store_config_write_rpa_rec(const struct ble_store_value_rpa_rec *value_rpa_rec)
{
#if MYNEWT_VAL(BLE_STORE_MAX_BONDS)
    int idx, rc, previous_count = ble_store_config_num_rpa_recs;
    struct ble_store_value_rpa_rec previous;
    if (previous_count < 0 || previous_count > MYNEWT_VAL(BLE_STORE_MAX_BONDS)) return BLE_HS_ESTORE_FAIL;
    /* Update the first canonical identity mapping, including native ID aliases.
     * An RPA address alone never authorizes replacing a different identity. */
    for (idx = 0; idx < previous_count; ++idx) {
        const ble_addr_t *peer = &ble_store_config_rpa_recs[idx].peer_addr;
        if (peer->type <= BLE_ADDR_RANDOM_ID && value_rpa_rec->peer_addr.type <= BLE_ADDR_RANDOM_ID &&
            (peer->type & 1U) == (value_rpa_rec->peer_addr.type & 1U) &&
            !memcmp(peer->val, value_rpa_rec->peer_addr.val, sizeof peer->val)) break;
    }
    if (idx == previous_count) {
        if (previous_count < MYNEWT_VAL(BLE_STORE_MAX_BONDS)) ++ble_store_config_num_rpa_recs;
        else {
            /* Keep one complete raw mapping for every existing identity. Only
             * a later duplicate can supply a slot; unique orphans stay intact. */
            for (idx = 1; idx < previous_count; ++idx) {
                int earlier;
                const ble_addr_t *peer = &ble_store_config_rpa_recs[idx].peer_addr;
                for (earlier = 0; earlier < idx; ++earlier) {
                    const ble_addr_t *first = &ble_store_config_rpa_recs[earlier].peer_addr;
                    if (peer->type <= BLE_ADDR_RANDOM_ID && first->type <= BLE_ADDR_RANDOM_ID &&
                        (peer->type & 1U) == (first->type & 1U) &&
                        !memcmp(peer->val, first->val, sizeof peer->val)) break;
                }
                if (earlier < idx) break;
            }
            if (idx == previous_count) return BLE_HS_ESTORE_CAP;
        }
    }
    previous = ble_store_config_rpa_recs[idx];
    ble_store_config_rpa_recs[idx] = *value_rpa_rec;
    rc = ble_store_config_persist_rpa_recs();
    if (rc != 0) {
        ble_store_config_rpa_recs[idx] = previous;
        ble_store_config_num_rpa_recs = previous_count;
    }
    return rc;
#else
    return BLE_HS_ENOENT;
#endif
}""")
    guard = b"""
#if defined(ESP_PLATFORM) && MYNEWT_VAL(BLE_STORE_CONFIG_PERSIST)
    extern int litch_nimble_store_restore_status;
    if (litch_nimble_store_restore_status != 0) return BLE_HS_ESTORE_FAIL;
#endif
"""
    for name in ("ble_store_config_read", "ble_store_config_write", "ble_store_config_delete"):
        start = source.index(("\n" + name + "(").encode())
        brace = source.index(b"{", start) + 1
        source = source[:brace] + guard + source[brace:]
    for kind in ("our", "peer"):
        # Renumber the existing identities through the same bounded upsert owner.
        # The SDK's delete-all/recreate sequence could commit an empty table
        # before recreation failed. Keep retryable RAM; an NVS error may already
        # have made some counter updates visible, but never deletes custody.
        table = "ble_store_config_" + kind + "_secs"
        count = "ble_store_config_num_" + kind + "_secs"
        counter = "ble_store_config_" + kind + "_bond_count"
        name = "ble_restore_" + kind + "_sec_nvs"
        body = (
            "int " + name + "(void)\n{\n"
            "    extern uint16_t " + counter + ";\n"
            "    struct ble_store_value_sec previous[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];\n"
            "    int count = " + count + ";\n    int rc;\n"
            "    if (count < 0 || count > MYNEWT_VAL(BLE_STORE_MAX_BONDS))\n"
            "        return BLE_HS_ESTORE_FAIL;\n"
            "    memcpy(previous, " + table + ", count * sizeof *previous);\n"
            "    qsort(" + table + ", count, sizeof *previous, ble_store_config_compare_bond_count);\n"
            "    for (int i = 0; i < count; ++i) " + table + "[i].bond_count = i + 1;\n"
            "    rc = ble_store_config_persist_" + kind + "_secs();\n"
            "    if (rc != 0) {\n"
            "        memcpy(" + table + ", previous, count * sizeof *previous);\n"
            "        return rc;\n    }\n"
            "    " + counter + " = count;\n    return 0;\n}"
        )
        source = _ReplacePinnedFunction(source, name, body.encode())
        name = "ble_store_config_delete_" + kind + "_sec"
        body = (
            "static int\n" + name + "(const struct ble_store_key_sec *key_sec)\n{\n"
            "#if MYNEWT_VAL(BLE_STORE_MAX_BONDS)\n"
            "    return ble_store_config_delete_sec(key_sec, ble_store_config_" + kind + "_secs,\n"
            "                                       &ble_store_config_num_" + kind + "_secs,\n"
            "                                       ble_store_config_persist_" + kind + "_secs);\n"
            "#else\n    return BLE_HS_ENOENT;\n#endif\n}"
        )
        source = _ReplacePinnedFunction(source, name, body.encode())
        marker = ("\nble_store_config_write_" + kind + "_sec(").encode()
        begin = source.rfind(b"static int\n", 0, source.index(marker) + 1)
        end = source.index(b"\n}", source.index(b"{", begin)) + 2
        before = source[begin:end]
        after = before.replace(b"    int rc;", (
            "    int rc;\n    int previous_count;\n"
            "    uint16_t previous_counter;\n"
            "    struct ble_store_value_sec previous;"
        ).encode(), 1)
        # A failed prior renumber leaves its high counter intact. Repair that
        # bounded work before another increment, even across repeated failures.
        lookup = b"    ble_store_key_from_value_sec(&key_sec, value_sec);"
        preflight = (
            "    if (" + counter + " > (UINT16_MAX - 5)) {\n"
            "        rc = ble_restore_" + kind + "_sec_nvs();\n"
            "        if (rc != 0) return rc;\n    }\n"
            "    previous_count = " + count + ";\n"
            "    previous_counter = " + counter + ";\n"
        ).encode()
        if after.count(lookup) != 1:
            raise ValueError("Pinned security write lookup is missing: " + kind)
        after = after.replace(lookup, preflight + lookup, 1)
        assignment = ("    ble_store_config_" + kind + "_secs[idx] = *value_sec;").encode()
        after = after.replace(assignment, ("    previous = ble_store_config_" + kind + "_secs[idx];\n").encode() + assignment, 1)
        old = ("    rc = ble_store_config_persist_" + kind + "_secs();\n    if (rc != 0) {\n        return rc;\n    }").encode()
        new = ("    rc = ble_store_config_persist_" + kind + "_secs();\n    if (rc != 0) {\n"
               "        ble_store_config_" + kind + "_secs[idx] = previous;\n"
               "        ble_store_config_num_" + kind + "_secs = previous_count;\n"
               "        ble_store_config_" + kind + "_bond_count = previous_counter;\n"
               "        return rc;\n    }").encode()
        if after.count(old) != 1 or after == before:
            raise ValueError("Pinned security write site is missing: " + kind)
        after = after.replace(old, new, 1)
        source = source[:begin] + after + source[end:]
    # Auxiliary writes use the same exact-inventory owner as security records.
    # Preserve RAM on error so a failed delete remains reachable by its key.
    for kind in ("cccd", "csfc"):
        table = "ble_store_config_" + kind + "s"
        count = "ble_store_config_num_" + kind + "s"
        for action in ("write", "delete"):
            name = "ble_store_config_" + action + "_" + kind
            marker = ("\n" + name + "(").encode()
            begin = source.rfind(b"static int\n", 0, source.index(marker) + 1)
            end = source.index(b"\n}", source.index(b"{", begin)) + 2
            before = source[begin:end]
            after = before.replace(b"    int rc;", (
                "    int rc;\n    int previous_count = " + count + ";\n"
                "    struct ble_store_value_" + kind + " previous;"
            ).encode(), 1)
            assignment = ("    " + table + "[idx] = *value_" + kind + ";").encode()
            if action == "delete":
                assignment = b"    rc = ble_store_config_delete_obj("
            after = after.replace(assignment, ("    previous = " + table + "[idx];\n").encode() + assignment, 1)
            old = ("    rc = ble_store_config_persist_" + kind + "s();\n    if (rc != 0) {\n        return rc;\n    }").encode()
            rollback = ""
            if action == "delete":
                rollback = (
                    "        if (idx < " + count + ")\n"
                    "            memmove(&" + table + "[idx + 1], &" + table + "[idx],\n"
                    "                    (previous_count - idx - 1) * sizeof *" + table + ");\n"
                )
            new = ("    rc = ble_store_config_persist_" + kind + "s();\n    if (rc != 0) {\n" +
                   rollback + "        " + table + "[idx] = previous;\n"
                   "        " + count + " = previous_count;\n        return rc;\n    }").encode()
            if after.count(old) != 1 or after == before:
                raise ValueError("Pinned auxiliary mutation site is missing: " + name)
            after = after.replace(old, new, 1)
            if kind == "cccd" and action == "delete":
                absent = b"    if (idx == -1) {\n        return BLE_HS_ENOENT;\n    }"
                if after.count(absent) != 1:
                    raise ValueError("Pinned CCCD absence branch is missing")
                after = after.replace(absent, b"""    if (idx == -1) {
        /* A prior failed append may already be durable behind absent RAM. */
        rc = ble_store_config_persist_cccds();
        return rc != 0 ? rc : BLE_HS_ENOENT;
    }""", 1)
            source = source[:begin] + after + source[end:]
    marker = b'#include "ble_store_config_priv.h"'
    if source.count(marker) != 1:
        raise ValueError("Pinned config private include is missing or ambiguous")
    source = source.replace(marker, marker + b'\n#include "nimble/nimble/host/src/ble_hs_priv.h"', 1)
    marker = b"void\nble_store_config_init(void)"
    if source.count(marker) != 1:
        raise ValueError("Pinned config initialization is missing or ambiguous")
    if addRollback is None:
        addRollback = Path(__file__).with_name("litch_nimble_add_rollback.inc").read_bytes()
    source = source.replace(marker, CONFIG_RECONCILE_EMPTY + addRollback + b"\n" + marker, 1)
    return source


def WriteHardenedConfigSource(packageDir: Path, buildDir: Path,
                              addRollbackSource: Path | None = None) -> Path:
    properties = dict(line.split("=", 1) for line in
                      (packageDir / "library.properties").read_text().splitlines()
                      if "=" in line and not line.startswith("#"))
    if properties.get("name") != "NimBLE-Arduino" or properties.get("version") != "2.3.7":
        raise ValueError("Litch durable security hardening requires NimBLE-Arduino 2.3.7")
    privacy = packageDir / "src/nimble/nimble/host/src/ble_hs_pvcy.c"
    if hashlib.sha256(privacy.read_bytes()).hexdigest() != PRIVACY_ORIGINAL_SHA256:
        raise ValueError("Unsupported privacy reset source; review the pinned SDK before building")
    source = packageDir / "src/nimble/nimble/host/store/config/src/ble_store_config.c"
    hardened = HardenConfigSource(source.read_bytes(),
        addRollbackSource.read_bytes() if addRollbackSource is not None else None)
    destination = buildDir / "litch_sdk/ble_store_config.c"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_bytes() != hardened:
        destination.write_bytes(hardened)
    return destination


def _HardenConfigNode(env, node):
    source = Path(node.srcnode().get_abspath())
    template = Path(env.subst("$PROJECT_DIR")) / "tools/pio/litch_nimble_add_rollback.inc"
    hardened = WriteHardenedConfigSource(source.parents[7], Path(env.subst("$BUILD_DIR")), template)
    env.AppendUnique(CPPPATH=[str(source.parent)])
    return env.File(str(hardened))


UTIL_ORIGINAL_SHA256 = "a25a4886c9cd47522287d9f43eb7e917c8e1383cd4247701bd2c5b875304524a"
UTIL_COMPLETE_INVENTORY = b"""int
ble_store_util_bonded_peers(ble_addr_t *out_peer_id_addrs, int *out_num_peers,
                            int max_peers)
{
    /* Each security table is bounded independently. Publish their complete
     * identity union only when both reads and caller capacity succeed. */
    ble_addr_t peers[2 * MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    struct ble_store_util_peer_set set = {
        .peer_id_addrs = peers,
        .num_peers = 0,
        .max_peers = 2 * MYNEWT_VAL(BLE_STORE_MAX_BONDS),
        .status = 0,
    };
    int rc;
    if (max_peers < 0 || out_num_peers == NULL ||
        (max_peers > 0 && out_peer_id_addrs == NULL)) return BLE_HS_EINVAL;
    rc = ble_store_iterate(BLE_STORE_OBJ_TYPE_OUR_SEC,
                           ble_store_util_iter_unique_peer, &set);
    if (rc != 0) return rc;
    if (set.status != 0) return set.status;
    rc = ble_store_iterate(BLE_STORE_OBJ_TYPE_PEER_SEC,
                           ble_store_util_iter_unique_peer, &set);
    if (rc != 0) return rc;
    if (set.status != 0) return set.status;
    if (set.num_peers > max_peers) return BLE_HS_ENOMEM;
    if (set.num_peers > 0)
        memcpy(out_peer_id_addrs, peers, set.num_peers * sizeof peers[0]);
    *out_num_peers = set.num_peers;
    return 0;
}"""
UTIL_NON_EVICTING_STATUS = b"""/**
 * Litch keeps the SDK callback ABI but never frees custody on overflow.
 * Let an ordinary write decide capacity; report actual overflow to its caller.
 */
int
ble_store_util_status_rr(struct ble_store_status_event *event, void *arg)
{
    (void)arg;
    switch (event->event_code) {
    case BLE_STORE_EVENT_OVERFLOW:
        switch (event->overflow.obj_type) {
        case BLE_STORE_OBJ_TYPE_OUR_SEC:
        case BLE_STORE_OBJ_TYPE_PEER_SEC:
        case BLE_STORE_OBJ_TYPE_PEER_ADDR:
        case BLE_STORE_OBJ_TYPE_CCCD:
        case BLE_STORE_OBJ_TYPE_CSFC:
#if MYNEWT_VAL(ENC_ADV_DATA)
        case BLE_STORE_OBJ_TYPE_ENC_ADV_DATA:
#endif
            return BLE_HS_ESTORE_CAP;
        default:
            return BLE_HS_EUNKNOWN;
        }
    case BLE_STORE_EVENT_FULL:
        return 0;
    default:
        return BLE_HS_EUNKNOWN;
    }
}"""


def HardenUtilSource(source: bytes) -> bytes:
    if hashlib.sha256(source).hexdigest() != UTIL_ORIGINAL_SHA256:
        raise ValueError("Unsupported ble_store_util.c; review the pinned SDK before building")
    # These SDK functions have int-on-its-own-line definitions. Replace the
    # status callback's demonstration-eviction comment together with its body.
    for name, replacement in ((b"ble_store_util_bonded_peers", UTIL_COMPLETE_INVENTORY),
                              (b"ble_store_util_status_rr", UTIL_NON_EVICTING_STATUS)):
        start = source.rfind(b"\nint\n", 0, source.index(b"\n" + name + b"(") + 1) + 1
        end = source.index(b"\n}", start) + 2
        if name == b"ble_store_util_status_rr":
            start = source.rfind(b"/**\n", 0, start)
        source = source[:start] + replacement + source[end:]
    return source


def WriteHardenedUtilSource(packageDir: Path, buildDir: Path) -> Path:
    properties = dict(line.split("=", 1) for line in
                      (packageDir / "library.properties").read_text().splitlines()
                      if "=" in line and not line.startswith("#"))
    if properties.get("name") != "NimBLE-Arduino" or properties.get("version") != "2.3.7":
        raise ValueError("Litch bond inventory and non-evicting capacity require NimBLE-Arduino 2.3.7")
    source = packageDir / "src/nimble/nimble/host/src/ble_store_util.c"
    hardened = HardenUtilSource(source.read_bytes())
    destination = buildDir / "litch_sdk/ble_store_util.c"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_bytes() != hardened:
        destination.write_bytes(hardened)
    return destination


def _HardenUtilNode(env, node):
    source = Path(node.srcnode().get_abspath())
    hardened = WriteHardenedUtilSource(source.parents[5], Path(env.subst("$BUILD_DIR")))
    env.AppendUnique(CPPPATH=[str(source.parent)])
    return env.File(str(hardened))


GAP_ORIGINAL_SHA256 = "8cadddeeacfd4e8f0ef010c21f4a7f4d30077459cacdd6821ba33c8b0328ce44"
GAP_DELETION_REPLACEMENTS = (
    (
        '            BLE_HS_LOG(ERROR, "Error while removing LTK , rc = %x\\n",rc);\n'
        '        }',
        '            BLE_HS_LOG(ERROR, "Error while removing LTK , rc = %x\\n",rc);\n'
        '            err = rc;\n'
        '        }',
    ),
    (
        '            ble_store_util_delete_peer(&key.sec.peer_addr);',
        '            err = ble_store_util_delete_peer(&key.sec.peer_addr);',
    ),
)
GAP_DATA_LENGTH_REPLACEMENT = (
    '    event.type = BLE_GAP_EVENT_DATA_LEN_CHG;\n'
    '    event.data_len_chg.max_tx_octets = le16toh(ev->max_tx_octets);',
    '    event.type = BLE_GAP_EVENT_DATA_LEN_CHG;\n'
    '    event.data_len_chg.conn_handle = conn_handle;\n'
    '    event.data_len_chg.max_tx_octets = le16toh(ev->max_tx_octets);',
)


def HardenGapSource(source: bytes) -> bytes:
    if hashlib.sha256(source).hexdigest() != GAP_ORIGINAL_SHA256:
        raise ValueError("Unsupported ble_gap.c; review the pinned SDK before building")
    for original, replacement in (*GAP_DELETION_REPLACEMENTS, GAP_DATA_LENGTH_REPLACEMENT):
        before = original.encode("utf-8")
        if source.count(before) != 1:
            raise ValueError("Pinned GAP transformation site is missing or ambiguous")
        source = source.replace(before, replacement.encode("utf-8"), 1)
    return source


def WriteHardenedGapSource(packageDir: Path, buildDir: Path) -> Path:
    properties = dict(
        line.split("=", 1)
        for line in (packageDir / "library.properties").read_text().splitlines()
        if "=" in line and not line.startswith("#")
    )
    if properties.get("name") != "NimBLE-Arduino" or properties.get("version") != "2.3.7":
        raise ValueError("Litch GAP hardening requires NimBLE-Arduino 2.3.7")
    source = packageDir / "src/nimble/nimble/host/src/ble_gap.c"
    hardened = HardenGapSource(source.read_bytes())
    destination = buildDir / "litch_sdk/ble_gap.c"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_bytes() != hardened:
        destination.write_bytes(hardened)
    return destination


def _HardenGapNode(env, node):
    source = Path(node.srcnode().get_abspath())
    hardened = WriteHardenedGapSource(source.parents[5], Path(env.subst("$BUILD_DIR")))
    env.AppendUnique(CPPPATH=[str(source.parent)])
    return env.File(str(hardened))


SM_ORIGINAL_SHA256 = "f88810ae6fcdf1f499efcb1471b18b36a154e7cd67dd45145b70bd4822d120da"
SM_ADMISSION_HOOK = """/* The optional Litch runtime owns consent and connection/session lifetime.
 * Absence preserves feature-off SDK behavior. The predicate must be bounded,
 * read synchronized state only, and return a native host error to refuse. */
extern int LitchSdkPairingAdmission(uint16_t conn_handle) __attribute__((weak));

static int
litch_sm_pairing_admission(uint16_t conn_handle)
{
    BLE_HS_DBG_ASSERT(!ble_hs_locked_by_cur_task());
    return LitchSdkPairingAdmission ? LitchSdkPairingAdmission(conn_handle) : 0;
}

"""


def HardenSmSource(source: bytes) -> bytes:
    if hashlib.sha256(source).hexdigest() != SM_ORIGINAL_SHA256:
        raise ValueError("Unsupported ble_sm.c; review the pinned SDK before building")
    replacements = (
        ("/** Procedure timeout; 30 seconds. */", SM_ADMISSION_HOOK + "/** Procedure timeout; 30 seconds. */"),
        ("    req = (struct ble_sm_pair_cmd *)(*om)->om_data;\n\n    ble_hs_lock();",
         "    req = (struct ble_sm_pair_cmd *)(*om)->om_data;\n\n"
         "    /* Refuse unsolicited pairing before capacity checks or proc mutation. */\n"
         "    res->app_status = litch_sm_pairing_admission(conn_handle);\n"
         "    if (res->app_status != 0) {\n"
         "        res->sm_err = BLE_SM_ERR_PAIR_NOT_SUPP;\n"
         "        return;\n"
         "    }\n\n    ble_hs_lock();"),
        ("static void\nble_sm_persist_keys(struct ble_sm_proc *proc)",
         "static int\nble_sm_persist_keys(struct ble_sm_proc *proc)"),
        ("    int identity_ev = 0;\n    int sc;",
         "    int identity_ev = 0;\n    int sc;\n    int rc;"),
        ("    authenticated = proc->flags & BLE_SM_PROC_F_AUTHENTICATED;\n    sc = proc->flags & BLE_SM_PROC_F_SC;",
         "    /* Consent can retire while native key exchange is in progress. */\n"
         "    rc = litch_sm_pairing_admission(proc->conn_handle);\n"
         "    if (rc != 0) return rc;\n\n"
         "    authenticated = proc->flags & BLE_SM_PROC_F_AUTHENTICATED;\n    sc = proc->flags & BLE_SM_PROC_F_SC;"),
        ("    ble_store_write_our_sec(&value_sec);",
         "    rc = ble_store_write_our_sec(&value_sec);\n    if (rc != 0) return rc;"),
        ("    ble_store_write_peer_sec(&value_sec);",
         "    rc = ble_store_write_peer_sec(&value_sec);\n    if (rc != 0) return rc;"),
        ("    ble_store_write_rpa_rec(&value_rpa_rec);\n}",
         "    /* A zero RPA denotes a direct identity connection: no mapping. */\n"
         "    if (memcmp(value_rpa_rec.peer_rpa_addr.val, BLE_ADDR_ANY->val, 6) == 0) return 0;\n"
         "    return ble_store_write_rpa_rec(&value_rpa_rec);\n}"),
        ("        if (res->enc_cb &&\n"
         "            res->app_status != BLE_HS_ENOTCONN) {\n"
         "            /* Do not send this event on broken connection */\n"
         "            ble_gap_pairing_complete_event(conn_handle, res->sm_err);\n"
         "        }\n", ""),
        ("            ble_sm_persist_keys(proc);",
         "            res->app_status = ble_sm_persist_keys(proc);\n"
         "            if (res->app_status != 0) {\n"
         "                res->bonded = 0;\n"
         "                res->sm_err = BLE_SM_ERR_UNSPECIFIED;\n"
         "            }"),
        ("        if (res->enc_cb) {\n            BLE_HS_DBG_ASSERT(proc == NULL || rm);",
         "        if (res->enc_cb &&\n"
         "            res->app_status != BLE_HS_ENOTCONN) {\n"
         "            /* Publish completion only after checked security persistence. */\n"
         "            ble_gap_pairing_complete_event(conn_handle, res->sm_err);\n"
         "        }\n\n"
         "        if (res->enc_cb) {\n            BLE_HS_DBG_ASSERT(proc == NULL || rm);"),
    )
    for original, replacement in replacements:
        before = original.encode()
        if source.count(before) != 1:
            raise ValueError("Pinned SMP admission/result site is missing or ambiguous")
        source = source.replace(before, replacement.encode(), 1)
    return source


def WriteHardenedSmSource(packageDir: Path, buildDir: Path) -> Path:
    properties = dict(
        line.split("=", 1)
        for line in (packageDir / "library.properties").read_text().splitlines()
        if "=" in line and not line.startswith("#")
    )
    if properties.get("name") != "NimBLE-Arduino" or properties.get("version") != "2.3.7":
        raise ValueError("Litch pairing admission and results require NimBLE-Arduino 2.3.7")
    source = packageDir / "src/nimble/nimble/host/src/ble_sm.c"
    hardened = HardenSmSource(source.read_bytes())
    destination = buildDir / "litch_sdk/ble_sm.c"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_bytes() != hardened:
        destination.write_bytes(hardened)
    return destination


def _HardenSmNode(env, node):
    source = Path(node.srcnode().get_abspath())
    hardened = WriteHardenedSmSource(source.parents[5], Path(env.subst("$BUILD_DIR")))
    env.AppendUnique(CPPPATH=[str(source.parent)])
    return env.File(str(hardened))


GATTS_ORIGINAL_SHA256 = "5d41a95f285a59910f691723726be91974cc9606f7ab8dd781e32ec0df7c3514"

CCCD_ACCESS = b"""int
ble_gatts_clt_cfg_access(uint16_t conn_handle, uint16_t attr_handle,
                         uint8_t op, uint16_t offset, struct os_mbuf **om,
                         void *arg)
{
    struct ble_store_value_cccd cccd_value;
    struct ble_store_key_cccd cccd_key;
    struct ble_hs_conn *conn;
    struct ble_gatts_clt_cfg *clt_cfg;
    uint16_t chr_val_handle = attr_handle - 1;
    uint8_t prev_flags = 0, cur_flags = 0;
    int rc;

    ble_hs_lock();
    conn = ble_hs_conn_find(conn_handle);
    if (conn == NULL) { rc = BLE_ATT_ERR_UNLIKELY; goto done; }
    rc = ble_gatts_clt_cfg_access_locked(conn, attr_handle, op, offset,
                                         *om, &cccd_value, &prev_flags, &cur_flags);
    if (rc != 0) goto done;
    /* Keep the same connection locked through persistence. The registered
     * callbacks own rollback; public store dispatch would take this lock again. */
    if (cccd_value.chr_val_handle != 0) {
        if (cccd_value.flags == 0) {
            ble_store_key_from_value_cccd(&cccd_key, &cccd_value);
            rc = ble_hs_cfg.store_delete_cb == NULL ? BLE_HS_ENOTSUP :
                ble_hs_cfg.store_delete_cb(BLE_STORE_OBJ_TYPE_CCCD, (const void *)&cccd_key);
            if (rc == BLE_HS_ENOENT) rc = 0;
        } else {
            rc = ble_hs_cfg.store_write_cb == NULL ? BLE_HS_ENOTSUP :
                ble_hs_cfg.store_write_cb(BLE_STORE_OBJ_TYPE_CCCD, (const void *)&cccd_value);
        }
        if (rc != 0) {
            rc = rc == BLE_HS_ESTORE_CAP ? BLE_ATT_ERR_INSUFFICIENT_RES : BLE_ATT_ERR_UNLIKELY;
            goto done;
        }
    }
#if MYNEWT_VAL(BLE_DYNAMIC_SERVICE)
    clt_cfg = ble_gatts_clt_cfg_find(&conn->bhc_gatt_svr.clt_cfgs, chr_val_handle);
#else
    clt_cfg = ble_gatts_clt_cfg_find(conn->bhc_gatt_svr.clt_cfgs, chr_val_handle);
#endif
    clt_cfg->flags = cur_flags;
done:
    ble_hs_unlock();
    if (rc == 0) {
        ble_gatts_subscribe_event(conn_handle, chr_val_handle,
                                  BLE_GAP_SUBSCRIBE_REASON_WRITE, prev_flags, cur_flags);
    }
    return rc;
}"""


def HardenGattsSource(source: bytes) -> bytes:
    if hashlib.sha256(source).hexdigest() != GATTS_ORIGINAL_SHA256:
        raise ValueError("Unsupported GATT source; review the pinned SDK before building")
    marker = b"int\nble_gatts_peer_cl_sup_feat_update("
    if source.count(marker) != 1:
        raise ValueError("Pinned CSFC update is missing or ambiguous")
    start = source.index(marker)
    end = source.index(b"\n}", start) + 2
    update = source[start:end]
    publication = b"    memcpy(conn->bhc_gatt_svr.peer_cl_sup_feat, feat, MYNEWT_VAL(BLE_GATT_CSFC_SIZE));\n"
    deletion = b"""        memset(&key_csfc, 0, sizeof key_csfc);
        key_csfc.peer_addr = conn->bhc_peer_addr;

        rc = ble_store_delete_csfc(&key_csfc);
        if (rc != 0) {
            goto done;
        }

"""
    old_write = b"""        rc = ble_store_write_csfc(&value_csfc);
        if (rc != 0) {
            goto done;
        }"""
    locked_write = b"""        /* This caller owns the host lock; public store dispatch would relock.
         * Reuse its registered owner without eviction or a lifecycle gap.
         * Upsert preserves the prior record when persistence reports failure.
         */
        if (ble_hs_cfg.store_write_cb == NULL) {
            rc = BLE_ATT_ERR_UNLIKELY;
            goto done;
        }
        rc = ble_hs_cfg.store_write_cb(BLE_STORE_OBJ_TYPE_CSFC,
                                      (const void *)&value_csfc);
        if (rc != 0) {
            rc = rc == BLE_HS_ESTORE_CAP ? BLE_ATT_ERR_INSUFFICIENT_RES
                                        : BLE_ATT_ERR_UNLIKELY;
            goto done;
        }"""
    replacements = (
        (b"    struct ble_store_key_csfc key_csfc;\n", b""),
        (publication, b""), (deletion, b""), (old_write, locked_write),
        (b"\ndone:", b"\n" + publication + b"\ndone:"),
    )
    for original, replacement in replacements:
        if update.count(original) != 1:
            raise ValueError("Pinned CSFC mutation site is missing or ambiguous")
        update = update.replace(original, replacement, 1)
    source = source[:start] + update + source[end:]
    # Validation returns a candidate; publication follows successful persistence.
    marker = b"static int\nble_gatts_clt_cfg_access_locked("
    begin = source.index(marker)
    finish = source.index(b"\n}", source.index(b"{", begin)) + 2
    locked = source[begin:finish]
    candidate = locked.index(b"        if (clt_cfg->flags != flags) {")
    candidate_end = locked.index(b"\n        break;", candidate)
    locked = locked[:candidate] + b"""        *out_cur_clt_cfg_flags = flags;
        /* Reconcile every bonded write, even a retry of prior live flags:
         * an earlier reported failure may already have changed NVS. */
        if (conn->bhc_sec_state.bonded) {
            out_cccd->peer_addr = conn->bhc_peer_addr;
            out_cccd->peer_addr.type =
                ble_hs_misc_peer_addr_type_to_id(conn->bhc_peer_addr.type);
            out_cccd->chr_val_handle = chr_val_handle;
            out_cccd->flags = flags;
            out_cccd->value_changed = 0;
        }""" + locked[candidate_end:]
    source = source[:begin] + locked + source[finish:]
    return _ReplacePinnedFunction(source, "ble_gatts_clt_cfg_access", CCCD_ACCESS)


def WriteHardenedGattsSource(packageDir: Path, buildDir: Path) -> Path:
    properties = dict(
        line.split("=", 1)
        for line in (packageDir / "library.properties").read_text().splitlines()
        if "=" in line and not line.startswith("#")
    )
    if properties.get("name") != "NimBLE-Arduino" or properties.get("version") != "2.3.7":
        raise ValueError("CSFC persistence hardening requires NimBLE-Arduino 2.3.7")
    source = packageDir / "src/nimble/nimble/host/src/ble_gatts.c"
    hardened = HardenGattsSource(source.read_bytes())
    destination = buildDir / "litch_sdk/ble_gatts.c"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_bytes() != hardened:
        destination.write_bytes(hardened)
    return destination


def _HardenGattsNode(env, node):
    source = Path(node.srcnode().get_abspath())
    hardened = WriteHardenedGattsSource(source.parents[5], Path(env.subst("$BUILD_DIR")))
    env.AppendUnique(CPPPATH=[str(source.parent)])
    return env.File(str(hardened))


ATT_SOURCE_TRANSFORMS = {'ble_att.c': {'sha256': '9d9c7f8a5b8155e0e3732d696017490923ea36437ccf994453b58f95c02f9bfd',
               'replacements': [['#include "ble_hs_priv.h"',
                                 '#include "ble_hs_priv.h"\n'
                                 '#include "modules/sdk_att_witness.hpp"\n'
                                 '#define LITCH_ATT_OBSERVE(phase, conn, cid, op, handle, status) \\\n'
                                 '    LitchSdkAttObserve((phase), ((uint32_t)(conn) << 16) | '
                                 '(uint16_t)(cid), \\\n'
                                 '                      ((uint32_t)(op) << 16) | (uint16_t)(handle), '
                                 '(status), \\\n'
                                 '                      (unsigned)os_msys_count(), '
                                 '(unsigned)os_msys_num_free())\n'],
                                ['    uint8_t op;\n    int rc;\n\n    BLE_HS_DBG_ASSERT(*om != NULL);',
                                 '    uint8_t op;\n'
                                 '    uint16_t litch_handle = 0;\n'
                                 '    uint8_t litch_handle_bytes[2];\n'
                                 '    int rc;\n'
                                 '\n'
                                 '    BLE_HS_DBG_ASSERT(*om != NULL);'],
                                ['    if (cid == BLE_L2CAP_CID_ATT && ble_att_is_response_op(op)) {',
                                 '    if ((op == BLE_ATT_OP_WRITE_REQ || op == BLE_ATT_OP_FIND_INFO_REQ '
                                 '||\n'
                                 '         op == BLE_ATT_OP_READ_TYPE_REQ || op == '
                                 'BLE_ATT_OP_READ_GROUP_TYPE_REQ) &&\n'
                                 '        os_mbuf_copydata(*om, 1, 2, litch_handle_bytes) == 0) {\n'
                                 '        litch_handle = (uint16_t)litch_handle_bytes[0] |\n'
                                 '                       ((uint16_t)litch_handle_bytes[1] << 8);\n'
                                 '    }\n'
                                 '    LITCH_ATT_OBSERVE(LITCH_ATT_ENTER, conn_handle, cid, op, '
                                 'litch_handle, 0);\n'
                                 '\n'
                                 '    if (cid == BLE_L2CAP_CID_ATT && ble_att_is_response_op(op)) {'],
                                ['        return BLE_HS_ENOTSUP;\n    }\n\n    ble_att_inc_rx_stat(op);',
                                 '        LITCH_ATT_OBSERVE(LITCH_ATT_RETURN, conn_handle, cid, op, '
                                 'litch_handle, BLE_HS_ENOTSUP);\n'
                                 '        return BLE_HS_ENOTSUP;\n'
                                 '    }\n'
                                 '\n'
                                 '    ble_att_inc_rx_stat(op);'],
                                ['    rc = entry->bde_fn(conn_handle, cid, om);',
                                 '    LITCH_ATT_OBSERVE(LITCH_ATT_DISPATCH, conn_handle, cid, op, '
                                 'litch_handle, 0);\n'
                                 '    rc = entry->bde_fn(conn_handle, cid, om);\n'
                                 '    LITCH_ATT_OBSERVE(LITCH_ATT_RETURN, conn_handle, cid, op, '
                                 'litch_handle, rc);']]},
 'ble_att_svr.c': {'sha256': '273ab4bbfb33c8061ae916f669f6db9629b26152f025cd5c7eaf46d8fd72342b',
                   'replacements': [['#include "ble_hs_priv.h"',
                                     '#include "ble_hs_priv.h"\n'
                                     '#include "modules/sdk_att_witness.hpp"\n'
                                     '#define LITCH_ATT_OBSERVE(phase, conn, cid, op, handle, status) '
                                     '\\\n'
                                     '    LitchSdkAttObserve((phase), ((uint32_t)(conn) << 16) | '
                                     '(uint16_t)(cid), \\\n'
                                     '                      ((uint32_t)(op) << 16) | '
                                     '(uint16_t)(handle), (status), \\\n'
                                     '                      (unsigned)os_msys_count(), '
                                     '(unsigned)os_msys_num_free())\n'],
                                    ['    *out_txom = ble_hs_mbuf_l2cap_pkt();',
                                     '    *out_txom = ble_hs_mbuf_l2cap_pkt();\n'
                                     '    LitchSdkAttAllocation(*out_txom != NULL, '
                                     '(unsigned)os_msys_count(),\n'
                                     '                          (unsigned)os_msys_num_free());'],
                                    ['    int do_tx;\n\n    if (hs_status != 0',
                                     '    int do_tx;\n'
                                     '\n'
                                     '    LITCH_ATT_OBSERVE(LITCH_ATT_REPLY_ENTER, conn_handle, cid, '
                                     'att_op, err_handle, hs_status);\n'
                                     '    if (hs_status != 0'],
                                    ['            hs_status = ble_att_tx(conn_handle, cid, om);',
                                     '            LITCH_ATT_OBSERVE(LITCH_ATT_TX_ENTER, conn_handle, '
                                     'cid, att_op, err_handle, 0);\n'
                                     '            hs_status = ble_att_tx(conn_handle, cid, om);\n'
                                     '            LITCH_ATT_OBSERVE(LITCH_ATT_TX_RETURN, conn_handle, '
                                     'cid, att_op, err_handle, hs_status);'],
                                    ['                om = ble_hs_mbuf_l2cap_pkt();',
                                     '                om = ble_hs_mbuf_l2cap_pkt();\n'
                                     '                LitchSdkAttAllocation(om != NULL, '
                                     '(unsigned)os_msys_count(),\n'
                                     '                                      '
                                     '(unsigned)os_msys_num_free());'],
                                    ['                ble_att_svr_tx_error_rsp(conn_handle, cid, om, '
                                     'att_op,\n'
                                     '                                         err_handle, err_status);',
                                     '                int litch_error_rc;\n'
                                     '                LITCH_ATT_OBSERVE(LITCH_ATT_TX_ENTER, '
                                     'conn_handle, cid, att_op, err_handle, 0);\n'
                                     '                litch_error_rc = '
                                     'ble_att_svr_tx_error_rsp(conn_handle, cid, om, att_op,\n'
                                     '                                                         '
                                     'err_handle, err_status);\n'
                                     '                LITCH_ATT_OBSERVE(LITCH_ATT_TX_RETURN, '
                                     'conn_handle, cid, att_op, err_handle, litch_error_rc);'],
                                    ['    return hs_status;\n}\n\nstatic int\nble_att_svr_build_mtu_rsp',
                                     '    LITCH_ATT_OBSERVE(LITCH_ATT_REPLY_RETURN, conn_handle, cid, '
                                     'att_op, err_handle, hs_status);\n'
                                     '    return hs_status;\n'
                                     '}\n'
                                     '\n'
                                     'static int\n'
                                     'ble_att_svr_build_mtu_rsp'],
                                    ['    rc = ble_att_svr_build_write_rsp(rxom, &txom, &att_err);',
                                     '    rc = ble_att_svr_build_write_rsp(rxom, &txom, &att_err);\n'
                                     '    LITCH_ATT_OBSERVE(LITCH_ATT_WRITE_BUILD, conn_handle, cid, '
                                     'BLE_ATT_OP_WRITE_REQ, handle, rc);'],
                                    ['int\n'
                                     'ble_att_svr_rx_write(uint16_t conn_handle, uint16_t cid, struct '
                                     'os_mbuf **rxom)\n'
                                     '{\n'
                                     '#if !MYNEWT_VAL(BLE_ATT_SVR_WRITE)\n'
                                     '    return BLE_HS_ENOTSUP;\n'
                                     '#endif\n'
                                     '\n'
                                     '    struct ble_att_write_req *req;\n'
                                     '    struct os_mbuf *txom;\n'
                                     '    uint16_t handle;\n'
                                     '    uint8_t att_err;\n'
                                     '    int rc;\n'
                                     '\n'
                                     '    /* Initialize some values in case of early error. */\n'
                                     '    txom = NULL;\n'
                                     '    att_err = 0;\n'
                                     '    handle = 0;\n'
                                     '\n'
                                     '    rc = ble_att_svr_pullup_req_base(rxom, sizeof(*req), '
                                     '&att_err);\n'
                                     '    if (rc != 0) {\n'
                                     '        goto done;\n'
                                     '    }\n'
                                     '\n'
                                     '    req = (struct ble_att_write_req *)(*rxom)->om_data;\n'
                                     '\n'
                                     '    handle = le16toh(req->bawq_handle);\n'
                                     '\n'
                                     '#if MYNEWT_VAL(BLE_GATT_CACHING)\n'
                                     '    ble_hs_lock();\n'
                                     '    if((ble_att_svr_get_csfs(conn_handle)[0] & 1)\n'
                                     '        && ble_svc_gatt_csf_handle() != handle) {\n'
                                     '        if (!ble_att_svr_check_conn_aware(conn_handle)) {\n'
                                     '            att_err = BLE_ATT_ERR_DB_OUT_OF_SYNC;\n'
                                     '            rc = BLE_HS_EREJECT;\n'
                                     '            ble_att_svr_make_conn_aware(conn_handle);\n'
                                     '            ble_hs_unlock();\n'
                                     '            goto done;\n'
                                     '        }\n'
                                     '    }\n'
                                     '    ble_hs_unlock();\n'
                                     '#endif\n'
                                     '\n'
                                     '    /* Allocate the write response.  This must be done prior to '
                                     'processing the\n'
                                     '     * request.  See the note at the top of this file for '
                                     'details.\n'
                                     '     */\n'
                                     '    rc = ble_att_svr_build_write_rsp(rxom, &txom, &att_err);\n'
                                     '    LITCH_ATT_OBSERVE(LITCH_ATT_WRITE_BUILD, conn_handle, cid, '
                                     'BLE_ATT_OP_WRITE_REQ, handle, rc);\n'
                                     '    if (rc != 0) {\n'
                                     '        goto done;\n'
                                     '    }\n'
                                     '\n'
                                     '    /* Strip the request base from the front of the mbuf. */\n'
                                     '    os_mbuf_adj(*rxom, sizeof(*req));\n'
                                     '\n'
                                     '    rc = ble_att_svr_write_handle(conn_handle, handle, 0, rxom, '
                                     '&att_err);\n'
                                     '    if (rc != 0) {\n'
                                     '        goto done;\n'
                                     '    }\n'
                                     '\n'
                                     '    rc = 0;\n'
                                     '\n'
                                     'done:\n'
                                     '    rc = ble_att_svr_tx_rsp(conn_handle, cid, rc, txom, '
                                     'BLE_ATT_OP_WRITE_REQ,\n'
                                     '                            att_err, handle);\n'
                                     '    return rc;\n'
                                     '}\n'
                                     '\n',
                                     'int\n'
                                     'ble_att_svr_rx_write(uint16_t conn_handle, uint16_t cid, struct '
                                     'os_mbuf **rxom)\n'
                                     '{\n'
                                     '#if !MYNEWT_VAL(BLE_ATT_SVR_WRITE)\n'
                                     '    return BLE_HS_ENOTSUP;\n'
                                     '#endif\n'
                                     '\n'
                                     '    struct ble_att_write_req *req;\n'
                                     '    struct os_mbuf *txom;\n'
                                     '    uint16_t handle;\n'
                                     '    uint8_t att_err;\n'
                                     '    int rc;\n'
                                     '\n'
                                     '    /* Initialize some values in case of early error. */\n'
                                     '    txom = NULL;\n'
                                     '    att_err = 0;\n'
                                     '    handle = 0;\n'
                                     '\n'
                                     '    rc = ble_att_svr_pullup_req_base(rxom, sizeof(*req), '
                                     '&att_err);\n'
                                     '    if (rc != 0) {\n'
                                     '        goto done;\n'
                                     '    }\n'
                                     '\n'
                                     '    req = (struct ble_att_write_req *)(*rxom)->om_data;\n'
                                     '\n'
                                     '    handle = le16toh(req->bawq_handle);\n'
                                     '\n'
                                     '#if MYNEWT_VAL(BLE_GATT_CACHING)\n'
                                     '    ble_hs_lock();\n'
                                     '    if((ble_att_svr_get_csfs(conn_handle)[0] & 1)\n'
                                     '        && ble_svc_gatt_csf_handle() != handle) {\n'
                                     '        if (!ble_att_svr_check_conn_aware(conn_handle)) {\n'
                                     '            att_err = BLE_ATT_ERR_DB_OUT_OF_SYNC;\n'
                                     '            rc = BLE_HS_EREJECT;\n'
                                     '            ble_att_svr_make_conn_aware(conn_handle);\n'
                                     '            ble_hs_unlock();\n'
                                     '            goto done;\n'
                                     '        }\n'
                                     '    }\n'
                                     '    ble_hs_unlock();\n'
                                     '#endif\n'
                                     '\n'
                                     '    /* Allocate the write response.  This must be done prior to '
                                     'processing the\n'
                                     '     * request.  See the note at the top of this file for '
                                     'details.\n'
                                     '     */\n'
                                     '    rc = ble_att_svr_build_write_rsp(rxom, &txom, &att_err);\n'
                                     '    LITCH_ATT_OBSERVE(LITCH_ATT_WRITE_BUILD, conn_handle, cid, '
                                     'BLE_ATT_OP_WRITE_REQ, handle, rc);\n'
                                     '    if (rc != 0) {\n'
                                     '        goto done;\n'
                                     '    }\n'
                                     '\n'
                                     '    /* Strip the request base from the front of the mbuf. */\n'
                                     '    os_mbuf_adj(*rxom, sizeof(*req));\n'
                                     '\n'
                                     '    LITCH_ATT_OBSERVE(LITCH_ATT_WRITE_ACCESS_ENTER, conn_handle, '
                                     'cid, BLE_ATT_OP_WRITE_REQ, handle, 0);\n'
                                     '    rc = ble_att_svr_write_handle(conn_handle, handle, 0, rxom, '
                                     '&att_err);\n'
                                     '    LITCH_ATT_OBSERVE(LITCH_ATT_WRITE_ACCESS_RETURN, conn_handle, '
                                     'cid, BLE_ATT_OP_WRITE_REQ, handle, rc);\n'
                                     '    if (rc != 0) {\n'
                                     '        goto done;\n'
                                     '    }\n'
                                     '\n'
                                     '    rc = 0;\n'
                                     '\n'
                                     'done:\n'
                                     '    rc = ble_att_svr_tx_rsp(conn_handle, cid, rc, txom, '
                                     'BLE_ATT_OP_WRITE_REQ,\n'
                                     '                            att_err, handle);\n'
                                     '    return rc;\n'
                                     '}\n'
                                     '\n']]}}


ATT_SOURCE_TRANSFORMS['ble_hs_hci.c'] = {'replacements': [['#include "ble_hs_priv.h"',
                   '#include "ble_hs_priv.h"\n'
                   '#include "modules/sdk_att_witness.hpp"\n'
                   '#define LITCH_ACL_OBSERVE(phase, conn, status, length) \\\n'
                   '    LitchSdkAclObserve((phase), ((uint32_t)(conn)->bhc_flags << 16) | '
                   '(conn)->bhc_handle, \\\n'
                   '                       (status), (length), \\\n'
                   '                       ((uint32_t)(conn)->bhc_outstanding_pkts << 16) | '
                   'ble_hs_hci_avail_pkts, \\\n'
                   '                       (unsigned)ble_hs_hci_frag_num_mbufs_free())\n'
                   '#define LITCH_ACL_GLOBAL(phase, status, amount) \\\n'
                   '    LitchSdkAclObserve((phase), UINT16_MAX, (status), (amount), \\\n'
                   '                       ((uint32_t)UINT16_MAX << 16) | ble_hs_hci_avail_pkts, '
                   '\\\n'
                   '                       (unsigned)ble_hs_hci_frag_num_mbufs_free())\n'],
                  ['    ble_hs_hci_avail_pkts = max_pkts;',
                   '    ble_hs_hci_avail_pkts = max_pkts;\n'
                   '    LITCH_ACL_GLOBAL(LITCH_ACL_BUFFER_INIT, 0, pktlen);'],
                  ['        ble_hs_hci_avail_pkts += delta;',
                   '        ble_hs_hci_avail_pkts += delta;\n'
                   '        LITCH_ACL_GLOBAL(LITCH_ACL_CREDIT_ADD, 0, delta);'],
                  ['    txom = *om;\n    *om = NULL;',
                   '    txom = *om;\n'
                   '    LITCH_ACL_OBSERVE(LITCH_ACL_NOW_ENTER, conn, 0, txom != NULL ? '
                   'OS_MBUF_PKTLEN(txom) : 0);\n'
                   '    *om = NULL;'],
                  ['        if (frag == NULL) {\n'
                   '            *om = txom;\n'
                   '            return BLE_HS_EAGAIN;',
                   '        if (frag == NULL) {\n'
                   '            *om = txom;\n'
                   '            LITCH_ACL_OBSERVE(LITCH_ACL_FRAGMENT_NULL, conn, BLE_HS_EAGAIN, '
                   'OS_MBUF_PKTLEN(txom));\n'
                   '            LITCH_ACL_OBSERVE(LITCH_ACL_NOW_RETURN, conn, BLE_HS_EAGAIN, '
                   'OS_MBUF_PKTLEN(txom));\n'
                   '            return BLE_HS_EAGAIN;'],
                  ['        if (frag == NULL) {\n            rc = BLE_HS_ENOMEM;',
                   '        if (frag == NULL) {\n'
                   '            rc = BLE_HS_ENOMEM;\n'
                   '            LITCH_ACL_OBSERVE(LITCH_ACL_HEADER_NULL, conn, rc, txom != NULL ? '
                   'OS_MBUF_PKTLEN(txom) : 0);'],
                  ['        rc = ble_hs_tx_data(frag);',
                   '        unsigned litch_fragment_length = OS_MBUF_PKTLEN(frag);\n'
                   '        LITCH_ACL_OBSERVE(LITCH_ACL_TRANSPORT_ENTER, conn, 0, '
                   'litch_fragment_length);\n'
                   '        rc = ble_hs_tx_data(frag);\n'
                   '        LITCH_ACL_OBSERVE(LITCH_ACL_TRANSPORT_RETURN, conn, rc, '
                   'litch_fragment_length);'],
                  ['        ble_hs_hci_avail_pkts--;',
                   '        ble_hs_hci_avail_pkts--;\n'
                   '        LITCH_ACL_OBSERVE(LITCH_ACL_CREDIT_ACCOUNTED, conn, 0, '
                   'litch_fragment_length);'],
                  ['        *om = txom;\n        return BLE_HS_EAGAIN;',
                   '        *om = txom;\n'
                   '        LITCH_ACL_OBSERVE(LITCH_ACL_REMAINDER, conn, BLE_HS_EAGAIN, '
                   'OS_MBUF_PKTLEN(txom));\n'
                   '        LITCH_ACL_OBSERVE(LITCH_ACL_NOW_RETURN, conn, BLE_HS_EAGAIN, '
                   'OS_MBUF_PKTLEN(txom));\n'
                   '        return BLE_HS_EAGAIN;'],
                  ['    /* The entire packet was transmitted. */\n'
                   '    conn->bhc_flags &= ~BLE_HS_CONN_F_TX_FRAG;\n'
                   '\n'
                   '    return 0;',
                   '    /* The entire packet was transmitted. */\n'
                   '    conn->bhc_flags &= ~BLE_HS_CONN_F_TX_FRAG;\n'
                   '    LITCH_ACL_OBSERVE(LITCH_ACL_NOW_RETURN, conn, 0, 0);\n'
                   '\n'
                   '    return 0;'],
                  ['    conn->bhc_flags &= ~BLE_HS_CONN_F_TX_FRAG;\n    os_mbuf_free_chain(txom);',
                   '    conn->bhc_flags &= ~BLE_HS_CONN_F_TX_FRAG;\n'
                   '    LITCH_ACL_OBSERVE(LITCH_ACL_NOW_RETURN, conn, rc, txom != NULL ? '
                   'OS_MBUF_PKTLEN(txom) : 0);\n'
                   '    os_mbuf_free_chain(txom);'],
                  ['    if (STAILQ_FIRST(&conn->bhc_tx_q) != NULL) {\n'
                   '        return BLE_HS_EAGAIN;',
                   '    if (STAILQ_FIRST(&conn->bhc_tx_q) != NULL) {\n'
                   '        LITCH_ACL_OBSERVE(LITCH_ACL_QUEUE_BLOCKED, conn, BLE_HS_EAGAIN, *om != '
                   'NULL ? OS_MBUF_PKTLEN(*om) : 0);\n'
                   '        return BLE_HS_EAGAIN;']],
 'sha256': 'e6e8b48be7cb298985e4157ad8a5b0c46e70a405ea41edbb40a55bb5b1a2e4bf'}

def ObserveAttSource(name: str, source: bytes) -> bytes:
    specification = ATT_SOURCE_TRANSFORMS[name]
    if hashlib.sha256(source).hexdigest() != specification["sha256"]:
        raise ValueError("Unsupported ATT source; review the pinned SDK before building")
    for original, replacement in specification["replacements"]:
        before = original.encode("utf-8")
        if source.count(before) != 1:
            raise ValueError("Pinned ATT witness site is missing or ambiguous")
        source = source.replace(before, replacement.encode("utf-8"), 1)
    return source


def WriteObservedAttSource(packageDir: Path, buildDir: Path, name: str) -> Path:
    properties = dict(
        line.split("=", 1)
        for line in (packageDir / "library.properties").read_text().splitlines()
        if "=" in line and not line.startswith("#")
    )
    if properties.get("name") != "NimBLE-Arduino" or properties.get("version") != "2.3.7":
        raise ValueError("ATT witness requires NimBLE-Arduino 2.3.7")
    source = packageDir / "src/nimble/nimble/host/src" / name
    observed = ObserveAttSource(name, source.read_bytes())
    destination = buildDir / "litch_sdk" / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_bytes() != observed:
        destination.write_bytes(observed)
    return destination


def _ObserveAttNode(env, node):
    source = Path(node.srcnode().get_abspath())
    observed = WriteObservedAttSource(source.parents[5], Path(env.subst("$BUILD_DIR")), source.name)
    # Private quoted host headers retain their dependency-owned include directory.
    env.AppendUnique(CPPPATH=[str(source.parent), env.subst("$PROJECT_INCLUDE_DIR")])
    return env.File(str(observed))


if "Import" in globals():
    Import("env")  # type: ignore[name-defined] # noqa: F821
    if env.subst("$PIOENV") not in ("litch-esp32s3", "litch-esp32s3-panic-capture", "litch-esp32s3-host-budget"):  # type: ignore[name-defined] # noqa: F821
        raise ValueError("Repeat-pairing hardening is scoped to the S3 build")
    env.AddBuildMiddleware(  # type: ignore[name-defined] # noqa: F821
        _HardenNode, "*/NimBLE-Arduino/src/NimBLEServer.cpp"
    )
    env.AddBuildMiddleware(
        _HardenDeviceNode, "*/NimBLE-Arduino/src/NimBLEDevice.cpp"
    )
    env.AddBuildMiddleware(  # type: ignore[name-defined] # noqa: F821
        _HardenNvsNode,
        "*/NimBLE-Arduino/src/nimble/nimble/host/store/config/src/ble_store_nvs.c",
    )
    env.AddBuildMiddleware(
        _HardenConfigNode,
        "*/NimBLE-Arduino/src/nimble/nimble/host/store/config/src/ble_store_config.c",
    )
    env.AddBuildMiddleware(
        _HardenUtilNode, "*/NimBLE-Arduino/src/nimble/nimble/host/src/ble_store_util.c"
    )
    env.AddBuildMiddleware(  # type: ignore[name-defined] # noqa: F821
        _HardenGapNode, "*/NimBLE-Arduino/src/nimble/nimble/host/src/ble_gap.c"
    )
    env.AddBuildMiddleware(
        _HardenSmNode, "*/NimBLE-Arduino/src/nimble/nimble/host/src/ble_sm.c"
    )
    env.AddBuildMiddleware(
        _HardenGattsNode, "*/NimBLE-Arduino/src/nimble/nimble/host/src/ble_gatts.c"
    )
    for name in ATT_SOURCE_TRANSFORMS:
        env.AddBuildMiddleware(  # type: ignore[name-defined] # noqa: F821
            _ObserveAttNode, "*/NimBLE-Arduino/src/nimble/nimble/host/src/" + name
        )
