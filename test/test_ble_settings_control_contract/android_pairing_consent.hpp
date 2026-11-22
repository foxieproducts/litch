#pragma once

// Actual pinned Android SMP actions; this is not an Android UI or radio model.
// The production BLE owner supplies its observed Security Request count. RF,
// valid SC commitment, display capability and the stack state-machine leaves
// are controlled so the two distinct app-consent stages remain observable.
namespace android_pairing_consent {
namespace log {
template<typename... Args> void verbose(const char*, Args...) {}
template<typename... Args> void error(const char*, Args...) {}
}
enum {
    SMP_EVT_NONE, SMP_INVALID_PARAMETERS, SMP_AUTH_CMPL_EVT, SMP_ENC_REQ_EVT,
    BTM_SEC_MODE_SC, SMP_SC_SUPPORT_BIT = 8, SMP_PAIR_AUTH_FAIL,
    SMP_SEC_DEFAULT_KEY, SMP_SEC_REQUEST_EVT, SMP_CONFIRM_VALUE_ERR,
    SMP_NUMERIC_COMPAR_FAIL, SMP_MODEL_SEC_CONN_JUSTWORKS,
    SMP_MODEL_SEC_CONN_NUM_COMP, SMP_MODEL_SEC_CONN_PASSKEY_ENT,
    SMP_MODEL_SEC_CONN_PASSKEY_DISP, SMP_MODEL_SEC_CONN_OOB,
    HCI_ROLE_PERIPHERAL, HCI_ROLE_CENTRAL, SMP_IO_CAP_IO, SMP_IO_CAP_KBDISP,
    SMP_CONSENT_REQ_EVT, SMP_STATE_WAIT_NONCE, SMP_SC_DSPL_NC_EVT,
    SMP_SC_PHASE1_CMPLT_EVT, SMP_SC_CALC_NC_EVT, SMP_STATE_SEC_CONN_PHS1_START,
    SMP_PAIR_FLAG_HAVE_PEER_COMM
};
enum tBTM_BLE_SEC_REQ_ACT {
    BTM_BLE_SEC_REQ_ACT_ENCRYPT, BTM_BLE_SEC_REQ_ACT_PAIR, BTM_BLE_SEC_REQ_ACT_DISCARD
};
using tBTM_LE_AUTH_REQ = uint8_t;
struct tSMP_CB {
    int pairing_bda = 1, cb_evt = SMP_EVT_NONE, init_security_mode = BTM_SEC_MODE_SC;
    bool sc_only_mode_locally_required = false, discard_sec_req = false;
    int peer_auth_req = 0, local_r_key = 0, local_i_key = 0;
    int cert_failure = 0, failure = 0, round = 0, flags = 0;
    int selected_association_model = SMP_MODEL_SEC_CONN_JUSTWORKS;
    int role = HCI_ROLE_CENTRAL, local_io_capability = SMP_IO_CAP_KBDISP;
};
struct tSMP_INT_DATA { int status = 0; void* p_data = nullptr; };
struct Config { bool isAndroidTVDevice() const { return false; } };
struct Profiles { Config* config; };
inline Profiles* GetInterfaceToProfiles() {
    static Config config;
    static Profiles profiles{&config};
    return &profiles;
}
inline bool smp_command_has_invalid_length(tSMP_CB*) { return false; }
inline void btm_ble_link_sec_check(int, uint8_t, tBTM_BLE_SEC_REQ_ACT* action) {
    *action = BTM_BLE_SEC_REQ_ACT_PAIR;
}
inline bool smp_check_commitment(tSMP_CB*) { return true; }
inline void smp_send_rand(tSMP_CB*, void*) {}
inline void smp_set_state(int) {}
inline void smp_start_nonce_generation(tSMP_CB*) {}
inline unsigned scConsent = 0U;
inline void smp_sm_event(tSMP_CB* control, int event, tSMP_INT_DATA*) {
    if (event == SMP_SC_DSPL_NC_EVT && control->cb_evt == SMP_CONSENT_REQ_EVT) ++scConsent;
}

#include "../fixtures/android_smp_consent_e6130675.inc"

struct Stages { unsigned securityConsent = 0U, scConsent = 0U; };
inline Stages Replay(unsigned peripheralSecurityRequests) {
    tSMP_CB control;
    uint8_t auth = SMP_SC_SUPPORT_BIT | 1U;
    tSMP_INT_DATA data{};
    data.p_data = &auth;
    Stages result;
    for (unsigned i = 0U; i < peripheralSecurityRequests; ++i) {
        smp_proc_sec_req(&control, &data);
        if (control.cb_evt == SMP_SEC_REQUEST_EVT) ++result.securityConsent;
    }
    // The same valid SC Just Works nonce path follows either accepted initial
    // consent (old) or the phone's createBond (new). OS approval is not faked.
    scConsent = 0U;
    smp_process_peer_nonce(&control, nullptr);
    result.scConsent = scConsent;
    return result;
}
} // namespace android_pairing_consent
