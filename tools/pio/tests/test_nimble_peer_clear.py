"""Actual native clear/store/privacy seam; controlled NVS, HCI and service leaves.

A duplicate resolving-list identity is a controlled fault, not physical RF cause
proof. The SDK privacy source is retained in the existing licensed pinned
fixture. Aggregate S3 compilation separately proves ABI/linkage.
"""
from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile
import unittest

from test_nimble_pairing_admission import PairingCorpus
from test_nimble_security_store import Function, FIXTURE, SCRIPT, hardening

REPO = SCRIPT.parents[2]
PRIVACY_SHA256 = "83ef670e15211ef7b96bed2574ffab746c3b13009903074a2044b45a5d057c38"


def PrivacySource():
    source = (FIXTURE / 'ble_hs_pvcy.c').read_bytes()
    if hashlib.sha256(source).hexdigest() != PRIVACY_SHA256:
        raise ValueError('Unsupported pinned SDK privacy source')
    return source.decode()


def NativeFunction(name):
    source = (REPO / 'src/ble_service_owner_native.cpp').read_text()
    start = source.index('bool BleService::' + name + '(')
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def ClearCorpus():
    sm = hardening.HardenSmSource((FIXTURE / 'ble_sm.c').read_bytes()).decode()
    body = PairingCorpus(sm).split('int main(')[0]
    store = (FIXTURE / 'ble_store.c').read_text()
    config = hardening.HardenConfigSource((FIXTURE / 'ble_store_config.c').read_bytes()).decode()
    # Use real public delete dispatch and remote table callbacks. Local
    # IRK deletion is real too so the OLD public-clear control cannot hide it.
    stub = 'static int ble_store_delete(int type,const union ble_store_key *key) {'
    start = body.index(stub); end = body.index('\n}', start) + 2
    body = body[:start] + 'int ble_store_delete(int,const union ble_store_key *);' + body[end:]
    body = body.replace('union ble_store_key {',
        'struct ble_store_key_local_irk { ble_addr_t addr; uint8_t idx; };\n'
        'struct ble_store_key_rpa_rec { ble_addr_t peer_rpa_addr; uint8_t idx; };\n'
        'union ble_store_key {', 1)
    body = body.replace('struct { ble_addr_t addr; uint8_t idx; } local_irk;',
                        'struct ble_store_key_local_irk local_irk;').replace(
        'struct { ble_addr_t peer_rpa_addr; uint8_t idx; } rpa_rec;',
        'struct ble_store_key_rpa_rec rpa_rec;')
    nvs = hardening.HardenNvsSource((FIXTURE / 'ble_store_nvs.c').read_bytes()).decode()
    for kind, table in (('local_irk', 'local_irk'), ('rpa_rec', 'rpa_recs')):
        stub = f'static int ble_store_config_delete_{kind}(const void *k) {{ return BLE_HS_ENOTSUP; }}'
        actual = Function(nvs, 'ble_store_config_persist_' + table) + '\n' + '\n'.join(
            Function(config, 'ble_store_config_' + action + '_' + kind) for action in ('find', 'delete'))
        body = body.replace(stub, actual, 1)
    stub = 'static int ble_hs_pvcy_add_entry(const uint8_t *a,uint8_t t,const uint8_t *i) { (void)a; (void)t; (void)i; return 0; }'
    assert body.count(stub) == 1
    privacy = PrivacySource()
    functions = '\n'.join(Function(privacy, name) for name in (
        'ble_hs_pvcy_set_resolve_enabled', 'ble_hs_pvcy_remove_entry', 'ble_hs_pvcy_clear_entries',
        'ble_hs_pvcy_add_entry_hci', 'ble_hs_pvcy_add_entry', 'ble_hs_pvcy_set_our_irk',
        'ble_hs_pvcy_our_irk', 'ble_hs_pvcy_set_mode'))
    body = body.replace(stub, HCI + functions, 1)
    body += '\n' + '\n'.join(Function(store, name) for name in ('ble_store_delete', 'ble_store_clear'))
    for name in ('LitchSdkClearPeerStore', 'LitchSdkReconcileEmptyPeerStore', 'LitchSdkResetPeerPrivacy',
                 'LitchSdkReadPeerMetadata'):
        if name in config:
            body += '\n' + Function(config, name)
    return body + MAIN


HCI = r'''
int fixture_adv,fixture_disc,fixture_links,fixture_stop_fail,fixture_cancel_fail;

#define BLE_DEV_ADDR_LEN 6
#define BLE_ERR_INV_HCI_CMD_PARMS 0x12
#define BLE_GAP_PRIVATE_MODE_DEVICE 1
#define BLE_HCI_OGF_LE 8
#define BLE_HCI_OP(g,f) (((g)<<10)|(f))
#define BLE_HCI_OCF_LE_ADD_RESOLV_LIST 0x27
#define BLE_HCI_OCF_LE_RMV_RESOLV_LIST 0x28
#define BLE_HCI_OCF_LE_CLR_RESOLV_LIST 0x29
#define BLE_HCI_OCF_LE_SET_ADDR_RES_EN 0x2d
#define BLE_HCI_OCF_LE_SET_PRIVACY_MODE 0x4e
#define BLE_HS_EBUSY 15
struct ble_hci_le_add_resolv_list_cp { uint8_t peer_addr_type,peer_id_addr[6],peer_irk[16],local_irk[16]; } __attribute__((packed));
struct ble_hci_le_rmv_resolve_list_cp { uint8_t peer_addr_type,peer_id_addr[6]; } __attribute__((packed));
struct ble_hci_le_set_addr_res_en_cp { uint8_t enable; } __attribute__((packed));
struct ble_hci_le_set_privacy_mode_cp { uint8_t peer_id_addr_type,peer_id_addr[6],mode; } __attribute__((packed));
static uint8_t ble_hs_pvcy_irk[16],ble_hs_pvcy_default_irk[16];
static struct { int used; struct ble_hci_le_add_resolv_list_cp entry; } controller[4];
static int hci_calls,add_calls,remove_calls,clear_calls,duplicates,preempted,enabled=1;
static int fail_call,last_hci_rc;
static void ble_gap_preempt(void) { BLE_HS_DBG_ASSERT(!host_locked); ++preempted; }
static void ble_gap_preempt_done(void) { BLE_HS_DBG_ASSERT(preempted); --preempted; }
static int ble_hs_hci_cmd_tx(uint16_t opcode,const void *command,uint8_t size,void *reply,uint8_t reply_size) {
    (void)size; (void)reply; (void)reply_size; BLE_HS_DBG_ASSERT(!host_locked);
    BLE_HS_DBG_ASSERT(!fixture_adv && !fixture_disc && !fixture_links);
    ++hci_calls;
    if(hci_calls==fail_call) return last_hci_rc=BLE_HS_HCI_ERR(0x0c);
    const int ocf=opcode&1023;
    if(ocf==BLE_HCI_OCF_LE_ADD_RESOLV_LIST) {
        const struct ble_hci_le_add_resolv_list_cp *c=command; ++add_calls;
        BLE_HS_DBG_ASSERT(preempted);
        for(int i=0;i<4;++i) if(controller[i].used && controller[i].entry.peer_addr_type==c->peer_addr_type &&
            !memcmp(controller[i].entry.peer_id_addr,c->peer_id_addr,6)) {
            ++duplicates;
            printf("[PV-C] duplicate type%u id%u controllerIRK%u newIRK%u disk%u/%u newLTK%u rc%u\n",
                c->peer_addr_type,c->peer_id_addr[0],controller[i].entry.peer_irk[0],c->peer_irk[0],
                disk_count(1),disk_count(2),ble_store_config_peer_secs[0].ltk[0],BLE_HS_HCI_ERR(0x12));
            return last_hci_rc=BLE_HS_HCI_ERR(0x12);
        }
        for(int i=0;i<4;++i) if(!controller[i].used) { controller[i].used=1; controller[i].entry=*c; return 0; }
        abort();
    }
    if(ocf==BLE_HCI_OCF_LE_RMV_RESOLV_LIST) {
        const struct ble_hci_le_rmv_resolve_list_cp *c=command; ++remove_calls;
        for(int i=0;i<4;++i) if(controller[i].used && controller[i].entry.peer_addr_type==c->peer_addr_type &&
            !memcmp(controller[i].entry.peer_id_addr,c->peer_id_addr,6)) { controller[i].used=0; return 0; }
        return BLE_HS_HCI_ERR(2);
    }
    if(ocf==BLE_HCI_OCF_LE_CLR_RESOLV_LIST) { ++clear_calls; memset(controller,0,sizeof controller); return 0; }
    if(ocf==BLE_HCI_OCF_LE_SET_ADDR_RES_EN) { enabled=((const struct ble_hci_le_set_addr_res_en_cp *)command)->enable; return 0; }
    if(ocf==BLE_HCI_OCF_LE_SET_PRIVACY_MODE) return 0;
    abort();
}
int ble_hs_pvcy_set_mode(const ble_addr_t *,uint8_t);
int fixture_stop_calls,fixture_cancel_calls;
int ble_gap_adv_active(void) { return fixture_adv; }
int ble_gap_disc_active(void) { return fixture_disc; }
int ble_gap_disc_cancel(void) { ++fixture_cancel_calls; if(fixture_cancel_fail) return 15; fixture_disc=0; return 0; }
'''

ADAPTER = r'''
#include <atomic>
#include <cstdint>
#define ARDUINO 1
#define LITCH_BLE 1
#define LITCH_ESP32S3 1
#define BLE_HS_EALREADY 2
extern "C" {
int ble_store_clear(void),LitchSdkClearPeerStore(void),LitchSdkReconcileEmptyPeerStore(void);
int LitchSdkResetPeerPrivacy(void),ble_gap_adv_active(void),ble_gap_disc_active(void),ble_gap_disc_cancel(void);
extern int fixture_adv,fixture_disc,fixture_links,fixture_stop_fail,fixture_stop_calls;
}
namespace modules {
namespace ble_internal { constexpr uint16_t INVALID_CONN_HANDLE = 0xffffU; }
struct NimBLEAdvertising {
    bool isAdvertising() { return fixture_adv != 0; }
    bool stop() { ++fixture_stop_calls; if(fixture_stop_fail) return false; fixture_adv=0; return true; }
};
struct SettingsStore {};
int lifecycle_entries,owner_polls,teardown_entries,revocations,factory_quiesces;
bool release_on_poll;
class BleService {
public:
    uint32_t m_ownerClearToken=0;
    bool m_enabled=true;
    std::atomic<uint32_t> m_ownerNativeEpoch{41};
    bool Stop();
    bool ShutdownStack();
    void RevokeOwnerAdmission() { ++revocations; }
    bool QuiesceFactoryBleDut() { ++factory_quiesces; return true; }
    void Update(const SettingsStore&);
    void UpdateOwnerPairing() { ++owner_polls; if(release_on_poll) m_ownerClearToken=0; }
    std::atomic<bool> m_ownerQuiescing{true};
    std::atomic<uint16_t> m_ownerNativeHandle{0xffffU};
    NimBLEAdvertising advertising;
    NimBLEAdvertising* m_advertising = &advertising;
    bool OwnerNativeLinksQuiescent() const { return fixture_links == 0; }
    bool OwnerNativeGapQuiescent() const;
    bool QuiesceOwnerNativeGap();
    bool ClearOwnerNativeStore(void*);
    /* STOP_DECLARATION */
};
/* ACTUAL_ADAPTER */
/* ACTUAL_UPDATE_PREFIX */
/* ACTUAL_STOP_TEARDOWN_PREFIX */
}
extern "C" int adapter_clear(void) {
    modules::BleService service;
    return service.ClearOwnerNativeStore(&service);
}
extern "C" int adapter_lifecycle_gate(void) {
    modules::BleService service;
    modules::SettingsStore settings;
    service.m_ownerClearToken=42;
    // A pending disable or rename must not reach the lifecycle tail; poll and
    // release still run, and the next ordinary turn applies the deferred work.
    service.Update(settings);
    if(modules::owner_polls!=1 || modules::lifecycle_entries!=0) return 0;
    modules::release_on_poll=true;
    service.Update(settings);
    if(modules::owner_polls!=2 || modules::lifecycle_entries!=0 || service.m_ownerClearToken!=0) return 0;
    service.Update(settings);
    return modules::owner_polls==2 && modules::lifecycle_entries==1;
}
extern "C" int adapter_stop_gate(void) {
    modules::BleService service;
    service.m_ownerClearToken=42;
    if(service.Stop()) return 0; // Pending native ownership refuses sleep.
    if(service.m_ownerClearToken!=42 || service.m_ownerNativeEpoch!=41 ||
       modules::teardown_entries || modules::revocations || modules::factory_quiesces || service.m_enabled) return 0;
    service.m_ownerClearToken=0;
    if(!service.Stop()) return 0;
    return modules::teardown_entries==1 && modules::revocations==1 &&
        modules::factory_quiesces==1 && service.m_ownerNativeEpoch==0;
}
extern "C" int adapter_quiesce(void) {
    modules::BleService service;
    return service.QuiesceOwnerNativeGap();
}
'''

MAIN = r'''
int adapter_clear(void),adapter_quiesce(void),adapter_lifecycle_gate(void),adapter_stop_gate(void);
static int claim(int marker) {
    begin_claim();
    connection.bhc_peer_addr.type=1; connection.bhc_peer_addr.val[0]=42;
    procedure.peer_keys.addr[0]=42; procedure.peer_keys.addr_type=1;
    memset(procedure.our_keys.ltk,marker,16); memset(procedure.peer_keys.ltk,marker,16);
    procedure.peer_keys.irk_valid=1; memset(procedure.peer_keys.irk,marker+1,16);
    struct ble_sm_result result={0};
    ble_hs_lock(); ble_sm_key_exch_success(&procedure,&result); ble_hs_unlock();
    ble_sm_process_result(42,&result,true);
    return enc_status;
}
static int empty_peers(void) {
    for(int t=0;t<4;++t) for(int s=0;s<8;++s) if(disk[t][s].present) return 0;
    for(int s=0;s<8;++s) if(disk[5][s].present) return 0;
    return ble_store_config_num_our_secs==0 && ble_store_config_num_peer_secs==0 &&
        ble_store_config_num_cccds==0 && ble_store_config_num_csfcs==0 && ble_store_config_num_rpa_recs==0;
}
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    ble_store_config_init(); ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    ble_hs_cfg.store_write_cb=counted_store_write;
    memset(ble_hs_pvcy_irk,73,sizeof ble_hs_pvcy_irk);
    CHECK(claim(17)==0 && enc_bonded && pair_status==0);
    for(int t=3;t<=4;++t) {
        union ble_store_value v={0};
        if(t==3) { v.cccd.peer_addr=value(42,0).peer_addr; v.cccd.chr_val_handle=7; }
        else v.csfc.peer_addr=value(42,0).peer_addr;
        CHECK(ble_store_write(t==3?BLE_STORE_OBJ_TYPE_CCCD:BLE_STORE_OBJ_TYPE_CSFC,&v)==0);
    }
    ble_store_config_num_local_irks=1;
    ble_store_config_local_irks[0].addr=value(91,0).peer_addr;
    memset(ble_store_config_local_irks[0].irk,73,16);
    ble_store_config_num_rpa_recs=1;
    ble_store_config_rpa_recs[0].peer_addr=value(92,0).peer_addr;
    ble_store_config_rpa_recs[0].peer_rpa_addr=value(93,0).peer_addr;
    for(int t=4;t<6;++t) {
        disk[t][0].present=true;
        disk[t][0].size=t==4?sizeof ble_store_config_local_irks[0]:sizeof ble_store_config_rpa_recs[0];
        memcpy(disk[t][0].data,t==4?(void*)ble_store_config_local_irks:(void*)ble_store_config_rpa_recs,disk[t][0].size);
    }
    struct blob retained[8]; memcpy(retained,disk[4],sizeof retained);
    if(which>=18) {
        /* Legacy/factory-reset residue: no security keys, but a full cache.
         * Restart after every failed leaf before retrying durable cleanup. */
        ble_store_config_num_our_secs=ble_store_config_num_peer_secs=0;
        ble_store_config_num_cccds=ble_store_config_num_csfcs=0;
        CHECK(ble_store_config_persist_our_secs()==0 && ble_store_config_persist_peer_secs()==0 &&
              ble_store_config_persist_cccds()==0 && ble_store_config_persist_csfcs()==0);
        for(int i=0;i<3;++i) {
            ble_store_config_rpa_recs[i].peer_addr=value(92+i,0).peer_addr;
            ble_store_config_rpa_recs[i].peer_rpa_addr=value(95+i,0).peer_addr;
        }
        ble_store_config_num_rpa_recs=3; CHECK(ble_store_config_persist_rpa_recs()==0);
        ble_store_config_init(); CHECK(litch_nimble_store_restore_status==0);
        int metadata=0; CHECK(LitchSdkReadPeerMetadata(&metadata)==0 && metadata);
        if(which==19) fault(BEFORE,6);
        if(which==20) fault(AFTER,6);
        if(which==21) read_fault_at=read_calls+1;
        if(which==22) fail_call=hci_calls+5;
        if(which==23) fault(DROP,6);
        int cleared=adapter_clear();
        CHECK(cleared==(which==18));
        fail_call=0;read_fault_at=0;
        ble_store_config_init(); CHECK(litch_nimble_store_restore_status==0);
        CHECK(adapter_clear() && empty_peers());
        ble_store_config_init(); CHECK(litch_nimble_store_restore_status==0);
        CHECK(LitchSdkReadPeerMetadata(&metadata)==0 && !metadata && empty_peers());
        CHECK(!memcmp(retained,disk[4],sizeof retained) && ble_store_config_num_local_irks==1);
        CHECK(claim(34)==0 && enc_bonded);
        CHECK(opened==closed && !host_locked && !preempted);
        printf("clear metadata-only case%d reopened_empty1 local_identity1 fresh_claim1\n",which);
        return 0;
    }
    if(which>=14 && which<=17) {
        CHECK(adapter_clear() && empty_peers());
        /* Failed actual publication leaves hidden durable data behind empty
         * restored RAM. A proof read fault must prevent the privacy/ACK seam. */
        int type=which-13;
        union ble_store_value v={0};
        if(type<=2) v.sec=value(42,51);
        else if(type==3) { v.cccd.peer_addr=value(42,0).peer_addr; v.cccd.chr_val_handle=7; }
        else v.csfc.peer_addr=value(42,0).peer_addr;
        fault(AFTER,type);
        CHECK(ble_store_write(type==4?BLE_STORE_OBJ_TYPE_CSFC:type,&v)==BLE_HS_ESTORE_FAIL);
        read_fault_at=read_calls+1;
    }
    int before_hci=hci_calls;
    if(which>=1 && which<=5) fail_call=hci_calls+which;
    if(which>=6 && which<=9) fault(AFTER,which-5);
    if(which==10) fixture_adv=1;
    if(which==11) fixture_disc=1;
    if(which==12) fixture_links=1;
    int rc=adapter_clear();
    if((which>=1 && which<=12) || which>=14) {
        CHECK(!rc);
        CHECK(enc_events==1); /* No re-admission after failed cleanup. */
        if(which>=6) CHECK(hci_calls==before_hci);
        fail_call=0; read_fault_at=0; fixture_adv=fixture_disc=fixture_links=0;
        CHECK(adapter_clear());
    } else CHECK(rc);
    CHECK(empty_peers());
    if(which!=0) CHECK(!memcmp(retained,&disk[4],sizeof retained));
    if(which!=0) CHECK(ble_store_config_num_local_irks==1 && ble_store_config_num_rpa_recs==0);
    if(which!=0) for(int i=0;i<16;++i) CHECK(ble_hs_pvcy_irk[i]==73 && ble_store_config_local_irks[0].irk[i]==73);
    CHECK(enabled && claim(34)==0 && enc_bonded && pair_status==0 && duplicates==0);
    CHECK(ble_store_config_peer_secs[0].ltk[0]==34);
    ble_store_config_init();
    CHECK(ble_store_config_num_our_secs==1 && ble_store_config_num_peer_secs==1);
    CHECK(ble_store_config_num_local_irks==1 && ble_store_config_num_rpa_recs==0);
    CHECK(!memcmp(retained,&disk[4],sizeof retained));
    if(which==13) {
        CHECK(adapter_lifecycle_gate() && adapter_stop_gate());
        fixture_adv=fixture_disc=fixture_stop_fail=1;
        CHECK(!adapter_quiesce() && fixture_adv && fixture_disc && fixture_cancel_calls==0);
        fixture_stop_fail=0; fixture_cancel_fail=1;
        CHECK(!adapter_quiesce() && !fixture_adv && fixture_disc);
        fixture_cancel_fail=0; CHECK(adapter_quiesce() && !fixture_adv && !fixture_disc);
    }
    CHECK(opened==closed && !host_locked && !preempted);
    printf("clear case%d PASS warm_same_identity1 identity_retained1 reopened1 HCIclear%d\n",which,clear_calls);
    return 0;
}
'''


class PeerClearTests(unittest.TestCase):
    def test_actual_clear_preserves_identity_clears_controller_and_checks_failures(self):
        compiler=shutil.which('clang'); self.assertIsNotNone(compiler)
        adapter=NativeFunction('ClearOwnerNativeStore')
        native=(REPO / 'src/ble_service_owner_native.cpp').read_text()
        if 'bool BleService::OwnerNativeGapQuiescent(' in native:
            adapter += '\n' + NativeFunction('OwnerNativeGapQuiescent')
            adapter += '\n' + NativeFunction('QuiesceOwnerNativeGap')
        else:
            # OLD control lacks the checked GAP gate; the main regression must
            # still compile and fail at its changed observable behavior.
            adapter += '\nbool BleService::QuiesceOwnerNativeGap() { return true; }'
        stack=(REPO / 'src/ble_service_stack.cpp').read_text()
        start=stack.index('BleService::StopAdvertising(')
        begin=stack.rfind('\n',0,start)+1
        stop=stack[begin:stack.index('\n}',start)+2]
        declaration='bool StopAdvertising();' if stop.startswith('bool') else 'void StopAdvertising();'
        runtime=(REPO / 'src/ble_service_runtime_update.cpp').read_text()
        start=runtime.index('void BleService::Update(')
        end=runtime.index('    if (!m_recoveryOnly) m_settings',start)
        # Keep the exact entry/guard, with the following lifecycle tail as an
        # observed leaf. Actual owner-result polling remains an aggregate gate.
        prefix=runtime[start:end]+'    ++lifecycle_entries;\n#endif\n}\n'
        base=(REPO / 'src/ble_service_runtime.cpp').read_text()
        start=base.index('bool BleService::Stop()')
        direct_stop=base[start:base.index('\n}',start)+2]
        start=stack.index('bool BleService::ShutdownStack()')
        end=stack.index('    StopAdvertising();',start)+len('    StopAdvertising();')
        shutdown=stack[start:end]+'\n    ++teardown_entries;\n    return true;\n}\n'
        source=ADAPTER.replace('/* STOP_DECLARATION */',declaration).replace(
            '/* ACTUAL_ADAPTER */',adapter+'\n'+stop).replace('/* ACTUAL_UPDATE_PREFIX */',prefix).replace(
            '/* ACTUAL_STOP_TEARDOWN_PREFIX */',direct_stop+'\n'+shutdown)
        with tempfile.TemporaryDirectory(prefix='clear-fix-') as temporary:
            root=Path(temporary); c=root/'corpus.c'; cpp=root/'adapter.cpp'; binary=root/'clear'
            c.write_text(ClearCorpus()); cpp.write_text(source)
            for src,extra in [(c,['-std=c11','-DWITH_ADMISSION_HOOK=1']),(cpp,['-std=c++17'])]:
                build=subprocess.run([compiler,*extra,'-O1','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all','-I',str(SCRIPT.parent),'-c',str(src),'-o',str(src.with_suffix('.o'))],capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
            build=subprocess.run([compiler+'++','-fsanitize=address,undefined',str(c.with_suffix('.o')),
                str(cpp.with_suffix('.o')),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            for case in range(24):
                with self.subTest(case=case):
                    run=subprocess.run([str(binary),str(case)],capture_output=True,text=True,timeout=5)
                    print(run.stdout.strip(),run.stderr.strip())
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)
                    self.assertEqual(run.stderr,'')


    def test_failed_connect_restart_waits_for_main_loop_quiescence_gate(self):
        original=(FIXTURE / 'NimBLEServer.cpp').read_bytes()
        patched=hardening.HardenServerSource(original)
        def connect_case(source):
            source=source.decode()
            start=source.index('        case BLE_GAP_EVENT_CONNECT: {')
            end=source.index('        } // BLE_GAP_EVENT_CONNECT',start)
            return source[start:end+len('        } // BLE_GAP_EVENT_CONNECT')]
        runtime=(REPO / 'src/ble_service_runtime.cpp').read_text()
        start=runtime.index('void BleService::StartAdvertising()')
        actual_start=runtime[start:runtime.index('\n}',start)+2]
        harness=r"""
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define LITCH_BLE 1
#define CONFIG_BT_NIMBLE_EXT_ADV 0
#define CONFIG_BT_NIMBLE_ROLE_BROADCASTER 1
#define BLE_GAP_EVENT_CONNECT 1
#define BLE_ERR_UNSUPP_REM_FEATURE 26
#define BLE_HS_CONN_HANDLE_NONE 65535
#define NIMBLE_LOGE(...) ((void)0)
namespace logger {
template<class... Args> void Info(const char*,Args...) {}
template<class... Args> void Warn(const char*,Args...) {}
}
namespace modules {
struct BleConnectionChangeEvent { bool connected=false; uint32_t connectionEpoch=0; };
struct Projection { bool TryPeek(BleConnectionChangeEvent&) { return false; } };
struct Settings { uint32_t CurrentConnectionEpoch() { return 0; } };
struct Advertising {
    bool active=false;
    int starts=0;
    bool isAdvertising() { return active; }
    bool start() { ++starts; active=true; return true; }
};
class BleService {
public:
    std::atomic<bool> m_ownerQuiescing{true},m_connected{false};
    bool m_initialized=true,m_enabled=true;
    Projection m_connectionProjection;
    Settings m_bleSettings;
    Advertising advertising;
    Advertising* m_advertising=&advertising;
    void StartAdvertising();
};
/* ACTUAL_MAIN_START */
}
int sdk_starts=0,connected_callbacks=0;
struct NimBLEDevice { static void startAdvertising() { ++sdk_starts; } };
struct Descriptor {};
struct PeerInfo { Descriptor m_desc; };
struct Callbacks { void onConnect(void*,PeerInfo&) { ++connected_callbacks; } } callbacks;
struct Server {
    std::array<uint16_t,1> m_connectedPeers{BLE_HS_CONN_HANDLE_NONE};
    Callbacks* m_pServerCallbacks=&callbacks;
} server;
struct Event { int type; struct { int status; uint16_t conn_handle; } connect; };
int ble_gap_conn_find(uint16_t,Descriptor*) { return 0; }
/* SDK_CONNECT_CASES */
int main(int argc,char** argv) {
    bool old=argc>1 && !std::strcmp(argv[1],"old");
    int failures=0;
    for(int status : {1,BLE_ERR_UNSUPP_REM_FEATURE,0}) {
        sdk_starts=connected_callbacks=0;
        Event event{BLE_GAP_EVENT_CONNECT,{status,42}};
        modules::BleService service; // GAP is inactive; clear owns quiescence.
        if(old) Old(&event); else New(&event);
        service.StartAdvertising();
        bool safe=sdk_starts==0 && service.advertising.starts==0 &&
            connected_callbacks==(status==1?0:1);
        service.m_ownerQuiescing=false;
        service.StartAdvertising();
        safe=safe && service.advertising.starts==1;
        failures+=!safe;
        std::printf("connect mode=%s status%d SDKstarts%d mainStarts%d connected%d safe%d\n",
            old?"old":"new",status,sdk_starts,service.advertising.starts,connected_callbacks,safe);
    }
    return failures?1:0;
}
"""
        branches=''
        for name,source in [('Old',original),('New',patched)]:
            branches+=f'int {name}(Event* event) {{ int rc=0; PeerInfo peerInfo; Server* pServer=&server; switch(event->type) {{\n'
            branches+=connect_case(source)+'\n} return 0; }\n'
        harness=harness.replace('/* ACTUAL_MAIN_START */',actual_start).replace('/* SDK_CONNECT_CASES */',branches)
        compiler=shutil.which('clang++'); self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix='clear-fix-') as temporary:
            root=Path(temporary); source=root/'restart.cpp'; binary=root/'restart'
            source.write_text(harness)
            build=subprocess.run([compiler,'-std=c++17','-O1','-fsanitize=address,undefined',
                '-fno-sanitize-recover=all',str(source),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            for label,code in [('old',1),('new',0)]:
                run=subprocess.run([str(binary),label],capture_output=True,text=True,timeout=5)
                print(run.stdout.strip(),run.stderr.strip())
                self.assertEqual(run.returncode,code,run.stdout+run.stderr)
                self.assertEqual(run.stderr,'')


if __name__=='__main__':
    unittest.main()
