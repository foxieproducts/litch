"""Exact pinned GAP body; host store/radio leaves are controlled, not durable proof."""
from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "harden_nimble_repeat_pairing.py"
FIXTURE = Path(__file__).parent / "fixtures/nimble_237"
spec = importlib.util.spec_from_file_location("litch_unpair_errors", SCRIPT)
hardening = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hardening)


def UnpairBody(source: bytes) -> str:
    text = source.decode()
    start = text.index("int\nble_gap_unpair(const ble_addr_t *peer_addr)")
    brace = text.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def NamedBody(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


DATA_LENGTH_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define NIMBLE_BLE_CONNECT 1
#define BLE_GAP_EVENT_DATA_LEN_CHG 34
#define BLE_GAP_EVENT_ENC_CHANGE 10
#define le16toh(x) (x)
struct ble_hci_ev_le_subev_data_len_chg {
    uint8_t subev_code;
    uint16_t conn_handle, max_tx_octets, max_tx_time, max_rx_octets, max_rx_time;
} __attribute__((packed));
struct ble_gap_event {
    uint8_t type;
    struct { uint16_t conn_handle; int status; } enc_change;
    struct { uint16_t conn_handle, max_tx_octets, max_tx_time, max_rx_octets, max_rx_time; } data_len_chg;
};
struct BleDataLengthObservation { uint16_t txOctets, txTimeUs, rxOctets, rxTimeUs; };
struct BleService {
    unsigned observations = 0, encryption = 0;
    uint16_t handle = 0xffff, encryptionHandle = 0xffff;
    int encryptionStatus = 0;
    BleDataLengthObservation value{};
    struct LinkObservation {
        uint16_t mtu=247,intervalUnits=6,latency=0,timeoutUnits=200;
        uint8_t txPhy=2,rxPhy=2;
        bool phyKnown=true,dataLengthKnown=false;
    } link;
    LinkObservation GetLinkObservation() const { return link; }
    void OnNativeEncryptionChange(uint16_t h, int s) { ++encryption; encryptionHandle=h; encryptionStatus=s; }
    void OnNativeDataLengthChange(uint16_t h, const BleDataLengthObservation& v) { ++observations; handle=h; value=v; link.dataLengthKnown=true; }
};
static BleService service;
struct { BleService& ble; } sys{service};
struct Console {
    char last[256]{};
    void PrintLn(const char *format,...) { va_list args;va_start(args,format);vsnprintf(last,sizeof last,format,args);va_end(args); }
} console;
static Console *consolePtr=&console;
static void RenderLink() { /* ACTUAL_SUPPORT_LINK_ROW */ }
static uint16_t g_max_tx_octets[64],g_max_rx_octets[64],g_max_tx_time[64],g_max_rx_time[64];
static unsigned connectionCallbacks;
static uint16_t connectionHandle;
/* ACTUAL_OWNER_GAP_EVENT */
static void ble_gap_event_listener_call(ble_gap_event *event) { OwnerGapEvent(event,&service); }
static void ble_gap_call_conn_event_cb(ble_gap_event *, uint16_t handle) { ++connectionCallbacks; connectionHandle=handle; }
/* ACTUAL_PINNED_DATA_LENGTH_FUNCTION */
int main() {
    RenderLink();
    const bool supportUnknown=strstr(console.last,"dle_known=0")!=nullptr;
    const ble_hci_ev_le_subev_data_len_chg input={7,23,237,2120,251,17040};
    ble_gap_rx_data_len_change(&input);
    int failures=service.observations!=1 || service.handle!=23 ||
        service.value.txOctets!=237 || service.value.txTimeUs!=2120 ||
        service.value.rxOctets!=251 || service.value.rxTimeUs!=17040 ||
        connectionCallbacks!=1 || connectionHandle!=23 || g_max_tx_octets[23]!=237;
    ble_gap_event security{};security.type=BLE_GAP_EVENT_ENC_CHANGE;
    security.enc_change.conn_handle=19;security.enc_change.status=7;OwnerGapEvent(&security,&service);
    failures+=service.encryption!=1 || service.encryptionHandle!=19 || service.encryptionStatus!=7;
    RenderLink();
    const bool supportKnown=strstr(console.last,"dle_known=1")!=nullptr;
    failures+=!supportUnknown || !supportKnown;
    printf("DLE actual_handle=23 observed_handle=%u observations=%u encryption=%u failures=%d\n",
           service.handle,service.observations,service.encryption,failures);
    printf("DLE Support unknown=%u observed=%u\n",unsigned(supportUnknown),unsigned(supportKnown));
    return failures ? 1 : 0;
}
"""


def RunDataLengthCorpus(source: bytes, root: Path, label: str):
    compiler = shutil.which("clang++")
    if compiler is None:
        raise RuntimeError("clang++ is required for exact SDK data-length evidence")
    owner = (SCRIPT.parents[2] / "src/ble_service_owner_native.cpp").read_text()
    body = DATA_LENGTH_HARNESS.replace("/* ACTUAL_OWNER_GAP_EVENT */",
        NamedBody(owner, "int OwnerGapEvent("))
    body = body.replace("/* ACTUAL_PINNED_DATA_LENGTH_FUNCTION */",
        NamedBody(source.decode(), "void\nble_gap_rx_data_len_change("))
    console = (SCRIPT.parents[2] / "src/firmware_console.cpp").read_text()
    start = console.index("        const auto link = sys.ble.GetLinkObservation();")
    end = console.index("        const auto session = sys.ble.GetSessionDiagnostics();", start)
    body = body.replace("/* ACTUAL_SUPPORT_LINK_ROW */", console[start:end])
    path = root / (label + ".cpp"); path.write_text(body)
    binary = root / label
    build = subprocess.run([compiler, "-std=c++17", "-fsanitize=undefined,bounds",
        "-fno-sanitize-recover=all", str(path), "-o", str(binary)], capture_output=True, text=True)
    if build.returncode:
        raise RuntimeError(build.stderr)
    return subprocess.run([str(binary)], capture_output=True, text=True)


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef NIMBLE_BLE_SM
#define NIMBLE_BLE_SM 1
#endif
#define MYNEWT_VAL(x) MYNEWT_VAL_##x
#define MYNEWT_VAL_BLE_HOST_BASED_PRIVACY 0
#define MYNEWT_VAL_BLE_SMP_ID_RESET 0
#define BLE_HS_ENOENT 5
#define BLE_HS_EINVAL 3
#define BLE_HS_EBUSY 15
#define BLE_HS_EDISABLED 30
#define BLE_HS_ENOTSUP 8
#define BLE_ERR_REM_USER_CONN_TERM 19
#define BLE_STORE_OBJ_TYPE_PEER_ADDR 6
#define BLE_STORE_OBJ_TYPE_PEER_SEC 2
#define BLE_STORE_OBJ_TYPE_OUR_SEC 1
#define BLE_HS_LOG(...) ((void)0)
typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
static const ble_addr_t any_addr = {0,{0}};
#define BLE_ADDR_ANY (&any_addr)
static int ble_addr_cmp(const ble_addr_t *a,const ble_addr_t *b) { return memcmp(a,b,sizeof(*a)); }
struct ble_hs_conn { int unused; };
union ble_store_key { struct { ble_addr_t peer_addr; } sec; struct { ble_addr_t peer_rpa_addr; } rpa_rec; };
union ble_store_value { struct { uint8_t irk_present; } sec; struct { ble_addr_t peer_addr; } rpa_rec; };
static struct ble_hs_conn connection;
static int enabled, branch, delete_rc, deletes, advertising, discovering, irk, connected, terminated;
static ble_addr_t deleted;
static int ble_hs_is_enabled(void) { return enabled; }
static void ble_hs_lock(void) {}
static void ble_hs_unlock(void) {}
static struct ble_hs_conn *ble_hs_conn_find_by_addr(const ble_addr_t *a) { (void)a; return connected ? &connection : NULL; }
static void ble_gap_terminate_with_conn(struct ble_hs_conn *c,int reason) { (void)c; (void)reason; ++terminated; }
static int ble_gap_adv_active(void) { return advertising; }
static int ble_gap_disc_active(void) { return discovering; }
static int ble_hs_pvcy_remove_entry(int type,const uint8_t *value) { (void)type; (void)value; return 0; }
static int ble_store_read(int type,const union ble_store_key *key,union ble_store_value *value) {
    (void)key; memset(value,0,sizeof(*value));
    if (type==BLE_STORE_OBJ_TYPE_PEER_ADDR) {
        if (branch!=3) return BLE_HS_ENOENT;
        value->rpa_rec.peer_addr.type=1; value->rpa_rec.peer_addr.val[0]=22; return 0;
    }
    if (branch==2 || (type==BLE_STORE_OBJ_TYPE_PEER_SEC && branch==1)) return BLE_HS_ENOENT;
    value->sec.irk_present=(uint8_t)irk; return 0;
}
static int ble_store_util_delete_peer(const ble_addr_t *peer) { ++deletes; deleted=*peer; return delete_rc; }
static void reset_case(void) { enabled=1; branch=delete_rc=deletes=advertising=discovering=irk=connected=terminated=0; memset(&deleted,0,sizeof(deleted)); }
"""
MAIN = r"""
int main(void) {
    const ble_addr_t peer={1,{1,0,0,0,0,0}};
    int failures=0, cases=0, result;
#if NIMBLE_BLE_SM
    const int errors[]={0,5,17,28};
    for (int path=0; path<2; ++path) for (unsigned n=0; n<4; ++n) {
        reset_case(); branch=path; delete_rc=errors[n]; result=ble_gap_unpair(&peer);
        int good=result==delete_rc && deletes==1 && deleted.val[0]==1;
        failures+=!good; ++cases;
        printf("UNPAIR branch=%s deletion_rc=%d API_rc=%d deletes=%d contract=%s\n",path?"OUR_SEC_ONLY":"PEER_SEC",delete_rc,result,deletes,good?"PASS":"FAIL");
    }
    reset_case(); enabled=0; result=ble_gap_unpair(&peer); failures+=result!=BLE_HS_EDISABLED || deletes; ++cases;
    reset_case(); result=ble_gap_unpair(BLE_ADDR_ANY); failures+=result!=BLE_HS_EINVAL || deletes; ++cases;
    reset_case(); branch=2; result=ble_gap_unpair(&peer); failures+=result!=BLE_HS_ENOENT || deletes; ++cases;
    for (int mode=0; mode<2; ++mode) {
        reset_case(); irk=1; advertising=mode==0; discovering=mode==1; result=ble_gap_unpair(&peer);
        failures+=result!=BLE_HS_EBUSY || deletes; ++cases;
    }
    reset_case(); connected=1; result=ble_gap_unpair(&peer); failures+=result!=0 || deletes!=1 || terminated!=1; ++cases;
    reset_case(); branch=3; result=ble_gap_unpair(&peer); failures+=result!=0 || deletes!=1 || deleted.val[0]!=22; ++cases;
#else
    reset_case(); result=ble_gap_unpair(&peer); failures+=result!=BLE_HS_ENOTSUP || deletes; ++cases;
#endif
    printf("UNPAIR cases=%d failures=%d\n",cases,failures);
    return failures ? 1 : 0;
}
"""


def RunCorpus(source: bytes, root: Path, label: str, *, sm: int = 1):
    compiler = shutil.which("clang")
    if compiler is None:
        raise RuntimeError("clang is required for exact SDK unpair evidence")
    path = root / (label + ".c")
    path.write_text(HARNESS + '\n#line 8270 "pinned_ble_gap.c"\n' + UnpairBody(source) + MAIN)
    binary = root / label
    build = subprocess.run([compiler, "-std=c11", f"-DNIMBLE_BLE_SM={sm}",
                            "-fsanitize=undefined,bounds", "-fno-sanitize-recover=all",
                            str(path), "-o", str(binary)], capture_output=True, text=True)
    if build.returncode:
        raise RuntimeError(build.stderr)
    return subprocess.run([str(binary)], capture_output=True, text=True)


class UnpairErrorTests(unittest.TestCase):
    def setUp(self):
        self.original = (FIXTURE / "ble_gap.c").read_bytes()

    def test_only_unpair_returns_and_data_length_handle_assignment_change(self):
        self.assertEqual(hashlib.sha256(self.original).hexdigest(), hardening.GAP_ORIGINAL_SHA256)
        patched = hardening.HardenGapSource(self.original)
        old, new = UnpairBody(self.original).encode(), UnpairBody(patched).encode()
        before, after = self.original.split(old)
        without_handle = patched.replace(b"    event.data_len_chg.conn_handle = conn_handle;\n", b"", 1)
        self.assertEqual(without_handle, before + new + after)
        self.assertIn(b"            err = rc;", new)
        self.assertIn(b"            err = ble_store_util_delete_peer", new)

    def test_nonzero_data_length_handle_fails_old_and_reaches_existing_owner_after_repair(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old = RunDataLengthCorpus(self.original, root, "dle-old")
            new = RunDataLengthCorpus(hardening.HardenGapSource(self.original), root, "dle-new")
        print(old.stdout + new.stdout, end="")
        self.assertEqual(old.returncode, 1, old.stdout + old.stderr)
        self.assertIn("observed_handle=0 observations=1 encryption=1 failures=1", old.stdout)
        self.assertEqual(new.returncode, 0, new.stdout + new.stderr)
        self.assertIn("observed_handle=23 observations=1 encryption=1 failures=0", new.stdout)
        self.assertIn("DLE Support unknown=1 observed=1", new.stdout)

    def test_unknown_source_is_rejected_before_output(self):
        for data in (b"", self.original+b"\n", self.original.replace(b"delete_peer",b"delete_pear")):
            with self.subTest(bytes=len(data)), self.assertRaises(ValueError):
                hardening.HardenGapSource(data)

    def test_real_source_node_private_headers_and_single_build_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "deps/NimBLE-Arduino"
            source = package / "src/nimble/nimble/host/src/ble_gap.c"
            source.parent.mkdir(parents=True)
            source.write_bytes(self.original)
            shutil.copyfile(FIXTURE / "library.properties", package / "library.properties")
            variant = root / "build/lib123/NimBLE-Arduino/ble_gap.c"
            case = self
            class Node:
                def __init__(self, path, original=None): self.path, self.original = path, original
                def srcnode(self): return self.original or self
                def get_abspath(self): return str(self.path)
            class Environment:
                def subst(self, value):
                    case.assertEqual(value,"$BUILD_DIR"); return str(root / "build")
                def AppendUnique(self, **values):
                    case.assertEqual(values,{"CPPPATH":[str(source.parent)]})
                def File(self, path): return Node(Path(path))
            output=hardening._HardenGapNode(Environment(),Node(variant,Node(source))).path
            self.assertEqual(output,root / "build/litch_sdk/ble_gap.c")
            stamp=output.stat().st_mtime_ns
            self.assertEqual(output.read_bytes(),hardening.HardenGapSource(self.original))
            self.assertEqual(hardening.WriteHardenedGapSource(package,root / "build"),output)
            self.assertEqual(stamp,output.stat().st_mtime_ns)
            self.assertEqual(source.read_bytes(),self.original)
            self.assertFalse(variant.exists())
            (package / "library.properties").write_text("name=NimBLE-Arduino\nversion=2.3.8\n")
            with self.assertRaises(ValueError): hardening.WriteHardenedGapSource(package,root / "unsupported")
            self.assertFalse((root / "unsupported").exists())

    def test_same_contract_fails_old_and_passes_new_exact_sdk_body(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            old=RunCorpus(self.original,root,"old")
            new=RunCorpus(hardening.HardenGapSource(self.original),root,"new")
        print(old.stdout+new.stdout,end="")
        self.assertEqual(old.returncode,1,old.stdout+old.stderr)
        self.assertIn("cases=15 failures=6",old.stdout)
        self.assertEqual(new.returncode,0,new.stdout+new.stderr)
        self.assertIn("cases=15 failures=0",new.stdout)

    def test_security_disabled_behavior_stays_disabled(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            for label,data in (("old",self.original),("new",hardening.HardenGapSource(self.original))):
                run=RunCorpus(data,root,label,sm=0)
                self.assertEqual(run.returncode,0,run.stdout+run.stderr)
                self.assertIn("cases=1 failures=0",run.stdout)

    def test_middleware_registered_once_and_only_for_s3(self):
        registrations=[]
        class Environment:
            def __init__(self, name): self.name=name
            def subst(self, value): return self.name
            def AddBuildMiddleware(self, callback, pattern): registrations.append((callback.__name__,pattern))
        namespace={"Import":lambda name:None,"env":Environment("litch-esp32s3")}
        exec(compile(SCRIPT.read_text(),str(SCRIPT),"exec"),namespace)
        self.assertEqual([p for n,p in registrations if n=="_HardenGapNode"],
                         ["*/NimBLE-Arduino/src/nimble/nimble/host/src/ble_gap.c"])
        with self.assertRaises(ValueError):
            exec(compile(SCRIPT.read_text(),str(SCRIPT),"exec"),{"Import":lambda name:None,"env":Environment("not-s3")})

    def test_whitespace_exception_preserves_pinned_fixture_but_rejects_owned_source(self):
        attributes=SCRIPT.parents[2] / ".gitattributes"
        self.assertIn("tools/pio/tests/fixtures/nimble_237/ble_gap.c -whitespace",
                      attributes.read_text().splitlines())
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            subprocess.run(["git","init","-q",str(root)],check=True)
            shutil.copyfile(attributes,root / ".gitattributes")
            fixture=root / "tools/pio/tests/fixtures/nimble_237/ble_gap.c"
            fixture.parent.mkdir(parents=True); fixture.write_bytes(self.original)
            subprocess.run(["git","-C",str(root),"add","."] ,check=True)
            result=subprocess.run(["git","-C",str(root),"diff","--cached","--check"],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            owned_sources=tuple(folder / name for folder in (root,fixture.parent)
                                for name in ("owned.c","owned.cpp"))
            for owned in owned_sources:
                owned.write_text("int owned; \n")
                subprocess.run(["git","-C",str(root),"add",str(owned)],check=True)
            rejected=subprocess.run(["git","-C",str(root),"diff","--cached","--check"],capture_output=True,text=True)
            self.assertNotEqual(rejected.returncode,0)
            for owned in owned_sources:
                prefix=owned.relative_to(root).as_posix()+":"
                self.assertTrue(any(line.startswith(prefix) for line in rejected.stdout.splitlines()),
                                rejected.stdout)


if __name__ == "__main__":
    if sys.argv[1:] == ["--old-sdk"]:
        with tempfile.TemporaryDirectory() as temporary:
            result=RunCorpus((FIXTURE / "ble_gap.c").read_bytes(),Path(temporary),"old")
            print(result.stdout,end=""); raise SystemExit(result.returncode)
    unittest.main()
