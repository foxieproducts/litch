"""Actual guarded SDK/NVS Add rollback; only NVS/HCI/task leaves controlled."""
from pathlib import Path
import configparser
import shutil
import subprocess
import tempfile
import unittest

from test_nimble_rollover_persistence import PersistenceCorpus
from test_nimble_security_store import Function, FIXTURE, SCRIPT, hardening


def AddCorpus(*, destructive=False, count_only=False):
    body = PersistenceCorpus().split('static void seed(')[0]
    # Match the actual S3 six subscriptions per saved phone; retain the existing
    # faultable NVS implementation, function order, and native public writers.
    body = body.replace('MYNEWT_VAL_BLE_STORE_MAX_CCCDS 8', 'MYNEWT_VAL_BLE_STORE_MAX_CCCDS 18')
    body = body.replace('ble_store_config_cccds[8]', 'ble_store_config_cccds[18]')
    body = body.replace('[6][8]', '[6][18]').replace('index<8', 'index<18')
    config = hardening.HardenConfigSource((FIXTURE / 'ble_store_config.c').read_bytes()).decode()
    nvs = (FIXTURE / 'ble_store_nvs.c').read_bytes()
    if not count_only:
        nvs = hardening.HardenNvsSource(nvs)
    body = 'int ble_store_config_persist_rpa_recs(void);\n' + body
    stub = 'static int ble_store_config_write_rpa_rec(const void *v) { return BLE_HS_ENOTSUP; }'
    assert body.count(stub) == 1
    body = body.replace(stub, Function(config, 'ble_store_config_write_rpa_rec'))
    body += '\n' + Function(nvs.decode(), 'ble_store_config_persist_rpa_recs')
    body += r'''
static int reset_error, add_error, add_calls;
int LitchSdkResetPeerPrivacy(void) { return reset_error; }
int ble_hs_pvcy_add_entry(const uint8_t *a, uint8_t t, const uint8_t *key) {
    (void)a; (void)t; (void)key; ++add_calls; return add_error;
}
'''
    store = (FIXTURE / 'ble_store.c').read_text()
    body = '#define BLE_ADDR_PUBLIC 0\n#define BLE_ADDR_RANDOM 1\n#define BLE_ADDR_RANDOM_ID 3\n' + body
    body += '\n'.join(Function(store, name) for name in (
        'ble_store_persist_sec', 'ble_store_write_our_sec',
        'ble_store_write_peer_sec', 'ble_store_write_rpa_rec'))
    helper = SCRIPT.with_name('litch_nimble_add_rollback.inc').read_text()
    if destructive:
        helper = helper.replace('LitchSdkRollbackAddedPeers(', 'checkedRollback(')
        body += Function(config, 'LitchSdkClearPeerStore') + '\n'
        helper += r'''
int LitchSdkRollbackAddedPeers(const ble_addr_t *s,int n,
    const struct ble_store_value_rpa_rec *r,int c) {
    (void)s;(void)n;(void)r;(void)c;return LitchSdkClearPeerStore();
}
'''
    return body + '\n' + helper + '\n' + MAIN


MAIN = r'''
static void seed_peer(int id) {
    union ble_store_value v={0}; v.sec=value(id,17+id); v.sec.irk_present=1;
    if(ble_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&v) ||
       ble_store_write(BLE_STORE_OBJ_TYPE_PEER_SEC,&v)) abort();
    for(int h=0;h<6;++h) {
        memset(&v,0,sizeof v);v.cccd.peer_addr=value(id,0).peer_addr;
        v.cccd.chr_val_handle=100+h;v.cccd.flags=1;
        if(ble_store_write(BLE_STORE_OBJ_TYPE_CCCD,&v)) abort();
    }
    memset(&v,0,sizeof v);v.csfc.peer_addr=value(id,0).peer_addr;v.csfc.data[0]=1;
    if(ble_store_write(BLE_STORE_OBJ_TYPE_CSFC,&v)) abort();
}
static int has_disk(int table,const void *record,size_t size) {
    for(int i=0;i<18;++i) if(disk[table][i].present && disk[table][i].size==size &&
        !memcmp(disk[table][i].data,record,size)) return 1;
    return 0;
}
static int third_phone_rpa_capacity(int which,const struct ble_store_value_sec keys[2][2]) {
    int duplicate=which>=35 && which<=40;
    int failed_save=which>=37 && which<=39;
    int update_alias=which==40;
    int capacity_refusal=which==33 || which==41;
    struct ble_store_value_rpa_rec before[3]={0};
    int baseline_count=which==34?2:3;
    for(int i=0;i<baseline_count;++i) {
        before[i].peer_rpa_addr=value(81+i,0).peer_addr;
        before[i].peer_addr=value(i==2?(duplicate || which==41?1:99):i+1,0).peer_addr;
        if(i==2 && which==41) before[i].peer_addr.type^=1;
        if(i==2 && duplicate && which!=35) before[i].peer_addr.type|=2;
    }
    /* Historical duplicate mappings must survive native reopen before Add.
     * Seed through the actual checked table persistence owner, not the upsert. */
    memcpy(ble_store_config_rpa_recs,before,sizeof before);
    ble_store_config_num_rpa_recs=baseline_count;
    CHECK(ble_store_config_persist_rpa_recs()==0);
    ble_store_config_init();CHECK(litch_nimble_store_restore_status==0);
    CHECK(ble_store_config_num_rpa_recs==baseline_count &&
          !memcmp(ble_store_config_rpa_recs,before,baseline_count*sizeof *before));
    struct ble_store_value_sec incoming=value(3,20);incoming.irk_present=1;
    int our_rc=update_alias?0:ble_store_write_our_sec(&incoming);
    int peer_rc=update_alias?0:ble_store_write_peer_sec(&incoming);
    union ble_store_value added={0};
    added.rpa_rec.peer_addr=incoming.peer_addr;added.rpa_rec.peer_rpa_addr=value(84,0).peer_addr;
    if(update_alias) added.rpa_rec.peer_addr=before[2].peer_addr;
    if(which==39) set_failure=2; /* new mapping reaches flash, then reports failure */
    else if(failed_save) fault(which-36,6);
    int rpa_rc=ble_store_write_rpa_rec(&added.rpa_rec);
    CHECK(our_rc==0 && peer_rc==0 && ble_store_config_num_our_secs==(update_alias?2:3) &&
          ble_store_config_num_peer_secs==(update_alias?2:3) && ble_store_config_num_rpa_recs==3);
    CHECK(rpa_rc==(capacity_refusal?BLE_HS_ESTORE_CAP:failed_save?BLE_HS_ESTORE_FAIL:0));
    if(capacity_refusal || failed_save) CHECK(!memcmp(ble_store_config_rpa_recs,before,sizeof before));
    else {
        CHECK(has_disk(5,&added.rpa_rec,sizeof added.rpa_rec));
        if(duplicate) {
            int changed=update_alias?0:2;
            for(int i=0;i<3;++i) if(i!=changed)
                CHECK(!memcmp(&ble_store_config_rpa_recs[i],&before[i],sizeof before[i]));
            CHECK(!memcmp(&ble_store_config_rpa_recs[changed],&added.rpa_rec,sizeof added.rpa_rec));
        }
    }
    /* Both new key tables reached durable media before the RPA refusal. */
    if(!update_alias) CHECK(has_disk(0,&ble_store_config_our_secs[2],sizeof incoming) &&
                           has_disk(1,&ble_store_config_peer_secs[2],sizeof incoming));
    for(int t=0;t<2;++t) for(int i=0;i<2;++i)
        CHECK(has_disk(t,&keys[t][i],sizeof keys[t][i]));
    /* A failed commit may already have changed flash. The retained Adding
     * baseline must restore every raw duplicate mapping after native reopen. */
    if(failed_save) { ble_store_config_init();CHECK(litch_nimble_store_restore_status==0); }
    ble_addr_t saved[2]={value(1,0).peer_addr,value(2,0).peer_addr};
    CHECK(LitchSdkRollbackAddedPeers(saved,2,before,baseline_count)==0);
    ble_store_config_init();CHECK(litch_nimble_store_restore_status==0);
    CHECK(ble_store_config_num_our_secs==2 && ble_store_config_num_peer_secs==2 &&
          ble_store_config_num_rpa_recs==baseline_count &&
          !memcmp(ble_store_config_rpa_recs,before,baseline_count*sizeof *before));
    for(int t=0;t<2;++t) for(int i=0;i<2;++i)
        CHECK(has_disk(t,&keys[t][i],sizeof keys[t][i]));
    CHECK(opened==closed && host_locked==0 &&
          unpair_oldest_requests==0 && unpair_except_requests==0);
    printf("thirdPhone case%d baselineSecurity2 baselineRPA%d ourRC%d peerRC%d rpaRC%d duplicate%d alias%d retainedKeys1 rollbackReopen1\n",
           which,baseline_count,our_rc,peer_rc,rpa_rc,duplicate,update_alias);
    return 0;
}
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    ble_store_config_init();ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    seed_peer(1);seed_peer(2);
    struct ble_store_value_sec keys[2][2];
    memcpy(keys[0],ble_store_config_our_secs,sizeof keys[0]);
    memcpy(keys[1],ble_store_config_peer_secs,sizeof keys[1]);
    if(which>=33 && which<=41) return third_phone_rpa_capacity(which,keys);
    struct ble_store_value_rpa_rec before[3]={0};
    before[0].peer_rpa_addr=value(81,0).peer_addr;before[0].peer_addr=value(1,0).peer_addr;
    before[1].peer_rpa_addr=value(82,0).peer_addr;before[1].peer_addr=value(99,0).peer_addr;
    memcpy(ble_store_config_rpa_recs,before,sizeof before);ble_store_config_num_rpa_recs=2;
    CHECK(ble_store_config_persist_rpa_recs()==0);
    /* Deliberately unrelated orphan RPA + local IRK must survive too. */
    struct ble_store_value_local_irk local={0};local.irk[0]=77;local.addr=value(90,0).peer_addr;
    disk[4][0].present=true;disk[4][0].size=sizeof local;memcpy(disk[4][0].data,&local,sizeof local);
    struct ble_store_value_rpa_rec snapshot[3];int snapshot_count=-1;
    CHECK(LitchSdkReadAddRpaSnapshot(snapshot,&snapshot_count,3)==0 && snapshot_count==2 &&
          !memcmp(snapshot,before,2*sizeof *before));
    ble_addr_t saved[2]={value(1,0).peer_addr,value(2,0).peer_addr};
    seed_peer(3);
    /* Both same-key metadata replacement and new-entry rollback are required. */
    ble_store_config_rpa_recs[0].peer_addr=value(3,0).peer_addr;
    ble_store_config_rpa_recs[2].peer_rpa_addr=value(83,0).peer_addr;
    ble_store_config_rpa_recs[2].peer_addr=value(3,0).peer_addr;ble_store_config_num_rpa_recs=3;
    CHECK(ble_store_config_persist_rpa_recs()==0);
    if(which>=30) {
        int prior=writes;
        if(which==30) saved[1]=saved[0];
        if(which==31) saved[1]=value(98,0).peer_addr;
        if(which==32) litch_nimble_store_restore_status=BLE_HS_ESTORE_FAIL;
        CHECK(LitchSdkRollbackAddedPeers(saved,2,before,2)!=0 && writes==prior);
        CHECK(has_disk(0,&keys[0][0],sizeof keys[0][0]) &&
              has_disk(0,&keys[0][1],sizeof keys[0][1]));
        printf("addGuard case%d preserved1 noIO1\n",which);return 0;
    }
    if(which>=1 && which<=25) {
        int table=(which-1)/5,mode=(which-1)%5;
        const int native_type[]={1,2,3,4,6};
        if(mode<3) fault(mode==0?BEFORE:mode==1?AFTER:DROP,native_type[table]);
        if(mode==3) open_fault=1;
        if(mode==4) read_fault=1;
    }
    int rc=LitchSdkRollbackAddedPeers(saved,2,before,2);
    if(which>=1 && which<=25) {
        CHECK(rc!=0);read_fault=0;open_fault=0;fault_mode=0;
        /* Reopen native media with the durable Adding journal still retained. */
        ble_store_config_init();CHECK(litch_nimble_store_restore_status==0);
        CHECK(LitchSdkRollbackAddedPeers(saved,2,before,2)==0);
    } else CHECK(rc==0);
    for(int t=0;t<2;++t) for(int i=0;i<2;++i)
        CHECK(has_disk(t,&keys[t][i],sizeof keys[t][i]));
    CHECK(ble_store_config_num_our_secs==2 && ble_store_config_num_peer_secs==2 &&
          ble_store_config_num_cccds==12 && ble_store_config_num_csfcs==2);
    CHECK(has_disk(4,&local,sizeof local));
    ble_store_config_init();CHECK(litch_nimble_store_restore_status==0);
    CHECK(ble_store_config_num_rpa_recs==2 &&
          !memcmp(ble_store_config_rpa_recs,before,2*sizeof *before));
    CHECK(LitchSdkRollbackAddedPeers(saved,2,before,2)==0);
    reset_error=15;CHECK(LitchSdkRestoreRetainedPeerPrivacy()==15);reset_error=0;
    add_error=42;CHECK(LitchSdkRestoreRetainedPeerPrivacy()==42);add_error=0;
    CHECK(LitchSdkRestoreRetainedPeerPrivacy()==0 && add_calls>=3);
    CHECK(opened==closed && host_locked==0 && unpair_oldest_requests==0 && unpair_except_requests==0);
    printf("addRollback case%d retainedKeys1 retainedOrphanRPA1 localIRK1 retryReopen1 checkedPrivacy1\n",which);
    return 0;
}
'''


class AddRollbackTests(unittest.TestCase):
    def test_real_third_phone_writers_distinguish_rpa_capacity_from_security_capacity(self):
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'rpa_capacity.c'
            binary = root / 'rpa_capacity'
            source.write_text(AddCorpus())
            build = subprocess.run([compiler, '-std=c11', '-O1', '-fsanitize=address,undefined',
                                    '-fno-sanitize-recover=all', '-I', str(SCRIPT.parent),
                                    str(source), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            for case, baseline_rpas, result in [(33, 3, 27), (34, 2, 0),
                                               (35, 3, 0), (36, 3, 0),
                                               (37, 3, 28), (38, 3, 28), (39, 3, 28),
                                               (40, 3, 0), (41, 3, 27)]:
                with self.subTest(case=case, baseline_rpas=baseline_rpas):
                    run = subprocess.run([str(binary), str(case)], capture_output=True,
                                         text=True, timeout=5)
                    print(run.stdout.strip(), run.stderr.strip())
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn(f'baselineRPA{baseline_rpas} ourRC0 peerRC0 rpaRC{result}', run.stdout)
                    self.assertEqual(run.stderr, '')

    def test_canonical_s3_capacity_matches_full_store_rollback_corpus(self):
        profile = configparser.ConfigParser(interpolation=None)
        profile.read(SCRIPT.parents[2] / 'platformio.ini')
        flags = profile['env:litch-esp32s3']['build_flags']
        for name, count in [('CONFIG_BT_NIMBLE_MAX_BONDS', 3),
                            ('CONFIG_BT_NIMBLE_MAX_CCCDS', 18),
                            ('MYNEWT_VAL_BLE_STORE_MAX_CSFCS', 3)]:
            self.assertIn(f'-D {name}={count}', flags)
        # The actual guarded writers below fill all eighteen subscription slots
        # and three CSFC slots before rolling back only the new phone.
        corpus = AddCorpus()
        self.assertIn('#define MYNEWT_VAL_BLE_STORE_MAX_CCCDS 18', corpus)
        self.assertIn('#define MYNEWT_VAL_BLE_STORE_MAX_CSFCS 3', corpus)

    def test_actual_sdk_rollback_faults_reopen_preserve_keys_and_all_preexisting_metadata(self):
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for label, destructive, count_only in [('checked', False, False),
                                                   ('destructive', True, False),
                                                   ('count_only', False, True)]:
                source = root / (label + '.c'); binary = root / label
                source.write_text(AddCorpus(destructive=destructive, count_only=count_only))
                build = subprocess.run([compiler, '-std=c11', '-O1', '-fsanitize=address,undefined',
                                        '-fno-sanitize-recover=all', '-I', str(SCRIPT.parent),
                                        str(source), '-o', str(binary)], capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                cases = [*range(26), 30, 31, 32] if label == 'checked' else [0]
                for case in cases:
                    with self.subTest(label=label, case=case):
                        run = subprocess.run([str(binary), str(case)], capture_output=True, text=True, timeout=5)
                        print(label, run.stdout.strip(), run.stderr.strip())
                        self.assertEqual(run.returncode, 0 if label == 'checked' else 1, run.stdout + run.stderr)
                        if label == 'checked': self.assertEqual(run.stderr, '')


if __name__ == '__main__':
    unittest.main()
