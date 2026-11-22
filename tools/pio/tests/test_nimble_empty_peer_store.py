"""Durable-empty reconciliation after actual failed SDK writes restore empty RAM."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile
import unittest

from test_nimble_rollover_persistence import PersistenceCorpus
from test_nimble_security_store import Function, FIXTURE, SCRIPT, hardening


def EmptyPeerStoreCorpus(*, false_ack=False):
    body = PersistenceCorpus().split('static void seed(')[0]
    host = (FIXTURE / 'acl_host/src/ble_hs.c').read_text()
    assert hashlib.sha256(host.encode()).hexdigest() == 'c5677503b264738b692d0e688672011206a308d20c5ca1aa5e32a649443bd48a'
    body = body.replace('static void ble_hs_lock(void) { ++host_locked; }',
                        '#define MYNEWT_VAL_BLE_HS_DEBUG 0\n'
                        'static int ble_hs_locked_by_cur_task(void);\n'
                        'static void ble_hs_lock_nested(void) { ++host_locked; }\n' +
                        Function(host, 'ble_hs_lock'))
    body = 'static int host_locked,require_host_lock,erase_failure;\n' + body
    marker = 'static esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *out) {'
    body = body.replace(marker, marker + '\n    if(require_host_lock) BLE_HS_DBG_ASSERT(host_locked==1);', 1)
    marker = 'static esp_err_t nvs_erase_key(nvs_handle_t h,const char *key) {'
    body = body.replace(marker, marker + '\n    if(erase_failure) { erase_failure=0; return ESP_FAIL; }', 1)
    config = hardening.HardenConfigSource((FIXTURE / 'ble_store_config.c').read_bytes()).decode()
    function = Function(config, 'LitchSdkReconcileEmptyPeerStore')
    if false_ack:
        # Controlled old admission mistake: successful clear/empty RAM is used
        # as durable proof, even though failed writes left hidden NVS records.
        function = 'int LitchSdkReconcileEmptyPeerStore(void) { return 0; }'
    return body + (FIXTURE / 'empty_peer_store_corpus.c').read_text().replace(
        '/* SDK_EMPTY_RECONCILIATION */', function)


class EmptyPeerStoreTests(unittest.TestCase):
    def test_actual_owner_reconciles_hidden_sec_and_aux_with_faults_retry_and_reopen(self):
        compiler=shutil.which('clang'); self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            for label,false_ack in [('false_ack',True),('checked',False)]:
                source=root/(label+'.c'); binary=root/label
                source.write_text(EmptyPeerStoreCorpus(false_ack=false_ack))
                build=subprocess.run([compiler,'-std=c11','-O1','-fsanitize=address,undefined',
                                      '-fno-sanitize-recover=all','-I',str(SCRIPT.parent),
                                      str(source),'-o',str(binary)],capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                cases=(28,) if false_ack else range(35)
                for case in cases:
                    with self.subTest(label=label,case=case):
                        run=subprocess.run([str(binary),str(case)],capture_output=True,text=True,timeout=5)
                        print(label,run.stdout.strip(),run.stderr.strip())
                        self.assertEqual(run.returncode,1 if false_ack else 0,run.stdout+run.stderr)
                        if not false_ack: self.assertEqual(run.stderr,'')
                        else: self.assertIn('peer_disk_empty()',run.stderr)


if __name__=='__main__':
    unittest.main()
