"""Pinned SMP admission/results through real store/NVS code; controlled RF/crypto."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_nimble_rollover_persistence import PersistenceCorpus
from test_nimble_security_store import Function, FIXTURE, SCRIPT, hardening


def PairingCorpus(sm):
    body = PersistenceCorpus().split('static void seed(')[0]
    body = body.replace('    int (*store_read_cb)',
                        '    unsigned sm_sc_only;\n    int (*store_read_cb)', 1)
    host = (FIXTURE / 'acl_host/src/ble_hs.c').read_text()
    assert hashlib.sha256(host.encode()).hexdigest() == 'c5677503b264738b692d0e688672011206a308d20c5ca1aa5e32a649443bd48a'
    body = body.replace('static void ble_hs_lock(void) { ++host_locked; }',
                        '#define MYNEWT_VAL_BLE_HS_DEBUG 0\n'
                        'static int ble_hs_locked_by_cur_task(void);\n'
                        'static void ble_hs_lock_nested(void) { ++host_locked; }\n' +
                        Function(host, 'ble_hs_lock'))
    store = (FIXTURE / 'ble_store.c').read_text()
    functions = '\n'.join(Function(store, name) for name in (
        'ble_store_read', 'ble_store_read_our_sec', 'ble_store_persist_sec',
        'ble_store_write_our_sec', 'ble_store_write_peer_sec'))
    if 'litch_sm_pairing_admission(' in sm:
        functions += ('\nextern int LitchSdkPairingAdmission(uint16_t) __attribute__((weak));\n' +
                      Function(sm, 'litch_sm_pairing_admission'))
    functions += '\n' + '\n'.join(Function(sm, name) for name in (
        'ble_sm_update_sec_state', 'ble_sm_fill_store_value', 'ble_sm_persist_keys',
        'ble_sm_process_result', 'ble_sm_pair_req_rx', 'ble_sm_ltk_restore_exec',
        'ble_sm_retrieve_ltk', 'ble_sm_ltk_req_rx', 'ble_sm_enc_event_rx',
        'ble_sm_key_exch_success', 'ble_sm_rx'))
    return body + (FIXTURE / 'pairing_admission_corpus.c').read_text().replace(
        '/* SDK_SMP_FUNCTIONS */', functions)


class PairingAdmissionTests(unittest.TestCase):
    def test_guarded_copy_pins_source_and_preserves_restore(self):
        original = (FIXTURE / 'ble_sm.c').read_bytes()
        self.assertEqual(hashlib.sha256(original).hexdigest(), hardening.SM_ORIGINAL_SHA256)
        patched = hardening.HardenSmSource(original)
        before = original
        after = patched.replace(hardening.SM_ADMISSION_HOOK.encode(), b'', 1)
        for name in ('ble_sm_pair_req_rx', 'ble_sm_persist_keys', 'ble_sm_process_result'):
            before = before.replace(Function(original.decode(), name).encode(), b'<changed>', 1)
            after = after.replace(Function(patched.decode(), name).encode(), b'<changed>', 1)
        self.assertEqual(before, after)
        # The complete restore path remains byte-for-byte the pinned SDK path.
        for name in ('ble_sm_ltk_req_rx', 'ble_sm_ltk_restore_exec',
                     'ble_sm_retrieve_ltk', 'ble_sm_enc_event_rx'):
            self.assertEqual(Function(original.decode(), name), Function(patched.decode(), name))
        for drift in (b'', original+b'\n', original.replace(b'ble_store_write_our_sec', b'changed')):
            with self.assertRaises(ValueError): hardening.HardenSmSource(drift)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); package = root / 'NimBLE-Arduino'
            source = package / 'src/nimble/nimble/host/src/ble_sm.c'
            source.parent.mkdir(parents=True); source.write_bytes(original)
            shutil.copy2(FIXTURE / 'library.properties', package)
            class Node:
                def __init__(self, path, origin=None): self.path,self.origin=path,origin
                def srcnode(self): return self.origin or self
                def get_abspath(self): return str(self.path)
            class Environment:
                includes = []
                def subst(self, name):
                    selfCase.assertEqual(name, '$BUILD_DIR'); return str(root / 'build')
                def AppendUnique(self, **values): self.includes.extend(values['CPPPATH'])
                def File(self, path): return Node(Path(path))
            selfCase = self
            env = Environment(); variant = root / 'build/library/ble_sm.c'
            result = hardening._HardenSmNode(env, Node(variant, Node(source)))
            self.assertEqual(result.path, root / 'build/litch_sdk/ble_sm.c')
            self.assertEqual(result.path.read_bytes(), patched)
            self.assertEqual(env.includes, [str(source.parent)])
            stamp = result.path.stat().st_mtime_ns
            hardening.WriteHardenedSmSource(package, root / 'build')
            self.assertEqual(result.path.stat().st_mtime_ns, stamp)
            self.assertEqual(source.read_bytes(), original); self.assertFalse(variant.exists())
            (package / 'library.properties').write_text('name=NimBLE-Arduino\nversion=2.3.8\n')
            with self.assertRaises(ValueError): hardening.WriteHardenedSmSource(package, root / 'unsupported')
            self.assertFalse((root / 'unsupported').exists())
        registrations = []
        class MiddlewareEnvironment:
            def subst(self, name): return 'litch-esp32s3'
            def AddBuildMiddleware(self, callback, pattern): registrations.append((callback.__name__,pattern))
        exec(compile(SCRIPT.read_text(), str(SCRIPT), 'exec'),
             {'Import': lambda name: None, 'env': MiddlewareEnvironment()})
        self.assertEqual([pattern for name,pattern in registrations if name=='_HardenSmNode'],
                         ['*/NimBLE-Arduino/src/nimble/nimble/host/src/ble_sm.c'])

    def test_actual_request_persistence_faults_revocation_and_quiet_restore(self):
        compiler = shutil.which('clang'); self.assertIsNotNone(compiler)
        original = (FIXTURE / 'ble_sm.c').read_bytes()
        hardened = hardening.HardenSmSource(original)
        names = ('unsolicited_justworks', 'wrong_connection', 'retired_session',
                 'exact_lease', 'short_request', 'both_native_tables_committed',
                 'our_commit_before_visibility', 'peer_commit_before_visibility',
                 'our_commit_after_visibility', 'peer_commit_after_visibility',
                 'our_capacity', 'peer_capacity', 'revoked_before_persist',
                 'revoked_in_identity_callback', 'quiet_owner_restore',
                 'denial_precedes_handler_security_proc_mutation',
                 'l2cap_unsolicited_justworks', 'l2cap_exact_lease',
                 'rpa_write_failure_after_keys', 'direct_identity_needs_no_rpa_mapping',
                 'rpa_capacity_failure_after_keys')
        old_failures = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for label,sm in [('old',original), ('new',hardened)]:
                source = root / (label+'.c'); binary = root / label
                source.write_text(PairingCorpus(sm.decode()))
                build = subprocess.run([compiler, '-std=c11', '-O1', '-DWITH_ADMISSION_HOOK=1',
                                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                                        '-I', str(SCRIPT.parent), str(source), '-o', str(binary)],
                                       capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                for case,name in enumerate(names):
                    with self.subTest(label=label, journey=name):
                        result = subprocess.run([str(binary),str(case)], capture_output=True,text=True,timeout=5)
                        print(label.upper(), name, result.stdout.strip(), result.stderr.strip())
                        if label=='old':
                            if result.returncode: old_failures.append(case)
                        else:
                            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                            self.assertEqual(result.stderr, '')
            # Link without any runtime hook: existing feature-off request and
            # existing native-bond reconnect still use their original paths.
            source = root / 'new.c'; binary = root / 'absent_hook'
            # Darwin ld requires an explicit permitted undefined weak import;
            # ELF targets (including S3) accept the weak declaration directly.
            weak_link = ['-Wl,-U,_LitchSdkPairingAdmission'] if sys.platform=='darwin' else []
            build = subprocess.run([compiler, '-std=c11', '-O1', '-DWITH_ADMISSION_HOOK=0',
                                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                                    '-I', str(SCRIPT.parent), str(source), '-o', str(binary)] + weak_link,
                                   capture_output=True,text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            for case in (3,5,14):
                result = subprocess.run([str(binary),str(case)],capture_output=True,text=True,timeout=5)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertEqual(result.stderr, '')
        self.assertEqual(old_failures, [0,1,2,5,6,7,8,9,10,11,12,13,15,16,18,19,20])


if __name__ == '__main__':
    unittest.main()
