"""Actual pinned GATT -> accepted store/NVS owner; controlled host/RF leaves."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile
import unittest

from test_nimble_rollover_persistence import PersistenceCorpus
from test_nimble_security_store import Function, FIXTURE, SCRIPT, hardening


def CsfcCorpus(gatts, strict_lock=True):
    body = PersistenceCorpus().split('static void seed(')[0]
    # Semantic host types gain the production field names used by real GATT.
    body = body.replace('uint8_t data[1];', 'uint8_t csfc[1];')
    body = body.replace('uint16_t chr_val_handle,flags; };',
                        'uint16_t chr_val_handle,flags; uint8_t value_changed; };', 1)
    body = body.replace(Function(body, 'ble_store_delete'),
                        'static int ble_store_delete(int,const union ble_store_key *);', 1)
    unlock = 'static void ble_hs_unlock(void) { --host_locked; }'
    body = body.replace(unlock, 'static void OnHostUnlocked(void);\n'
                        'static void ble_hs_unlock(void) { if (--host_locked==0) OnHostUnlocked(); }')
    if strict_lock:
        host = (FIXTURE / 'acl_host/src/ble_hs.c').read_text()
        assert hashlib.sha256(host.encode()).hexdigest() == 'c5677503b264738b692d0e688672011206a308d20c5ca1aa5e32a649443bd48a'
        lock = Function(host, 'ble_hs_lock')
        body = body.replace('static void ble_hs_lock(void) { ++host_locked; }',
                            '#define MYNEWT_VAL_BLE_HS_DEBUG 0\n'
                            'static int ble_hs_locked_by_cur_task(void);\n'
                            'static void ble_hs_lock_nested(void) { ++host_locked; }\n' + lock)
    store = (FIXTURE / 'ble_store.c').read_text()
    body += '\n' + '\n'.join(Function(store, name) for name in (
        'ble_store_delete', 'ble_store_read', 'ble_store_read_csfc',
        'ble_store_write_csfc', 'ble_store_delete_csfc', 'ble_store_read_cccd',
        'ble_store_write_cccd'))
    service = (FIXTURE / 'ble_svc_gatt.c').read_text()
    # Skip the earlier prototype; the full definition must own the extraction.
    service = service[service.rindex('static int\nble_svc_gatt_cl_sup_feat_access('):]
    functions = '\n'.join(Function(gatts, name) for name in (
        'ble_gatts_peer_cl_sup_feat_get', 'ble_gatts_peer_cl_sup_feat_update',
        'ble_gatts_bonding_restored'))
    functions += '\n' + Function(service, 'ble_svc_gatt_cl_sup_feat_access')
    # The peer-address table is empty in this journey. Let the actual SDK
    # delete-peer walk observe its native absent result at that controlled leaf.
    for action, args in [('read', 'const void *k,void *v'), ('delete', 'const void *k')]:
        body = body.replace(f'static int ble_store_config_{action}_rpa_rec({args}) {{ return BLE_HS_ENOTSUP; }}',
                            f'static int ble_store_config_{action}_rpa_rec({args}) {{ return BLE_HS_ENOENT; }}')
    return body + f'\n#define CSFC_STRICT_LOCK {int(strict_lock)}\n' + (FIXTURE / 'csfc_gatt_corpus.c').read_text().replace('/* SDK_GATT_FUNCTIONS */', functions)


class CsfcPersistenceTests(unittest.TestCase):
    def test_guarded_single_copy_preserves_sdk_and_private_headers(self):
        original = (FIXTURE / 'ble_gatts.c').read_bytes()
        self.assertEqual(hashlib.sha256(original).hexdigest(), hardening.GATTS_ORIGINAL_SHA256)
        self.assertEqual(hashlib.sha256((FIXTURE / 'ble_svc_gatt.c').read_bytes()).hexdigest(),
                         'f77468fee46cc2d497e1461506b61a20ccb789d722a0380e41cc7f1eb87a3d28')
        hardened = hardening.HardenGattsSource(original)
        expected = original
        for name in ('ble_gatts_peer_cl_sup_feat_update', 'ble_gatts_clt_cfg_access_locked',
                     'ble_gatts_clt_cfg_access'):
            before = Function(original.decode(), name)
            after = Function(hardened.decode(), name)
            expected = expected.replace(before.encode(), after.encode(), 1)
        self.assertEqual(hardened, expected)
        for drift in (b'', original+b'\n', original.replace(b'ble_store_write_csfc(&value_csfc)', b'changed')):
            with self.assertRaises(ValueError): hardening.HardenGattsSource(drift)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); package = root / 'NimBLE-Arduino'
            source = package / 'src/nimble/nimble/host/src/ble_gatts.c'
            source.parent.mkdir(parents=True); source.write_bytes(original)
            shutil.copy2(FIXTURE / 'library.properties', package)
            class Node:
                def __init__(self, path, origin=None): self.path, self.origin = path, origin
                def srcnode(self): return self.origin or self
                def get_abspath(self): return str(self.path)
            class Environment:
                includes = []
                def subst(self, value):
                    assert value == '$BUILD_DIR'; return str(root / 'build')
                def AppendUnique(self, **values): self.includes.extend(values['CPPPATH'])
                def File(self, path): return Node(Path(path))
            env = Environment(); variant = root / 'build/library/ble_gatts.c'
            result = hardening._HardenGattsNode(env, Node(variant, Node(source)))
            self.assertEqual(result.path, root / 'build/litch_sdk/ble_gatts.c')
            self.assertEqual(result.path.read_bytes(), hardened)
            self.assertEqual(env.includes, [str(source.parent)])
            stamp = result.path.stat().st_mtime_ns
            hardening.WriteHardenedGattsSource(package, root / 'build')
            self.assertEqual(result.path.stat().st_mtime_ns, stamp)
            self.assertEqual(source.read_bytes(), original); self.assertFalse(variant.exists())
            (package / 'library.properties').write_text('name=NimBLE-Arduino\nversion=2.3.8\n')
            with self.assertRaises(ValueError): hardening.WriteHardenedGattsSource(package, root / 'unsupported')
            self.assertFalse((root / 'unsupported').exists())

    def test_actual_gatt_update_failure_retry_reconnect_and_reopen(self):
        compiler = shutil.which('clang'); self.assertIsNotNone(compiler)
        original = (FIXTURE / 'ble_gatts.c').read_bytes()
        old_failures = []
        journeys = ("normal", "append_before_visibility", "append_after_visibility",
                    "commit_before_visibility", "commit_after_visibility_reopen",
                    "missing_record", "identical_value", "capacity", "missing_callback",
                    "missing_connection", "queued_disconnect", "queued_handle_reuse",
                    "queued_clear")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            hardened = hardening.HardenGattsSource(original)
            mapping = (b"            rc = rc == BLE_HS_ESTORE_CAP ? BLE_ATT_ERR_INSUFFICIENT_RES\n"
                       b"                                        : BLE_ATT_ERR_UNLIKELY;")
            self.assertEqual(hardened.count(mapping), 1)
            # Negative control: retain real NVS failure/RAM rollback, but lie
            # to ATT. The four fault journeys must reject this false success.
            swallowed = hardened.replace(mapping, b"            rc = 0;", 1)
            variants = [('old', original, False, 7),
                        ('new', hardened, True, 13),
                        ('old_lock', original, True, 1),
                        ('swallowed_error', swallowed, True, 5)]
            for label, source, strict_lock, count in variants:
                path=root/(label+'.c'); binary=root/label
                path.write_text(CsfcCorpus(source.decode(), strict_lock))
                build=subprocess.run([compiler,'-std=c11','-O1','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all','-I',str(SCRIPT.parent),str(path),'-o',str(binary)],
                    capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                for case in range(count):
                    result=subprocess.run([str(binary),str(case)],capture_output=True,text=True,timeout=5)
                    print(label,journeys[case],'exit',result.returncode,result.stdout.strip(),result.stderr.strip())
                    if label=='new':
                        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                        self.assertEqual(result.stderr,'')
                    elif label=='old_lock': self.assertEqual(result.returncode,-6)
                    elif label=='swallowed_error':
                        self.assertEqual(result.returncode, 0 if case==0 else 1,
                                         result.stdout+result.stderr)
                        if case:
                            self.assertIn('failed==expected_status',result.stderr)
                    elif result.returncode: old_failures.append(case)
            self.assertEqual(old_failures,list(range(1,7)))


if __name__=='__main__':
    unittest.main()
