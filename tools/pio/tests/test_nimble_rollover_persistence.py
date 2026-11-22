"""SDK counter rollover and auxiliary retry/reopen; controlled NVS/GAP leaves."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_nimble_bond_inventory import CapacityCorpus
from test_nimble_security_store import Function, FIXTURE, SCRIPT, hardening


def PersistenceCorpus():
    config = hardening.HardenConfigSource((FIXTURE / 'ble_store_config.c').read_bytes()).decode()
    body = CapacityCorpus(hardening.HardenUtilSource(
        (FIXTURE / 'ble_store_util.c').read_bytes()).decode()).split('int main(')[0]
    # Exercise the real recursive renumbering callbacks instead of the older
    # security corpus's no-op rollover leaves. The seed below avoids 65k writes.
    prototypes = '''
int ble_store_config_compare_bond_count(const void *,const void *);
static int ble_store_config_write(int,const union ble_store_value *);
static int ble_store_config_delete(int,const union ble_store_key *);
'''
    for kind in ('our', 'peer'):
        stub = f'int ble_restore_{kind}_sec_nvs(void) {{ return 0; }}'
        if kind == 'our':
            stub += ' /* bond-counter rollover not exercised */'
        assert body.count(stub) == 1
        body = body.replace(stub, prototypes + Function(config, f'ble_restore_{kind}_sec_nvs'))
        prototypes = ''
    for kind in ('cccd', 'csfc'):
        for action in ('read', 'delete'):
            args = 'const void *k,void *v' if action == 'read' else 'const void *k'
            stub = f'static int ble_store_config_{action}_{kind}({args}) {{ return BLE_HS_ENOTSUP; }}'
            assert body.count(stub) == 1
            actual = Function(config, f'ble_store_config_{action}_{kind}')
            if action == 'read':
                actual = Function(config, f'ble_store_config_find_{kind}').split('{')[0] + ';\n' + actual
            body = body.replace(stub, actual)
    return body + MAIN


MAIN = r'''
static void seed(int table,int slot,const struct ble_store_value_sec *record) {
    disk[table][slot].present=true; disk[table][slot].size=sizeof *record;
    memcpy(disk[table][slot].data,record,sizeof *record);
}
static void aux_value(int type,union ble_store_value *record,int id,int marker) {
    memset(record,0,sizeof *record);
    if(type==BLE_STORE_OBJ_TYPE_CCCD) {
        record->cccd.peer_addr=value(id,0).peer_addr;
        record->cccd.chr_val_handle=(uint16_t)(100+id); record->cccd.flags=(uint16_t)marker;
    } else { record->csfc.peer_addr=value(id,0).peer_addr; record->csfc.data[0]=(uint8_t)marker; }
}
static void aux_key(int type,const union ble_store_value *record,union ble_store_key *key) {
    memset(key,0,sizeof *key);
    if(type==BLE_STORE_OBJ_TYPE_CCCD) ble_store_key_from_value_cccd(&key->cccd,&record->cccd);
    else ble_store_key_from_value_csfc(&key->csfc,&record->csfc);
}
static int aux_exact(int type,const union ble_store_value *record) {
    union ble_store_key key; union ble_store_value actual={0}; aux_key(type,record,&key);
    if(ble_hs_cfg.store_read_cb(type,&key,&actual)!=0) return 0;
    size_t size=type==BLE_STORE_OBJ_TYPE_CCCD?sizeof record->cccd:sizeof record->csfc;
    return !memcmp(&actual,record,size);
}
static int aux_count(int type) {
    return type==BLE_STORE_OBJ_TYPE_CCCD?ble_store_config_num_cccds:ble_store_config_num_csfcs;
}
static int aux_disk_count(int type) {
    int count=0,table=type==BLE_STORE_OBJ_TYPE_CCCD?2:3;
    for(int i=0;i<8;++i) count+=disk[table][i].present;
    return count;
}
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    int type=which<4?(which%2?BLE_STORE_OBJ_TYPE_CSFC:BLE_STORE_OBJ_TYPE_CCCD):which-3;
    ble_store_config_init(); ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    if(which<4) {
        union ble_store_value retained,incoming; union ble_store_key key;
        aux_value(type,&retained,1,1); aux_value(type,&incoming,2,2);
        /* Distinct CCCD handles for one peer must remain distinct records. */
        if(type==BLE_STORE_OBJ_TYPE_CCCD) incoming.cccd.peer_addr=retained.cccd.peer_addr;
        CHECK(ble_store_write(type,&retained)==0);
        if(which<2) {
            fault(BEFORE,type==BLE_STORE_OBJ_TYPE_CCCD?3:4);
            int failed=ble_store_write(type,&incoming);
            int retry=ble_store_write(type,&incoming);
            CHECK(failed==BLE_HS_ESTORE_FAIL && retry==0);
            ble_store_config_init();
            CHECK(aux_count(type)==2 && aux_exact(type,&retained) && aux_exact(type,&incoming));
            printf("aux %s append failure%d retry%d reopened2 retained_exact1\n",
                   type==BLE_STORE_OBJ_TYPE_CCCD?"CCCD":"CSFC",failed,retry);
            union ble_store_value prior=incoming;
            if(type==BLE_STORE_OBJ_TYPE_CCCD) incoming.cccd.flags=3;
            else incoming.csfc.data[0]=3;
            fault(type==BLE_STORE_OBJ_TYPE_CCCD?BEFORE:AFTER,type==BLE_STORE_OBJ_TYPE_CCCD?3:4);
            failed=ble_store_write(type,&incoming);
            int failed_ram_retained=aux_exact(type,&prior);
            retry=ble_store_write(type,&incoming);
            printf("aux %s update failure%d retry%d durable_count%d\n",
                   type==BLE_STORE_OBJ_TYPE_CCCD?"CCCD":"CSFC",failed,retry,aux_disk_count(type));
            ble_store_config_init();
            int reopened_exact=aux_exact(type,&incoming);
            printf("aux update reopened_exact%d unused_fault%d failed_ram_retained%d\n",
                   reopened_exact,fault_mode,failed_ram_retained);
            CHECK(failed==BLE_HS_ESTORE_FAIL && retry==0 && reopened_exact && fault_mode==0 && failed_ram_retained);
            CHECK(aux_count(type)==2 && aux_exact(type,&retained));
        } else {
            CHECK(ble_store_write(type,&incoming)==0); aux_key(type,&retained,&key);
            fault(type==BLE_STORE_OBJ_TYPE_CCCD?BEFORE:AFTER,type==BLE_STORE_OBJ_TYPE_CCCD?3:4);
            int failed=ble_hs_cfg.store_delete_cb(type,&key);
            int failed_ram_retained=aux_exact(type,&retained) && aux_exact(type,&incoming);
            int retry=ble_hs_cfg.store_delete_cb(type,&key);
            printf("aux %s delete failure%d retry%d durable_count%d\n",
                   type==BLE_STORE_OBJ_TYPE_CCCD?"CCCD":"CSFC",failed,retry,aux_disk_count(type));
            ble_store_config_init();
            printf("aux delete reopened_count%d failed_ram_retained%d\n",aux_count(type),failed_ram_retained);
            CHECK(failed==BLE_HS_ESTORE_FAIL && retry==0 && failed_ram_retained);
            union ble_store_value out={0};
            CHECK(ble_hs_cfg.store_read_cb(type,&key,&out)==BLE_HS_ENOENT);
            CHECK(aux_count(type)==1 && aux_exact(type,&incoming));
        }
        CHECK(aux_count(type)==aux_disk_count(type));
    } else {
        CHECK(sizeof ble_store_config_our_bond_count==2 && sizeof ble_store_config_our_secs[0].bond_count==2);
        const uint16_t threshold=UINT16_MAX-5;
        struct ble_store_value_sec expected[3];
        for(int i=0;i<3;++i) {
            expected[i]=value(i+1,17*(i+1)); expected[i].bond_count=(uint16_t)(threshold-3+i);
            seed(type-1,i,&expected[i]);
        }
        ble_store_config_init();
        uint16_t *counter=type==1?&ble_store_config_our_bond_count:&ble_store_config_peer_bond_count;
        CHECK(*counter==threshold-1);
        for(int step=0;step<6;++step) {
            int id=step%3; union ble_store_value incoming={0};
            expected[id].ltk[0]=(uint8_t)(90+step); incoming.sec=expected[id];
            CHECK(ble_store_write(type,&incoming)==0);
            CHECK(*counter==(step==0?threshold:step+2));
            for(int i=0;i<3;++i) {
                union ble_store_key key={0}; union ble_store_value out={0}; key.sec.peer_addr=expected[i].peer_addr;
                CHECK(ble_hs_cfg.store_read_cb(type,&key,&out)==0);
                expected[i].bond_count=out.sec.bond_count;
                CHECK(!memcmp(&out.sec,&expected[i],sizeof expected[i]) && exact_disk(type,&expected[i]));
            }
            printf("rollover %s step%d uint16_counter%u all3_exact1 durable%d\n",
                   type==1?"OUR":"PEER",step,*counter,disk_count(type));
        }
        struct blob before[6][8]; memcpy(before,disk,sizeof before);
        union ble_store_value excess={0}; excess.sec=value(99,99);
        CHECK(ble_store_write(type,&excess)==BLE_HS_ESTORE_CAP);
        CHECK(!memcmp(before,disk,sizeof before));
        ble_store_config_init(); CHECK(*counter==7 && disk_count(type)==3);
        for(int i=0;i<3;++i) {
            union ble_store_key key={0}; union ble_store_value out={0}; key.sec.peer_addr=expected[i].peer_addr;
            CHECK(ble_hs_cfg.store_read_cb(type,&key,&out)==0 && !memcmp(&out.sec,&expected[i],sizeof expected[i]));
        }
    }
    CHECK(opened==closed && host_locked==0);
    CHECK(unpair_oldest_requests==0 && unpair_except_requests==0);
    printf("journey%d PASS writes%d commits%d freshReadonly%d\n",which,writes,commits,read_opens);
    return 0;
}
'''


class RolloverPersistenceTests(unittest.TestCase):
    def test_uint16_rollover_and_auxiliary_failure_retry_reopen(self):
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler)
        names = ('cccd_append_update_retry_reopen', 'csfc_append_update_retry_reopen',
                 'cccd_delete_retry_reopen', 'csfc_delete_retry_reopen',
                 'our_uint16_rollover_full_store_reopen', 'peer_uint16_rollover_full_store_reopen')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); source = root / 'persistence.c'; binary = root / 'persistence'
            source.write_text(PersistenceCorpus())
            build = subprocess.run([compiler, '-std=c11', '-O1', '-fsanitize=address,undefined',
                                    '-fno-sanitize-recover=all', '-I', str(SCRIPT.parent),
                                    str(source), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            for case, name in enumerate(names):
                with self.subTest(journey=name):
                    result = subprocess.run([str(binary), str(case)], capture_output=True, text=True, timeout=5)
                    print(name, 'exit', result.returncode, result.stdout.strip(), result.stderr.strip())
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(result.stderr, '')


if __name__ == '__main__':
    unittest.main()
