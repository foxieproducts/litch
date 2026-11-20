"""Actual SDK rollover callers retain custody through partial NVS failure."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_nimble_rollover_persistence import PersistenceCorpus
from test_nimble_security_store import Function, FIXTURE, SCRIPT


def RolloverFailureCorpus(original_renumber=False):
    body = PersistenceCorpus().split('static void seed(')[0]
    if original_renumber:
        pinned = (FIXTURE / 'ble_store_config.c').read_text()
        for kind in ('our', 'peer'):
            name = f'ble_restore_{kind}_sec_nvs'
            body = body.replace(Function(body, name), Function(pinned, name), 1)
    marker = 'static esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t size) {'
    assert body.count(marker) == 1
    body = body.replace(marker, '''
static int set_calls,fail_set_at,fail_commit_at,fail_visibility;
''' + marker + '''
    if (++set_calls==fail_set_at) set_failure=fail_visibility;
''', 1)
    marker = '    ++commits;\n    int mode='
    assert body.count(marker) == 1
    body = body.replace(marker, '''    ++commits;
    if (commits==fail_commit_at) fault(fail_visibility,handles[h].dirty_type);
    int mode=''', 1)
    return body + MAIN


MAIN = r'''
static void seed(int table,int slot,const void *record,size_t size) {
    disk[table][slot].present=true; disk[table][slot].size=size;
    memcpy(disk[table][slot].data,record,size);
}
static int table_count(int type) {
    return type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs;
}
static uint16_t table_counter(int type) {
    return type==1?ble_store_config_our_bond_count:ble_store_config_peer_bond_count;
}
static int same_keys(struct ble_store_value_sec left,struct ble_store_value_sec right) {
    left.bond_count=right.bond_count=0;
    return !memcmp(&left,&right,sizeof left);
}
static int custody(int type,const struct ble_store_value_sec *expected,int count) {
    if(table_count(type)!=count || disk_count(type)!=count) return 0;
    for(int i=0;i<count;++i) {
        union ble_store_key key={0}; union ble_store_value out={0};
        key.sec.peer_addr=expected[i].peer_addr;
        if(ble_hs_cfg.store_read_cb(type,&key,&out)!=0 || !same_keys(out.sec,expected[i])) return 0;
        int found=0;
        for(int slot=0;slot<3;++slot) if(disk[type-1][slot].present) {
            struct ble_store_value_sec native;
            if(disk[type-1][slot].size!=sizeof native) return 0;
            memcpy(&native,disk[type-1][slot].data,sizeof native);
            found+=same_keys(native,expected[i]);
        }
        if(found!=1) return 0;
    }
    return 1;
}
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    int type=which%2+1,scenario=which/2;
    int repeated=scenario==16;
    int count=scenario<4 || repeated?1:3;
    int failure=scenario<4?scenario:(scenario-4)%4;
    int position=scenario<4 || repeated?0:(scenario-4)/4;
    int target_set=2+position,target_commit=2+position;
    struct ble_store_value_sec expected[3];
    for(int i=0;i<count;++i) {
        expected[i]=value(i+1,17*(i+1));
        expected[i].bond_count=(uint16_t)(UINT16_MAX-5-count+1+i);
        seed(type-1,i,&expected[i],sizeof expected[i]);
    }
    struct ble_store_value_sec neighbor=value(80,45); neighbor.bond_count=9;
    seed(type==1?1:0,1,&neighbor,sizeof neighbor);
    struct ble_store_value_cccd cccd={0}; cccd.peer_addr=neighbor.peer_addr;
    cccd.chr_val_handle=33; cccd.flags=1; seed(2,0,&cccd,sizeof cccd);
    struct ble_store_value_csfc csfc={0}; csfc.peer_addr=neighbor.peer_addr;
    csfc.data[0]=1; seed(3,0,&csfc,sizeof csfc);
    struct blob untouched[6][8]; memcpy(untouched,disk,sizeof untouched);
    ble_store_config_init(); ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    CHECK(table_counter(type)==UINT16_MAX-5);
    union ble_store_value incoming={0}; incoming.sec=expected[0]; incoming.sec.ltk[0]=99;
    expected[0]=incoming.sec;
    if(repeated) failure=0;
    fail_visibility=failure%2?AFTER:BEFORE;
    if(failure<2) fail_set_at=target_set;
    else fail_commit_at=target_commit;
    int failed=ble_store_write(type,&incoming);
    printf("rollover fault%d type%d count%d position%d kind%d rc%d ram%d native%d counter%u sets%d commits%d\n",
           which,type,count,position,failure,failed,table_count(type),disk_count(type),
           table_counter(type),set_calls,commits);
    fflush(stdout);
    CHECK(failed==BLE_HS_ESTORE_FAIL);
    CHECK((failure<2?set_calls>=target_set:commits>=target_commit) && set_failure==0 && fault_mode==0);
    CHECK(custody(type,expected,count));
    CHECK(table_counter(type)==UINT16_MAX-4);
    if(repeated) {
        for(int retry=0;retry<8;++retry) {
            fail_set_at=set_calls+1; fail_visibility=BEFORE;
            CHECK(ble_store_write(type,&incoming)==BLE_HS_ESTORE_FAIL);
            CHECK(custody(type,expected,count) && table_counter(type)==UINT16_MAX-4);
        }
        printf("rollover repeated type%d failures9 counter%u custody1\n",type,table_counter(type));
    }
    /* Reset/reopen before retry: partial counter rewrites are valid custody,
     * not an absent owner. Startup must restore every complete native record. */
    ble_store_config_init();
    CHECK(custody(type,expected,count));
    fail_set_at=fail_commit_at=0;
    CHECK(ble_store_write(type,&incoming)==0);
    CHECK(custody(type,expected,count) && table_counter(type)==count+1);
    ble_store_config_init();
    CHECK(custody(type,expected,count) && table_counter(type)==count+1);
    for(int table=0;table<6;++table) if(table!=type-1)
        CHECK(!memcmp(disk[table],untouched[table],sizeof disk[table]));
    CHECK(opened==closed && host_locked==0 && unpair_oldest_requests==0 && unpair_except_requests==0);
    printf("rollover fault%d PASS reopened%d retried1 neighbor_exact1 counter%u\n",which,count,table_counter(type));
    return 0;
}
'''


class RolloverFailureTests(unittest.TestCase):
    def test_actual_rollover_failures_keep_keys_retry_and_reopen(self):
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for label, original, cases in [('old', True, (0, 1)),
                                            ('new', False, range(34))]:
                source = root / (label + '.c'); binary = root / label
                source.write_text(RolloverFailureCorpus(original))
                build = subprocess.run([compiler, '-std=c11', '-O1', '-fsanitize=address,undefined',
                                        '-fno-sanitize-recover=all', '-I', str(SCRIPT.parent),
                                        str(source), '-o', str(binary)], capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                for case in cases:
                    with self.subTest(variant=label, case=case):
                        result = subprocess.run([str(binary), str(case)], capture_output=True,
                                                text=True, timeout=5)
                        print(label, result.stdout.strip(), result.stderr.strip())
                        if original:
                            self.assertEqual(result.returncode, 1)
                            self.assertIn('rc28 ram0 native0', result.stdout)
                        else:
                            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                            self.assertEqual(result.stderr, '')


if __name__ == '__main__':
    unittest.main()
