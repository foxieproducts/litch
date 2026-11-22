"""Exact SDK inventory and store/status dispatch; controlled NVS/GAP leaves."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_nimble_security_store import Function, FIXTURE, SCRIPT, hardening
from test_nimble_startup_readiness import StartupCorpus

MAIN = r'''
static void seed(int table,int slot,const struct ble_store_value_sec *record) {
    disk[table][slot].present=true; disk[table][slot].size=sizeof *record;
    memcpy(disk[table][slot].data,record,sizeof *record);
}
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    struct ble_store_value_sec a=value(1,17),b=value(2,34),c=value(3,51);
    a.bond_count=1; b.bond_count=2; c.bond_count=3;
    if(which==0) seed(1,2,&a); /* peer-only custody */
    if(which==1) { seed(0,0,&a); seed(1,2,&a); } /* same identity, one entry */
    if(which==2) { seed(0,0,&a); a.peer_addr.type=0; seed(1,2,&a); }
    if(which==3) for(int i=0;i<3;++i) {
        struct ble_store_value_sec v=value(i+1,17); v.bond_count=i+1; seed(0,i,&v);
        v.peer_addr.val[0]+=3; seed(1,i,&v);
    }
    if(which==4 || which==5) { seed(0,0,&a); seed(1,2,&b); }
    if(which==7) { seed(0,0,&a); read_fault_at=1; }
    ble_store_config_init();
    inventory_fault=which==5;
    ble_addr_t out[6], prior[6]; memset(out,0x7e,sizeof out); memcpy(prior,out,sizeof out);
    int count=91, capacity=which==4?1:6;
    int rc=ble_store_util_bonded_peers(out,&count,capacity);
    printf("inventory case%d rc%d count%d\n",which,rc,count);
    if(which==4 || which==5 || which==7) {
        CHECK(rc==(which==4?BLE_HS_ENOMEM:BLE_HS_ESTORE_FAIL));
        CHECK(count==91 && !memcmp(out,prior,sizeof out));
    } else {
        int expected=which==3?6:which==2?2:which==6?0:1;
        CHECK(rc==0 && count==expected);
        if(which==0 || which==1) CHECK(!ble_addr_cmp(&out[0],&a.peer_addr));
        if(which==2) CHECK(out[0].type==1 && out[1].type==0 && !memcmp(out[0].val,out[1].val,6));
        if(which==3) for(int i=0;i<6;++i) CHECK(out[i].type==1 && out[i].val[0]==i+1);
    }
    CHECK(opened==closed); CHECK(writes==0 && commits==0); return 0;
}
'''


def InventoryCorpus(util):
    config = hardening.HardenConfigSource((FIXTURE / 'ble_store_config.c').read_bytes()).decode()
    nvs = hardening.HardenNvsSource((FIXTURE / 'ble_store_nvs.c').read_bytes()).decode()
    body = StartupCorpus(config, nvs).split('static void seed(')[0]
    leaf = r'''
typedef int ble_store_iterator_fn(int,union ble_store_value *,void *);
static int inventory_fault;
static int ble_store_read(int type,const union ble_store_key *key,union ble_store_value *value) {
    if(inventory_fault && type==BLE_STORE_OBJ_TYPE_PEER_SEC && key->sec.idx==1) return BLE_HS_ESTORE_FAIL;
    return ble_hs_cfg.store_read_cb(type,key,value);
}
'''
    store = (FIXTURE / 'ble_store.c').read_text()
    start = util.index('struct ble_store_util_peer_set {')
    peer_set = util[start:util.index('};', start)+2]
    return (body + leaf + Function(store, 'ble_store_iterate') + peer_set +
            Function(util, 'ble_store_util_iter_unique_peer') +
            Function(util, 'ble_store_util_bonded_peers') + MAIN)


def CapacityCorpus(util):
    config = hardening.HardenConfigSource((FIXTURE / 'ble_store_config.c').read_bytes()).decode()
    nvs = hardening.HardenNvsSource((FIXTURE / 'ble_store_nvs.c').read_bytes()).decode()
    store = (FIXTURE / 'ble_store.c').read_text()
    body = StartupCorpus(config, nvs).split('static void seed(')[0]
    # Extend the existing semantic host types for the actual auxiliary writers;
    # the retained SDK fixtures and their hash guards remain unchanged.
    body = body.replace('uint16_t handle,flags;', 'uint16_t chr_val_handle,flags;')
    body = body.replace('union ble_store_key {', '''
struct ble_store_key_cccd { ble_addr_t peer_addr; uint16_t chr_val_handle; uint8_t idx; };
struct ble_store_key_csfc { ble_addr_t peer_addr; uint8_t idx; };
union ble_store_key {''', 1).replace(
        'struct { ble_addr_t peer_addr; uint8_t idx; } cccd,csfc;',
        'struct ble_store_key_cccd cccd; struct ble_store_key_csfc csfc;')
    for kind in ('cccd', 'csfc'):
        stub = f'static int ble_store_config_write_{kind}(const void *v) {{ return BLE_HS_ENOTSUP; }}'
        actual = '\n'.join((Function(store, 'ble_store_key_from_value_' + kind),
                            Function(config, 'ble_store_config_find_' + kind),
                            Function(config, 'ble_store_config_write_' + kind)))
        assert body.count(stub) == 1
        body = body.replace(stub, actual)
    needle = '    int (*store_delete_cb)(int,const union ble_store_key *);'
    assert body.count(needle) == 1
    body = 'struct ble_store_status_event;\n' + body.replace(needle, needle + '''
    int (*store_status_cb)(struct ble_store_status_event *,void *);
    void *store_status_arg;''')
    leaf = r'''
#define BLE_STORE_EVENT_OVERFLOW 1
#define BLE_STORE_EVENT_FULL 2
struct ble_store_status_event {
    int event_code;
    union {
        struct { int obj_type; const union ble_store_value *value; } overflow;
        struct { int obj_type; uint16_t conn_handle; } full;
    };
};
static int host_locked, unpair_oldest_requests, unpair_except_requests;
static void ble_hs_lock(void) { ++host_locked; }
static void ble_hs_unlock(void) { --host_locked; }
static int ble_hs_locked_by_cur_task(void) { return host_locked != 0; }
/* Stop at the actual GAP deletion boundary; do not emulate deletion or RF. */
static int ble_gap_unpair_oldest_peer(void) {
    ++unpair_oldest_requests; return BLE_HS_EUNKNOWN;
}
static int ble_gap_unpair_oldest_except(const ble_addr_t *peer) {
    (void)peer; ++unpair_except_requests; return BLE_HS_EUNKNOWN;
}
'''
    functions = '\n'.join(Function(store, name) for name in (
        'ble_store_status', 'ble_store_overflow_event', 'ble_store_full_event', 'ble_store_write'))
    functions += '\n' + Function(util, 'ble_store_util_status_rr')
    main = r'''
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    ble_store_config_init();
    ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    for (int id=1;id<=3;++id) {
        union ble_store_value item={0}; item.sec=value(id,17*id);
        CHECK(ble_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&item)==0);
        CHECK(ble_store_write(BLE_STORE_OBJ_TYPE_PEER_SEC,&item)==0);
        item.csfc.peer_addr=item.sec.peer_addr;
        CHECK(ble_store_write(BLE_STORE_OBJ_TYPE_CSFC,&item)==0);
    }
    for (int id=1;id<=8;++id) {
        union ble_store_value item={0}; item.cccd.peer_addr=value(id,0).peer_addr;
        item.cccd.chr_val_handle=id; item.cccd.flags=1;
        CHECK(ble_store_write(BLE_STORE_OBJ_TYPE_CCCD,&item)==0);
    }
    struct blob before_disk[6][8]; memcpy(before_disk,disk,sizeof disk);
    struct ble_store_value_sec before_our[3],before_peer[3];
    struct ble_store_value_cccd before_cccd[8]; struct ble_store_value_csfc before_csfc[3];
    memcpy(before_our,ble_store_config_our_secs,sizeof before_our);
    memcpy(before_peer,ble_store_config_peer_secs,sizeof before_peer);
    memcpy(before_cccd,ble_store_config_cccds,sizeof before_cccd);
    memcpy(before_csfc,ble_store_config_csfcs,sizeof before_csfc);
    uint16_t our_counter=ble_store_config_our_bond_count,peer_counter=ble_store_config_peer_bond_count;
    int before_writes=writes,before_commits=commits;
    const int types[]={BLE_STORE_OBJ_TYPE_OUR_SEC,BLE_STORE_OBJ_TYPE_PEER_SEC,
                       BLE_STORE_OBJ_TYPE_CCCD,BLE_STORE_OBJ_TYPE_CSFC};
    int type=which<4?types[which]:which%2+1;
    union ble_store_value incoming={0}; incoming.sec=value(which<4?99:1,99);
    if(which==2) { incoming.cccd.peer_addr=value(99,0).peer_addr; incoming.cccd.chr_val_handle=99; }
    if(which==3) incoming.csfc.peer_addr=value(99,0).peer_addr;
    if(which==6 || which==7) fault(BEFORE,type);
    int rc;
    if(which==8) rc=ble_store_overflow_event(BLE_STORE_OBJ_TYPE_PEER_ADDR,&incoming);
    else if(which==9) {
        for(int i=0;i<4;++i) CHECK(ble_store_full_event(types[i],1)==0);
        rc=0;
    } else if(which==10) {
        CHECK(ble_store_overflow_event(255,&incoming)==BLE_HS_EUNKNOWN);
        struct ble_store_status_event event={.event_code=255};
        rc=ble_store_status(&event);
    } else rc=ble_store_write(type,&incoming);
    int same_disk=!memcmp(before_disk,disk,sizeof disk);
    int same_ram=!memcmp(before_our,ble_store_config_our_secs,sizeof before_our) &&
        !memcmp(before_peer,ble_store_config_peer_secs,sizeof before_peer) &&
        !memcmp(before_cccd,ble_store_config_cccds,sizeof before_cccd) &&
        !memcmp(before_csfc,ble_store_config_csfcs,sizeof before_csfc);
    printf("capacity case%d rc%d unpair%d/%d same_ram%d same_disk%d writes%d commits%d\n",
        which,rc,unpair_oldest_requests,unpair_except_requests,same_ram,same_disk,
        writes-before_writes,commits-before_commits);
    CHECK(unpair_oldest_requests==0 && unpair_except_requests==0);
    CHECK(ble_store_config_num_our_secs==3 && ble_store_config_num_peer_secs==3 &&
          ble_store_config_num_cccds==8 && ble_store_config_num_csfcs==3);
    if(which<4 || which==8) {
        CHECK(rc==BLE_HS_ESTORE_CAP && same_ram && same_disk);
        CHECK(writes==before_writes && commits==before_commits);
        CHECK(our_counter==ble_store_config_our_bond_count && peer_counter==ble_store_config_peer_bond_count);
    } else if(which==4 || which==5) {
        CHECK(rc==0 && !same_ram && !same_disk);
        CHECK(writes==before_writes+1 && commits==before_commits+1);
        struct ble_store_value_sec expected=incoming.sec;
        expected.bond_count=type==1?ble_store_config_our_bond_count:ble_store_config_peer_bond_count;
        CHECK(exact_disk(type,&expected));
    } else if(which==6 || which==7) {
        CHECK(rc==BLE_HS_ESTORE_FAIL && same_ram && same_disk);
        CHECK(our_counter==ble_store_config_our_bond_count && peer_counter==ble_store_config_peer_bond_count);
    } else {
        CHECK(rc==(which==9?0:BLE_HS_EUNKNOWN) && same_ram && same_disk);
        CHECK(writes==before_writes && commits==before_commits);
    }
    CHECK(opened==closed && host_locked==0); return 0;
}
'''
    return body + leaf + functions + main


class BondInventoryTests(unittest.TestCase):
    def test_public_store_capacity_never_requests_bond_eviction(self):
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler)
        original = (FIXTURE / 'ble_store_util.c').read_bytes()
        old_failures = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for label, source in [('old', original), ('new', hardening.HardenUtilSource(original))]:
                path = root / (label+'.c'); binary = root / label
                path.write_text(CapacityCorpus(source.decode()))
                build = subprocess.run([compiler,'-std=c11','-O1','-fsanitize=address,undefined',
                                        '-fno-sanitize-recover=all','-I',str(SCRIPT.parent),str(path),
                                        '-o',str(binary)],capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                for case in range(11):
                    with self.subTest(label=label,case=case):
                        result = subprocess.run([str(binary),str(case)],capture_output=True,text=True,timeout=5)
                        print(label.upper(),result.stdout.strip(),result.stderr.splitlines()[:1])
                        if label=='old':
                            if result.returncode: old_failures.append(case)
                        else:
                            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                            self.assertEqual(result.stderr,'')
        self.assertEqual(old_failures,[0,1,2,3,8])

    def test_peer_only_union_exact_types_capacity_and_read_failure(self):
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler)
        original = (FIXTURE / 'ble_store_util.c').read_bytes()
        self.assertEqual(hashlib.sha256(original).hexdigest(), hardening.UTIL_ORIGINAL_SHA256)
        old_failures = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for label, source in [('old', original), ('new', hardening.HardenUtilSource(original))]:
                path = root / (label+'.c'); binary = root / label
                path.write_text(InventoryCorpus(source.decode()))
                build = subprocess.run([compiler,'-std=c11','-O1','-fsanitize=address,undefined',
                                        '-fno-sanitize-recover=all','-I',str(SCRIPT.parent),str(path),
                                        '-o',str(binary)],capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                for case in range(8):
                    result = subprocess.run([str(binary),str(case)],capture_output=True,text=True)
                    print(label.upper(),result.stdout.strip(),result.stderr.splitlines()[:1])
                    if label=='old':
                        if result.returncode: old_failures.append(case)
                    else:
                        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                        self.assertEqual(result.stderr,'')
        self.assertEqual(old_failures,[0,2,3,4,5])

    def test_pinned_transform_and_single_generated_util_owner(self):
        original=(FIXTURE/'ble_store_util.c').read_bytes()
        with self.assertRaises(ValueError): hardening.HardenUtilSource(original+b'\n')
        # Only inventory and the status callback (including its stale SDK
        # eviction comment) change; all other utility bytes remain exact.
        actual=hardening.HardenUtilSource(original)
        def Normalize(source):
            source=source.replace(Function(source.decode(),'ble_store_util_bonded_peers').encode(),b'<inventory>')
            callback=Function(source.decode(),'ble_store_util_status_rr').encode()
            start=source.rfind(b'/**\n',0,source.index(callback))
            return source[:start]+b'<status>'+source[source.index(callback)+len(callback):]
        self.assertEqual(Normalize(actual),Normalize(original))
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); package=root/'deps/NimBLE-Arduino'
            source=package/'src/nimble/nimble/host/src/ble_store_util.c'
            source.parent.mkdir(parents=True); source.write_bytes(original)
            shutil.copy2(FIXTURE/'library.properties',package)
            class Node:
                def __init__(self,path,original=None): self.path,self.original=path,original
                def get_abspath(self): return str(self.path)
                def srcnode(self): return self.original or self
            case=self
            class Environment:
                def subst(self,name): case.assertEqual(name,'$BUILD_DIR'); return str(root/'build')
                def AppendUnique(self,**values): case.assertEqual(values,{'CPPPATH':[str(source.parent)]})
                def File(self,path): return Node(Path(path))
            variant=root/'build/lib123/NimBLE-Arduino/ble_store_util.c'
            output=hardening._HardenUtilNode(Environment(),Node(variant,Node(source))).path
            self.assertEqual(output,root/'build/litch_sdk/ble_store_util.c')
            self.assertEqual(output.read_bytes(),actual)
            stamp=output.stat().st_mtime_ns
            self.assertEqual(hardening.WriteHardenedUtilSource(package,root/'build'),output)
            self.assertEqual(output.stat().st_mtime_ns,stamp)
            self.assertEqual(source.read_bytes(),original); self.assertFalse(variant.exists())
            (package/'library.properties').write_text('name=NimBLE-Arduino\nversion=2.3.8\n')
            with self.assertRaises(ValueError): hardening.WriteHardenedUtilSource(package,root/'unknown')
            self.assertFalse((root/'unknown').exists())
