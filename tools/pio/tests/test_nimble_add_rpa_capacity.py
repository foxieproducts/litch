"""Consent-scoped Add cache capacity, through actual native/NVS writers."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_nimble_add_rollback import AddCorpus
from test_nimble_security_store import Function, FIXTURE, SCRIPT, hardening


MAIN = r'''
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    ble_store_config_init(); ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    seed_peer(1);
    ble_addr_t saved[2]={value(1,0).peer_addr,value(91,0).peer_addr};
    int saved_count=1;
    if(which==1 || which==2) {
        /* A partial record in either table still protects its identity. */
        struct ble_store_value_sec partial={0}; partial.peer_addr=saved[1];
        if(which==1) {
            ble_store_config_our_secs[1]=partial; ble_store_config_num_our_secs=2;
            CHECK(ble_store_config_persist_our_secs()==0);
        } else {
            ble_store_config_peer_secs[1]=partial; ble_store_config_num_peer_secs=2;
            CHECK(ble_store_config_persist_peer_secs()==0);
        }
        saved_count=2;
    }
    struct ble_store_value_sec retained[2][3]={0};
    int our_count=ble_store_config_num_our_secs, peer_count=ble_store_config_num_peer_secs;
    memcpy(retained[0],ble_store_config_our_secs,our_count*sizeof retained[0][0]);
    memcpy(retained[1],ble_store_config_peer_secs,peer_count*sizeof retained[1][0]);
    struct ble_store_value_rpa_rec before[3]={0};
    for(int i=0;i<3;++i) {
        before[i].peer_rpa_addr=value(81+i,0).peer_addr;
        before[i].peer_addr=value(i==0?1:90+i,0).peer_addr;
    }
    before[0].peer_addr.type|=2; /* Native ID alias still owns its last mapping. */
    if(which==2) before[1].peer_addr.type|=2;
    if(which==15) before[2].peer_addr=before[1].peer_addr;
    int baseline_count=which==16?2:3;
    memcpy(ble_store_config_rpa_recs,before,sizeof before); ble_store_config_num_rpa_recs=baseline_count;
    CHECK(ble_store_config_persist_rpa_recs()==0);
    struct ble_store_value_rpa_rec baseline[3]; int count=-1;
    CHECK(LitchSdkReadAddRpaSnapshot(baseline,&count,3)==0 && count==baseline_count);
    struct ble_store_value_sec added=value(2,22); added.irk_present=1;
    CHECK(ble_store_write_our_sec(&added)==0 && ble_store_write_peer_sec(&added)==0);
    struct ble_store_value_rpa_rec incoming={0};
    incoming.peer_addr=added.peer_addr; incoming.peer_rpa_addr=value(84,0).peer_addr;
    if(which==0) {
        CHECK(ble_store_write_rpa_rec(&incoming)==BLE_HS_ESTORE_CAP);
        CHECK(!memcmp(ble_store_config_rpa_recs,before,sizeof before));
    }
    if(which==3) ble_store_config_our_secs[0].peer_addr.type=4;
    if(which==4) ble_store_config_num_peer_secs=4;
    if(which==5) ble_store_config_num_rpa_recs=-1;
    if(which==6) litch_nimble_store_restore_status=BLE_HS_ESTORE_FAIL;
    if(which==7) ble_store_config_rpa_recs[0].peer_addr.type=4;
    if(which==8) memset(ble_store_config_peer_secs[0].peer_addr.val,0,6);
    if(which==9) incoming.peer_rpa_addr=before[0].peer_rpa_addr;
    if(which==10) ble_store_config_num_our_secs=-1;
    if(which==11 || which==12) fault(which-10,6);
    if(which==13) set_failure=2;
    if(which==14) incoming.peer_addr=before[0].peer_addr;
    if(which>=14) force_rpa_capacity=1;
    struct ble_store_value_rpa_rec at_write[3]; memcpy(at_write,ble_store_config_rpa_recs,sizeof at_write);
    int count_at_write=ble_store_config_num_rpa_recs;
    int rc=LitchSdkWriteAddRpaRec(&incoming);
    int expected=which>=14?BLE_HS_ESTORE_CAP:which>=3?BLE_HS_ESTORE_FAIL:0;
    printf("Add cache case%d rc=%d\n",which,rc);
    CHECK(rc==expected);
    if(which>=3) {
        CHECK(ble_store_config_num_rpa_recs==count_at_write &&
              !memcmp(ble_store_config_rpa_recs,at_write,sizeof at_write));
        if(which>=14) CHECK(forced_rpa_calls==1);
        if(which<=10 || which>=14) {
            CHECK(host_locked==0 && opened==closed && unpair_oldest_requests==0 && unpair_except_requests==0);
            return 0;
        }
    } else {
    CHECK(!memcmp(&ble_store_config_rpa_recs[0],&before[0],sizeof before[0]));
    int replaced=which==0?1:2, untouched=which==0?2:1;
    CHECK(!memcmp(&ble_store_config_rpa_recs[replaced],&incoming,sizeof incoming));
    CHECK(!memcmp(&ble_store_config_rpa_recs[untouched],&before[untouched],sizeof before[untouched]));
    }
    for(int i=0;i<our_count;++i) CHECK(has_disk(0,&retained[0][i],sizeof retained[0][i]));
    for(int i=0;i<peer_count;++i) CHECK(has_disk(1,&retained[1][i],sizeof retained[1][i]));
    ble_store_config_init(); CHECK(litch_nimble_store_restore_status==0);
    CHECK(LitchSdkRollbackAddedPeers(saved,saved_count,baseline,count)==0);
    ble_store_config_init(); CHECK(litch_nimble_store_restore_status==0);
    CHECK(ble_store_config_num_our_secs==our_count && ble_store_config_num_peer_secs==peer_count &&
          ble_store_config_num_rpa_recs==3 && !memcmp(ble_store_config_rpa_recs,before,sizeof before));
    for(int i=0;i<our_count;++i) CHECK(has_disk(0,&retained[0][i],sizeof retained[0][i]));
    for(int i=0;i<peer_count;++i) CHECK(has_disk(1,&retained[1][i],sizeof retained[1][i]));
    CHECK(host_locked==0 && opened==closed && unpair_oldest_requests==0 && unpair_except_requests==0);
    return 0;
}
'''


class AddRpaCapacityTests(unittest.TestCase):
    def test_authorized_add_reuses_orphan_without_losing_retained_custody(self):
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler)
        source = AddCorpus().split('int main(int argc,char **argv)')[0] + MAIN
        persist = Function(hardening.HardenNvsSource((FIXTURE / 'ble_store_nvs.c').read_bytes()).decode(),
                           'ble_store_config_persist_rpa_recs')
        source = source.replace(persist, persist.replace('ble_store_config_persist_rpa_recs(',
                                                        'actual_persist_rpa_recs(', 1) + r'''
static int force_rpa_capacity,forced_rpa_calls;
int ble_store_config_persist_rpa_recs(void) {
    if(force_rpa_capacity) { ++forced_rpa_calls; return BLE_HS_ESTORE_CAP; }
    return actual_persist_rpa_recs();
}
''', 1)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / 'add_rpa.c'
            binary = root / 'add_rpa'
            path.write_text(source)
            build = subprocess.run([compiler, '-std=c11', '-O1', '-fsanitize=address,undefined',
                                    '-fno-sanitize-recover=all', '-I', str(SCRIPT.parent),
                                    str(path), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            for case in range(17):
                with self.subTest(case=case):
                    run = subprocess.run([str(binary), str(case)], capture_output=True, text=True, timeout=5)
                    print(run.stdout.strip(), run.stderr.strip())
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertEqual(run.stderr, '')
