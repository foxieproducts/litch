"""Production startup and store callbacks; controlled NVS, not radio/reset proof."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_nimble_security_store import Corpus, Function, FIXTURE, SCRIPT, hardening


CALLBACK_STUBS = r'''
#define ESP_PLATFORM 1
#define SYSINIT_ASSERT_ACTIVE() ((void)0)
extern int litch_nimble_store_restore_status;
static struct {
    int (*store_read_cb)(int,const union ble_store_key *,union ble_store_value *);
    int (*store_write_cb)(int,const union ble_store_value *);
    int (*store_delete_cb)(int,const union ble_store_key *);
} ble_hs_cfg;
'''
for kind in ('cccd', 'csfc', 'local_irk', 'rpa_rec'):
    CALLBACK_STUBS += f'static int ble_store_config_read_{kind}(const void *k,void *v) {{ return BLE_HS_ENOTSUP; }}\n'
    CALLBACK_STUBS += f'static int ble_store_config_write_{kind}(const void *v) {{ return BLE_HS_ENOTSUP; }}\n'
    CALLBACK_STUBS += f'static int ble_store_config_delete_{kind}(const void *k) {{ return BLE_HS_ENOTSUP; }}\n'

MAIN = r'''
static void seed(int table,int slot,const void *record,size_t size) {
    disk[table][slot].present=true; disk[table][slot].size=size;
    memcpy(disk[table][slot].data,record,size);
}
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    struct ble_store_value_sec a=value(1,17),b=value(2,34),c=value(3,51);
    a.bond_count=7; b.bond_count=2; c.bond_count=12;
    union ble_store_key key={0}; union ble_store_value out={0},incoming={0};
    key.sec.peer_addr=a.peer_addr; incoming.sec=c;
    if(which==0) absent_namespace=1;
    else {
        seed(0,0,&a,sizeof a); seed(1,2,&a,sizeof a);
        if(which==1 || which==10) seed(0,2,&b,sizeof b);
        if(which==10) seed(0,1,&c,sizeof c);
        if(which==2) disk[0][0].size=sizeof a-1;
        if(which==3) { disk[0][0].size=sizeof a+8; memset(disk[0][0].data,42,disk[0][0].size); }
        if(which==4) read_fault_at=3; /* after first OUR record */
        if(which==5) read_fault_at=5; /* after complete OUR inventory */
        if(which==6) {
            struct ble_store_value_csfc v={0}; v.peer_addr=a.peer_addr;
            seed(3,0,&v,sizeof v); disk[3][0].size=sizeof v-1;
        }
        if(which==7) seed(0,2,&a,sizeof a); /* duplicate identity */
        if(which==8) open_fault=1;
    }
    struct blob before[6][8]; memcpy(before,disk,sizeof before);
    ble_store_config_init();
    int rc=ble_hs_cfg.store_read_cb(BLE_STORE_OBJ_TYPE_OUR_SEC,&key,&out);
    printf("startup case%d lookup%d counts%d/%d counters%u/%u writes%d\n",which,rc,
        ble_store_config_num_our_secs,ble_store_config_num_peer_secs,
        ble_store_config_our_bond_count,ble_store_config_peer_bond_count,writes);
    if(which>=2 && which<=8) {
        CHECK(rc==BLE_HS_ESTORE_FAIL);
        CHECK(ble_store_config_num_our_secs==0 && ble_store_config_num_peer_secs==0);
        CHECK(ble_store_config_our_bond_count==0 && ble_store_config_peer_bond_count==0);
        CHECK(ble_hs_cfg.store_write_cb(BLE_STORE_OBJ_TYPE_OUR_SEC,&incoming)==BLE_HS_ESTORE_FAIL);
        CHECK(ble_hs_cfg.store_delete_cb(BLE_STORE_OBJ_TYPE_OUR_SEC,&key)==BLE_HS_ESTORE_FAIL);
        CHECK(writes==0 && commits==0 && !memcmp(before,disk,sizeof before));
        /* Error must survive ordinary reads; only complete re-init recovers. */
        CHECK(ble_hs_cfg.store_read_cb(BLE_STORE_OBJ_TYPE_OUR_SEC,&key,&out)==BLE_HS_ESTORE_FAIL);
        memset(disk,0,sizeof disk); seed(0,0,&a,sizeof a); seed(1,2,&a,sizeof a);
        read_fault_at=0; ble_store_config_init();
        CHECK(ble_hs_cfg.store_read_cb(BLE_STORE_OBJ_TYPE_OUR_SEC,&key,&out)==0);
        CHECK(!memcmp(&out.sec,&a,sizeof a));
    } else if(which==0) {
        CHECK(rc==BLE_HS_ENOENT); CHECK(ble_store_config_num_our_secs==0 && ble_store_config_num_peer_secs==0);
    } else {
        CHECK(rc==0 && !memcmp(&out.sec,&a,sizeof a));
        CHECK(ble_store_config_num_our_secs==(which==10?3:2));
        CHECK(ble_store_config_num_peer_secs==1);
        CHECK(ble_store_config_our_bond_count==(which==10?12:7) && ble_store_config_peer_bond_count==7);
        CHECK(writes==0 && commits==0 && !memcmp(before,disk,sizeof before));
        ble_store_config_init(); CHECK(ble_store_config_num_our_secs==(which==10?3:2));
    }
    CHECK(opened==closed); return 0;
}
'''


def StartupCorpus(config, nvs):
    # The existing corpus owns the NVS leaf and semantic host types. Public
    # startup/callback bodies are copied from the pinned/generated source too.
    util = (FIXTURE / 'ble_store_util.c').read_text()
    body = Corpus(config, nvs, util, True).split('int main(')[0]
    callbacks = '\n'.join(Function(config, name) for name in (
        'ble_store_config_read', 'ble_store_config_write', 'ble_store_config_delete'))
    return (body + CALLBACK_STUBS + callbacks + '\n' +
            Function(nvs, 'ble_store_config_conf_init') + '\n' +
            Function(config, 'ble_store_config_init') + MAIN)


class StartupReadinessTests(unittest.TestCase):
    def test_startup_inventory_is_complete_or_callbacks_keep_storage_failure(self):
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler)
        raw_config = (FIXTURE / 'ble_store_config.c').read_bytes()
        raw_nvs = (FIXTURE / 'ble_store_nvs.c').read_bytes()
        old_nvs = raw_nvs.replace(hardening.NVS_ORIGINAL_COUNTERS.encode(),
                                  hardening.NVS_HARDENED_COUNTERS.encode())
        variants = [('old', raw_config.decode(), old_nvs.decode()),
                    ('new', hardening.HardenConfigSource(raw_config).decode(),
                     hardening.HardenNvsSource(raw_nvs).decode())]
        old_failures = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for label, config, nvs in variants:
                source = root / (label + '.c')
                binary = root / label
                source.write_text(StartupCorpus(config, nvs))
                build = subprocess.run([compiler, '-std=c11', '-O1', '-fsanitize=address,undefined',
                                        '-fno-sanitize-recover=all', '-I', str(SCRIPT.parent),
                                        str(source), '-o', str(binary)], capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                for case in (0,1,2,3,4,5,6,7,8,10):
                    with self.subTest(label=label, case=case):
                        result = subprocess.run([str(binary), str(case)], capture_output=True, text=True)
                        print(label.upper(), result.stdout.strip(), result.stderr.splitlines()[:1])
                        if label == 'old':
                            if result.returncode:
                                old_failures.append(case)
                        else:
                            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                            self.assertEqual(result.stderr, '')
        self.assertEqual(old_failures, [2,3,4,5,6,7,8])


if __name__ == '__main__':
    unittest.main()
