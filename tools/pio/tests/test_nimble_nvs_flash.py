"""Actual generated NVS leaves; controlled SDK storage and admission failures."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_nimble_security_store import Corpus, FIXTURE, Function, hardening
from test_nimble_csfc_persistence import CsfcCorpus


LEAF_JOURNEY = r'''
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    struct ble_store_value_sec item=value(1,17);
    union ble_store_value read={0};
    bool present=false;
    guard_required=1;
    CHECK(ble_nvs_write_key_value("our_sec_1",&item,sizeof item)==0);
    CHECK(get_nvs_db_value(BLE_STORE_OBJ_TYPE_OUR_SEC,"our_sec_1",&read)==0);
    CHECK(litch_read_native_slot(BLE_STORE_OBJ_TYPE_OUR_SEC,1,&read,sizeof item,&present)==0 && present);
    CHECK(unguarded_ops==0 && guard_active==0 && guard_begins==guard_ends);
    int operations=opened+read_calls+writes+commits;
    int released=guard_ends;
    guard_denied=1;
    CHECK(ble_nvs_write_key_value("our_sec_1",&item,sizeof item)==BLE_HS_ESTORE_FAIL);
    CHECK(ble_nvs_delete_value(BLE_STORE_OBJ_TYPE_OUR_SEC,1)==BLE_HS_ESTORE_FAIL);
    CHECK(get_nvs_db_value(BLE_STORE_OBJ_TYPE_OUR_SEC,"our_sec_1",&read)==BLE_HS_ESTORE_FAIL);
    CHECK(litch_read_native_slot(BLE_STORE_OBJ_TYPE_OUR_SEC,1,&read,sizeof item,&present)==BLE_HS_ESTORE_FAIL);
    CHECK(operations==opened+read_calls+writes+commits && released==guard_ends && !guard_active);
    guard_denied=0;
    /* Every real open/get/set/commit error must release its acquired lease. */
    open_fault=1; CHECK(ble_nvs_write_key_value("our_sec_1",&item,sizeof item)==BLE_HS_ESTORE_FAIL);
    open_fault=1; CHECK(ble_nvs_delete_value(BLE_STORE_OBJ_TYPE_OUR_SEC,1)==BLE_HS_ESTORE_FAIL);
    open_fault=1; CHECK(get_nvs_db_value(BLE_STORE_OBJ_TYPE_OUR_SEC,"our_sec_1",&read)==BLE_HS_ESTORE_FAIL);
    open_fault=1; CHECK(litch_read_native_slot(BLE_STORE_OBJ_TYPE_OUR_SEC,1,&read,sizeof item,&present)==BLE_HS_ESTORE_FAIL);
    set_failure=1; CHECK(ble_nvs_write_key_value("our_sec_1",&item,sizeof item)==BLE_HS_ESTORE_FAIL);
    fault(BEFORE,1); CHECK(ble_nvs_write_key_value("our_sec_1",&item,sizeof item)==BLE_HS_ESTORE_FAIL);
    read_fault=1; CHECK(get_nvs_db_value(BLE_STORE_OBJ_TYPE_OUR_SEC,"our_sec_1",&read)==BLE_HS_ESTORE_FAIL);
    read_fault=1; CHECK(litch_read_native_slot(BLE_STORE_OBJ_TYPE_OUR_SEC,1,&read,sizeof item,&present)==BLE_HS_ESTORE_FAIL);
    CHECK(ble_nvs_delete_value(BLE_STORE_OBJ_TYPE_OUR_SEC,1)==0);
    CHECK(ble_nvs_delete_value(BLE_STORE_OBJ_TYPE_OUR_SEC,1)==BLE_HS_ESTORE_FAIL);
    CHECK(!guard_active && guard_begins-4==guard_ends && opened==closed && unguarded_ops==0);
    absent_namespace=1;
    CHECK(get_nvs_db_value(BLE_STORE_OBJ_TYPE_OUR_SEC,"our_sec_1",&read)==ESP_ERR_NVS_NOT_FOUND);
    CHECK(litch_read_native_slot(BLE_STORE_OBJ_TYPE_OUR_SEC,1,&read,sizeof item,&present)==0 && !present);
    absent_namespace=0;
    const int types[]={BLE_STORE_OBJ_TYPE_OUR_SEC,BLE_STORE_OBJ_TYPE_PEER_SEC,
        BLE_STORE_OBJ_TYPE_CCCD,BLE_STORE_OBJ_TYPE_CSFC,BLE_STORE_OBJ_TYPE_LOCAL_IRK,BLE_STORE_OBJ_TYPE_PEER_ADDR};
    for(unsigned i=0;i<sizeof types/sizeof types[0];++i) {
        union ble_store_value v={0}; v.sec=item;
        CHECK(ble_store_nvs_write(types[i],&v)==0);
        CHECK(!guard_active && unguarded_ops==0);
    }
    CHECK(opened==closed && guard_begins-4==guard_ends);
    printf("[NATIVE_NVS_FLASH] begins=%d ends=%d denied=4 unguarded=%d handles=%d/%d\n",guard_begins,guard_ends,unguarded_ops,opened,closed);
    return 0;
}
'''


class NativeNvsFlashTests(unittest.TestCase):
    def test_actual_stop_failure_keeps_tree_and_binding_until_retry(self):
        repo=Path(hardening.__file__).parents[2]
        stack=(repo/'src/ble_service_stack.cpp').read_text()
        start=stack.index('bool BleService::StopNativeStack()')
        end=stack.index('\n}',start)+2
        stop=stack[start:end]
        self.assertEqual(stack.count('if (StopNativeStack()) g_bleService = nullptr;'),2)
        shutdown=stack[stack.index('bool BleService::ShutdownStack()'):]
        self.assertLess(shutdown.index('if (ownedStack && !StopNativeStack()) return false;'),
                        shutdown.index('m_server = nullptr;'))
        self.assertLess(shutdown.index('if (ownedStack && !StopNativeStack()) return false;'),
                        shutdown.index('ble_internal::g_bleService = nullptr;'))
        init=stack[stack.index('void BleService::InitStack('):]
        self.assertLess(init.index('g_bleService = this;'),init.index('NimBLEDevice::init('))
        self.assertLess(init.index('if (g_bleService == this) {'),init.index('g_bleService = this;'))
        code=r'''
#include <cstdio>
static bool host=true,tree=true,fail_stop=true;
static int destroyed=0,stops=0;
struct NimBLEDevice {
    static bool deinit(bool clearAll) {
        bool ok=true;
        if(host) { ++stops; ok=!fail_stop; if(ok) host=false; }
        if(clearAll) { tree=false; ++destroyed; }
        return ok;
    }
};
namespace logger { void Warn(const char*) {} }
struct BleService { bool m_initialized=true,m_enabled=true; bool StopNativeStack(); };
'''+stop+r'''
int main() {
    BleService service; BleService *binding=&service;
    if(service.StopNativeStack()) binding=nullptr;
    if(binding!=&service || !host || !tree || destroyed || service.m_initialized || service.m_enabled) return 1;
    fail_stop=false;
    if(service.StopNativeStack()) binding=nullptr;
    if(binding || host || tree || destroyed!=1 || stops!=2) return 2;
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='litch-nvs-stop-') as temporary:
            path=Path(temporary)/'stop.cpp'; path.write_text(code); binary=path.with_suffix('')
            build=subprocess.run(['c++','-std=c++17',str(path),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            run=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)

    def test_controller_shutdown_error_cannot_publish_stopped(self):
        repo = Path(hardening.__file__).parents[2]
        stack = (repo / 'src/ble_service_stack.cpp').read_text()
        start = stack.index('bool BleService::StopNativeStack()')
        stop = stack[start:stack.index('\n}', start) + 2]
        fixture = FIXTURE / 'controller_stop_functions.inc'
        code = r"""
#include <cstdio>
#include <initializer_list>
#define ARDUINO 1
#define LITCH_ESP32S3 1
#define ESP_PLATFORM 1
#define CONFIG_BT_CONTROLLER_ENABLED 1
#define CONFIG_BT_LE_CONTROLLER_NPL_OS_PORTING_SUPPORT 0
#define BT_HCI_LOG_INCLUDED 0
#define CONFIG_BT_NIMBLE_ROLE_PERIPHERAL 1
#define CONFIG_BT_NIMBLE_ROLE_BROADCASTER 0
#define CONFIG_BT_NIMBLE_ROLE_OBSERVER 0
#define CONFIG_BT_NIMBLE_ROLE_CENTRAL 0
#define NIMBLE_LOGE(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_BT_CONTROLLER_STATUS_IDLE = 0;
static int fault, controller, native_calls, destroyed;
struct NativeTree { ~NativeTree() { ++destroyed; } };
int nimble_port_stop() { ++native_calls; return fault == 4 ? -1 : 0; }
int esp_nimble_deinit() { return fault == 1 ? -1 : 0; }
int esp_bt_controller_disable() { if (fault == 2) return -1; controller = 1; return 0; }
int esp_bt_controller_deinit() { if (fault == 3) return -1; controller = 0; return 0; }
int esp_bt_controller_get_status() { return controller; }
struct NimBLEDevice {
    static inline bool m_initialized, m_synced;
    static inline NativeTree* m_pServer;
    static bool deinit(bool);
};
namespace logger { template<typename... Args> void Warn(Args...) {} }
struct BleService { bool m_initialized = true, m_enabled = true; bool StopNativeStack(); };
""" + fixture.read_text() + '\n' + stop + r"""
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line=%d fault=%d controller=%d\n", __LINE__, fault, controller); return 1; } } while (0)
int main() {
    for (const int failure : {0, 1, 2, 3, 4}) {
        fault = failure; controller = 2; native_calls = destroyed = 0;
        NimBLEDevice::m_initialized = NimBLEDevice::m_synced = true;
        NimBLEDevice::m_pServer = new NativeTree;
        BleService service;
        BleService* binding = &service;
        const bool first = service.StopNativeStack();
        if (first) binding = nullptr;
        const bool retry = service.StopNativeStack();
        if (retry) binding = nullptr;
        CHECK(first == (fault == 0) && retry == (fault == 0));
        if (fault == 0) {
            CHECK(controller == 0 && binding == nullptr && destroyed == 1);
            CHECK(NimBLEDevice::m_pServer == nullptr);
        } else {
            CHECK(controller != 0 && binding == &service && destroyed == 0);
            CHECK(NimBLEDevice::m_pServer != nullptr && !service.m_initialized && !service.m_enabled);
        }
        std::printf("[CONTROLLER_STOP] fault=%d first=%u retry=%u controller=%d native_calls=%d tree_destroyed=%d binding_retained=%u\n",
            fault, first, retry, controller, native_calls, destroyed, binding != nullptr);
        delete NimBLEDevice::m_pServer;
        NimBLEDevice::m_pServer = nullptr;
    }
}
"""
        with tempfile.TemporaryDirectory(prefix='litch-controller-stop-') as temporary:
            source = Path(temporary) / 'stop.cpp'
            binary = Path(temporary) / 'stop'
            source.write_text(code)
            build = subprocess.run(['c++', '-std=c++17', str(source), '-o', str(binary)],
                                   capture_output=True, text=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout, end='')

    def test_production_acquisition_bounds_wrap_and_current_transfers(self):
        repo=Path(hardening.__file__).parents[2]
        native=(repo/'src/ble_service_owner_native.cpp').read_text()
        def method(name, result):
            start=native.index(result+' BleService::'+name+'(')
            end=native.index('{',start)+1; depth=1
            while depth:
                depth+=(native[end]=='{')-(native[end]=='}'); end+=1
            return native[start:end]
        code=r'''
#include <cstdint>
#include <cstdio>
#include "hardware/rmt_flash_arbiter.hpp"
using TickType_t=uint32_t;
static TickType_t ticks=0;
static unsigned mode=0;
static hardware::RmtFlashArbiter *current=nullptr;
TickType_t xTaskGetTickCount() { return ticks; }
#define pdMS_TO_TICKS(ms) (ms)
namespace fxe { uint32_t Millis() { return ticks; } }
void vTaskDelay(TickType_t amount) {
    ticks+=amount;
    if(mode==1 && ticks==5) current->EndRender();
    if(mode==2 && ticks==10) {
        current->EndFlash();
        if(current->TryBeginRequestedFlash() || !current->TryBeginRequestedRender()) __builtin_abort();
        mode=3;
    } else if(mode==3 && ticks==15) {
        current->EndRender(); mode=0;
    }
}
namespace modules {
struct BleService {
    hardware::RmtFlashArbiter *m_nativeStoreFlashArbiter=nullptr;
    bool BeginNativeStoreFlashAccess(); void EndNativeStoreFlashAccess();
};
'''+method('BeginNativeStoreFlashAccess','bool')+'\n'+method('EndNativeStoreFlashAccess','void')+r'''
}
#define CHECK(value) do { if(!(value)) return __LINE__; } while(0)
int main() {
    hardware::RmtFlashArbiter arbiter; current=&arbiter;
    modules::BleService service; CHECK(!service.BeginNativeStoreFlashAccess());
    service.m_nativeStoreFlashArbiter=&arbiter;
    // Idle native storage needs no future frame or scheduler delay.
    CHECK(service.BeginNativeStoreFlashAccess() && ticks==0);
    CHECK(arbiter.CompletedRenderGeneration()==0 && !arbiter.TryBeginRender());
    service.EndNativeStoreFlashAccess();
    CHECK(arbiter.TryBeginRender()); mode=1;
    CHECK(service.BeginNativeStoreFlashAccess() && ticks==5);
    CHECK(!arbiter.TryBeginRender());
    service.EndNativeStoreFlashAccess();
    // Complete the active primitive, then the already-ready frame, before NVS.
    CHECK(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()));
    arbiter.RequestRender();
    mode=2; CHECK(service.BeginNativeStoreFlashAccess() && ticks==15 && mode==0);
    CHECK(!arbiter.TryBeginRender());
    service.EndNativeStoreFlashAccess();
    // Timeout cancels only this caller, preserving another active and queued owner.
    CHECK(arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()));
    arbiter.RequestFlash();
    const auto waiting=ticks;
    CHECK(!service.BeginNativeStoreFlashAccess() && TickType_t(ticks-waiting)==100);
    CHECK(!arbiter.TryBeginRender() && !arbiter.TryBeginRequestedFlash());
    arbiter.EndFlash();
    CHECK(arbiter.TryBeginRequestedFlash()); arbiter.EndFlash();
    CHECK(arbiter.TryBeginRender()); arbiter.EndRender(false);
    ticks=UINT32_MAX-50; const auto started=ticks;
    CHECK(!service.BeginNativeStoreFlashAccess() && TickType_t(ticks-started)==100);
    CHECK(!arbiter.TryBeginFlash(arbiter.CompletedRenderGeneration()));
    CHECK(arbiter.TryBeginRender()); arbiter.EndRender();
    // An ordinary frame after recovery proves the timed-out claim was retired.
    CHECK(arbiter.TryBeginRender()); arbiter.EndRender();
    CHECK(service.BeginNativeStoreFlashAccess()); service.EndNativeStoreFlashAccess();
    std::puts("[NVS_ACQUIRE] idle_wait=0 RMT_wait=5 ready_turn=PASS caller_cancel=PASS wrap_timeout=100 uncertain_preserved=PASS");
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='litch-nvs-arbiter-') as temporary:
            path=Path(temporary)/'port.cpp'; path.write_text(code); binary=path.with_suffix('')
            build=subprocess.run(['c++','-std=c++17','-DARDUINO','-DLITCH_ESP32S3',
                                  '-I',str(repo/'include'),'-I',str(repo/'lib/foxie_embedded/include'),
                                  str(path),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            run=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            print(run.stdout.strip())

    def test_exact_boot_nvs_block_releases_before_host_sync(self):
        code=r'''
#include <cstdio>
static int denied=0, held=0, begins=0, ends=0, operations=0, erases=0, fail=0;
extern "C" int LitchSdkNvsBeginAccess(void) { ++begins; if(denied) return 28; held=1; return 0; }
extern "C" void LitchSdkNvsEndAccess(void) { if(!held) __builtin_abort(); held=0; ++ends; }
using esp_err_t=int;
#define ESP_OK 0
#define ESP_ERR_NVS_NO_FREE_PAGES 1
#define ESP_ERR_NVS_NEW_VERSION_FOUND 2
int nvs_flash_init() {
    if(!held) __builtin_abort(); ++operations;
    if(operations==1 && (fail==1 || fail==2 || fail==4 || fail==5)) return fail==2 || fail==5 ? 2 : 1;
    return fail==3 || fail==4 ? 3 : 0;
}
int nvs_flash_erase() { if(!held) __builtin_abort(); ++erases; return fail==2 ? 3 : 0; }
bool Init() {
'''+hardening.DEVICE_NVS_INIT.decode()+r'''
        if (err != ESP_OK) return false;
        if(held) __builtin_abort(); // real host synchronization starts after this block
        return true;
}
int main() {
    denied=1; if(Init() || operations || ends) return 1;
    denied=0; if(!Init() || held || ends!=1) return 2;
    operations=0; fail=1; if(!Init() || erases!=1 || operations!=2 || held) return 3;
    fail=3; if(Init() || held) return 4;
    operations=0; fail=2; if(Init() || held) return 5;
    operations=0; fail=4; if(Init() || held) return 6;
    operations=0; fail=5; if(!Init() || held) return 7;
    if(begins!=ends+1) return 8;
    return 0;
}
'''
        guarded=hardening.HardenDeviceSource(code.encode()).decode()
        with self.assertRaises(ValueError):
            hardening.HardenDeviceSource(code.encode().replace(b'nvs_flash_erase()',b'changed()'))
        with tempfile.TemporaryDirectory(prefix='litch-nvs-init-') as temporary:
            path=Path(temporary)/'init.cpp'; path.write_text(guarded); binary=path.with_suffix('')
            build=subprocess.run(['c++','-std=c++17',str(path),'-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            run=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)

    def compile_and_run(self, source):
        with tempfile.TemporaryDirectory(prefix='litch-native-nvs-') as temporary:
            root=Path(temporary); path=root/'leaves.c'; path.write_text(source)
            build=subprocess.run([shutil.which('clang'),'-std=c11','-O1','-fsanitize=address,undefined',
                                  '-fno-sanitize-recover=all','-I',str(Path(hardening.__file__).parent),
                                  str(path),'-o',str(root/'leaves')],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            run=subprocess.run([str(root/'leaves')],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            print(run.stdout.strip())

    def test_actual_nvs_leaves_balance_admission_and_fail_before_io(self):
        config=hardening.HardenConfigSource((FIXTURE/'ble_store_config.c').read_bytes())
        nvs=hardening.HardenNvsSource((FIXTURE/'ble_store_nvs.c').read_bytes())
        source=Corpus(config.decode(),nvs.decode(),(FIXTURE/'ble_store_util.c').read_text(),True)
        source=source[:source.index('int main(')]+LEAF_JOURNEY
        self.compile_and_run(source)

    def test_actual_cccd_failure_preserves_live_flags_and_identical_retry(self):
        gatts=hardening.HardenGattsSource((FIXTURE/'ble_gatts.c').read_bytes()).decode()
        source=CsfcCorpus(gatts)
        source=source[:source.index('int main(')]
        source=source.replace('struct ble_gatts_clt_cfg { uint16_t chr_val_handle,flags; };',
                              'struct ble_gatts_clt_cfg { uint16_t chr_val_handle,flags,allowed; };')
        source=source.replace('(void)cfg; (void)handle; abort();',
                              'return cfg && cfg->chr_val_handle==handle ? cfg : NULL;',1)
        source=source.replace('static void ble_gatts_subscribe_event(',
                              'static int subscribe_events;\nstatic void ble_gatts_subscribe_event(',1)
        source=source.replace('(void)a; (void)b; (void)c; (void)d; (void)e; abort();',
                              '(void)a; (void)b; (void)c; (void)d; (void)e; ++subscribe_events;',1)
        source+=r'''
#define BLE_GATT_ACCESS_OP_READ_DSC 0
#define BLE_GATT_ACCESS_OP_WRITE_DSC 1
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_ATT_ERR_REQ_NOT_SUPPORTED 6
#define BLE_GAP_SUBSCRIBE_REASON_WRITE 2
#define BLE_GATTS_CLT_CFG_F_RESERVED 0x80
#define STATS_INC(...) ((void)0)
#define OS_MBUF_PKTLEN(om) ((om)->len)
#define om_data data
static int ble_gatts_dsc_op(int op) { return op; }
static void ble_gatts_dsc_inc_stat(int op) { (void)op; }
static void *os_mbuf_extend(struct os_mbuf *om,int size) {
    void *result=om->data+om->len; om->len+=(uint16_t)size; return result;
}
static struct os_mbuf *os_mbuf_pullup(struct os_mbuf *om,int size) { (void)size; return om; }
static uint16_t get_le16(const void *p) { const uint8_t *b=p; return b[0]|(uint16_t)b[1]<<8; }
static void put_le16(void *p,uint16_t value) { uint8_t *b=p; b[0]=(uint8_t)value; b[1]=(uint8_t)(value>>8); }
'''
        source+='\n'+Function(gatts,'ble_gatts_clt_cfg_access_locked')
        source+='\n'+Function((FIXTURE/'ble_store.c').read_text(),'ble_store_delete_cccd')
        source+='\n'+Function(gatts,'ble_gatts_clt_cfg_access')
        source+=r'''
static int cccd_request(uint8_t flags) {
    struct os_mbuf mbuf={.data={flags,0},.len=2}; struct os_mbuf *om=&mbuf;
    return ble_gatts_clt_cfg_access(1,10,BLE_GATT_ACCESS_OP_WRITE_DSC,0,&om,NULL);
}
int main(void) {
    int which=0;
    ble_store_config_init(); ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    live.bhc_peer_addr=value(1,0).peer_addr; live.bhc_sec_state.bonded=true;
    struct ble_gatts_clt_cfg cfg={.chr_val_handle=9,.flags=0,.allowed=3};
    live.bhc_gatt_svr.clt_cfgs=&cfg;
    set_failure=1;
    int rc=cccd_request(1);
    printf("[CCCD_FAILED_ENABLE] rc=%d flags=%u events=%d\n",rc,cfg.flags,subscribe_events);
    CHECK(rc==BLE_ATT_ERR_UNLIKELY && cfg.flags==0 && subscribe_events==0 && host_locked==0);
    CHECK(cccd_request(1)==0 && cfg.flags==1 && subscribe_events==1);
    CHECK(ble_store_config_num_cccds==1 && disk[2][0].present);
    guard_denied=1; int before=writes;
    CHECK(cccd_request(0)==BLE_ATT_ERR_UNLIKELY && cfg.flags==1 && subscribe_events==1 && writes==before);
    guard_denied=0;
    CHECK(cccd_request(0)==0 && cfg.flags==0 && subscribe_events==2);
    CHECK(!disk[2][0].present && ble_store_config_num_cccds==0 && host_locked==0 && opened==closed);
    CHECK(cccd_request(1)==0);
    fault(AFTER,3);
    CHECK(cccd_request(0)==BLE_ATT_ERR_UNLIKELY && cfg.flags==1);
    CHECK(cccd_request(1)==0);
    ble_store_config_init();
    CHECK(ble_store_config_num_cccds==1 && disk[2][0].present);
    CHECK(cccd_request(0)==0);
    fault(AFTER,3);
    CHECK(cccd_request(1)==BLE_ATT_ERR_UNLIKELY && cfg.flags==0);
    CHECK(cccd_request(0)==0);
    ble_store_config_init();
    CHECK(ble_store_config_num_cccds==0 && !disk[2][0].present);
    printf("[CCCD_RETRY] enable_disable=exact events=%d handles=%d/%d\n",subscribe_events,opened,closed);
    return 0;
}
'''
        self.compile_and_run(source)
