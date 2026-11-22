"""Pinned GAP -> SMP Security Request transmitter; only RF/config are controlled."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_nimble_pairing_admission import PairingCorpus
from test_nimble_security_store import Function, FIXTURE, SCRIPT, hardening


def RequestCorpus():
    original = (FIXTURE / 'ble_sm.c').read_bytes()
    if hashlib.sha256(original).hexdigest() != hardening.SM_ORIGINAL_SHA256:
        raise ValueError('Unsupported pinned SMP source')
    sm = hardening.HardenSmSource(original).decode()
    gap = (FIXTURE / 'ble_gap.c').read_text()
    if hashlib.sha256(gap.encode()).hexdigest() != '8cadddeeacfd4e8f0ef010c21f4a7f4d30077459cacdd6821ba33c8b0328ce44':
        raise ValueError('Unsupported pinned GAP source')
    body = PairingCorpus(sm).split('int main(')[0]
    stub = 'static int ble_sm_slave_initiate(uint16_t h) { (void)h; abort(); }'
    assert body.count(stub) == 1
    body = body.replace(stub, 'int ble_sm_slave_initiate(uint16_t);')
    marker = 'static void ble_sm_ltk_restore_exec(struct ble_sm_proc *,struct ble_sm_result *,void *);'
    assert body.count(marker) == 1
    leaves = r'''
#define NIMBLE_BLE_SM 1
#define BLE_HS_EALREADY 2
#define BLE_HS_EDISABLED 30
#define BLE_SM_OP_SEC_REQ 11
struct ble_sm_sec_req { uint8_t authreq; };
static struct ble_sm_sec_req outgoing;
static struct os_mbuf outgoing_packet;
static int security_requests,tx_result,command_allocation_fails;
static int ble_hs_is_enabled(void) { return 1; }
static int ble_sm_enc_initiate(uint16_t h,uint8_t k,const uint8_t *ltk,uint16_t d,uint64_t r,int a) {
    (void)h;(void)k;(void)ltk;(void)d;(void)r;(void)a; abort();
}
static uint8_t ble_sm_build_authreq(void) { return 9; }
static void *ble_sm_cmd_get(uint8_t op,size_t size,struct os_mbuf **packet) {
    BLE_HS_DBG_ASSERT(op==BLE_SM_OP_SEC_REQ && size==sizeof outgoing);
    if(command_allocation_fails) return NULL;
    outgoing_packet.om_data=&outgoing; outgoing_packet.size=sizeof outgoing;
    *packet=&outgoing_packet; return &outgoing;
}
static int ble_sm_tx(uint16_t handle,struct os_mbuf *packet) {
    BLE_HS_DBG_ASSERT(host_locked && handle==42 && packet==&outgoing_packet && outgoing.authreq==9);
    ++security_requests; return tx_result;
}
static int ble_store_read_peer_sec(const struct ble_store_key_sec *k,struct ble_store_value_sec *v) {
    (void)k;(void)v; abort();
}
static void ble_sm_sec_req_exec(struct ble_sm_proc *,struct ble_sm_result *,void *);
'''
    body = body.replace(marker, leaves + marker)
    body = body.replace('else r->execute=0; /* RF/crypto waits; the full pairing engine is not faked. */',
        'else if(p->state==BLE_SM_PROC_STATE_SEC_REQ) { r->execute=0; ble_sm_sec_req_exec(p,r,arg); }\n'
        '    else r->execute=0; /* RF/crypto waits. */')
    body += '\n' + Function(sm, 'ble_sm_sec_req_exec')
    body += '\n' + Function(sm, 'ble_sm_slave_initiate')
    body += '\n' + Function(gap, 'ble_gap_security_initiate')
    return body + r'''
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    ble_store_config_init(); ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    memset(&connection,0,sizeof connection); /* Production peripheral role. */
    if(which==1) command_allocation_fails=1;
    if(which==2) tx_result=15;
    int result=ble_gap_security_initiate(42);
    if(which==1) CHECK(result==BLE_HS_ENOMEM && security_requests==0 && !active_proc);
    else if(which==2) CHECK(result==15 && security_requests==1 && !active_proc);
    else {
        CHECK(result==0 && security_requests==1 && active_proc);
        for(int replay=0;replay<3;++replay) CHECK(ble_gap_security_initiate(42)==BLE_HS_EALREADY);
        CHECK(security_requests==1 && allocations==1);
    }
    CHECK(writes==0 && commits==0 && our_attempts==0 && peer_attempts==0);
    printf("SDK Security Request case%d tx%d result%d active%d stores%d/%d\n",
        which,security_requests,result,active_proc,our_attempts,peer_attempts);
    return 0;
}
'''


class NativeSecurityRequestTests(unittest.TestCase):
    def test_android_consent_actions_match_pinned_official_source(self):
        # Only these exact actions are compiled by the production BLE owner's
        # host test. They are not a Samsung binary or an Android dialog model.
        source = (SCRIPT.parents[1] / '../test/fixtures/android_smp_consent_e6130675.inc').resolve().read_text()
        expected = {
            'smp_proc_sec_req': 'ba570f86aba0a71d9062d12b0237efa65b145f7a4e717461b86ae99030a721bc',
            'smp_process_peer_nonce': '0c99d55b79125be57059e35c80fed1d0b4543e7a3cf7dde8b10a230c53e9332d',
        }
        for name, digest in expected.items():
            self.assertEqual(hashlib.sha256(Function(source, name).encode()).hexdigest(), digest)

    def test_actual_peripheral_request_transmitter_and_repeated_pending_calls(self):
        compiler = shutil.which('clang')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'security_request.c'
            binary = Path(temporary) / 'security_request'
            source.write_text(RequestCorpus())
            build = subprocess.run([compiler, '-std=c11', '-O1', '-DWITH_ADMISSION_HOOK=1',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(SCRIPT.parent), str(source), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            for case in range(3):
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), str(case)], capture_output=True,
                        text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(result.stderr, '')
                    print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
