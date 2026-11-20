/* Controlled host/RF/crypto leaves around exact SDK SMP and security-store
 * functions. This does not simulate crypto, RF or physical NVS behavior. */
#define MYNEWT_VAL_BLE_HS_DEBUG 0
#define MYNEWT_VAL_BLE_SM_LVL 0
#define BLE_HS_ENOTCONN 7
#define BLE_HS_EBADDATA 10
#define BLE_HS_EAUTHOR 29
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_HS_SM_US_ERR(x) (0x400 + (x))
#define BLE_HS_HCI_ERR(x) ((x) ? 0x200 + (x) : 0)
#define BLE_ADDR_PUBLIC 0
#define BLE_ADDR_RANDOM 1
#define BLE_ADDR_PUBLIC_ID 2
#define BLE_ADDR_RANDOM_ID 3
#define BLE_HS_CONN_F_MASTER 1
#define BLE_SM_OP_PAIR_REQ 1
#define BLE_SM_OP_PAIR_FAIL 5
#define BLE_SM_IOACT_NONE 0
#define BLE_SM_ERR_SUCCESS 0
#define BLE_SM_ERR_AUTHREQ 3
#define BLE_SM_ERR_PAIR_NOT_SUPP 5
#define BLE_SM_ERR_ENC_KEY_SZ 6
#define BLE_SM_ERR_CMD_NOT_SUPP 7
#define BLE_SM_ERR_UNSPECIFIED 8
#define BLE_SM_ERR_INVAL 10
#define BLE_SM_PAIR_KEY_SZ_MIN 7
#define BLE_SM_PAIR_KEY_SZ_MAX 16
#define BLE_SM_PAIR_AUTHREQ_SC 8
#define BLE_SM_PROC_STATE_NONE 255
#define BLE_SM_PROC_STATE_PAIR 0
#define BLE_SM_PROC_STATE_LTK_START 3
#define BLE_SM_PROC_STATE_LTK_RESTORE 4
#define BLE_SM_PROC_STATE_ENC_START 5
#define BLE_SM_PROC_STATE_ENC_RESTORE 6
#define BLE_SM_PROC_STATE_KEY_EXCH 7
#define BLE_SM_PROC_STATE_SEC_REQ 8
#define BLE_SM_PROC_F_AUTHENTICATED 8
#define BLE_SM_PROC_F_SC 16
#define BLE_SM_PROC_F_BONDING 32
#define BLE_SM_PROC_F_INITIATOR 1
#define le16toh(x) (x)
#define le64toh(x) (x)
#define STATS_INC(...) ((void)0)
typedef uint8_t ble_sm_proc_flags;
typedef uint8_t ble_hs_conn_flags_t;
struct ble_gap_passkey_params { uint8_t action; };
struct ble_sm_pair_cmd {
    uint8_t io_cap,oob_data_flag,authreq,max_enc_key_size,init_key_dist,resp_key_dist;
};
struct ble_sm_keys {
    unsigned ltk_valid:1,ediv_rand_valid:1,irk_valid:1,csrk_valid:1,addr_valid:1;
    uint32_t sign_counter;
    uint16_t ediv;
    uint64_t rand_val;
    uint8_t addr_type,key_size,ltk[16],irk[16],csrk[16],addr[6];
};
struct ble_sm_proc {
    ble_sm_proc_flags flags;
    uint16_t conn_handle;
    uint8_t state,rx_key_flags,key_size,pair_req[7],pair_rsp[7];
    struct ble_sm_keys our_keys,peer_keys;
};
struct ble_sm_result {
    int app_status;
    uint8_t sm_err;
    struct ble_gap_passkey_params passkey_params;
    void *state_arg;
    unsigned execute:1,enc_cb:1,bonded:1,restore:1,out_of_order:1;
};
struct ble_hs_conn {
    ble_hs_conn_flags_t bhc_flags;
    ble_addr_t bhc_peer_addr,bhc_peer_rpa_addr;
    struct { uint8_t encrypted,authenticated,bonded,key_size; } bhc_sec_state;
};
struct ble_hs_conn_addrs { ble_addr_t peer_id_addr; };
struct ble_hci_ev_le_subev_lt_key_req { uint16_t conn_handle,div; uint64_t rand; };
struct os_mbuf { void *om_data; size_t size; };
struct ble_l2cap_chan { uint16_t conn_handle; struct os_mbuf *rx_buf; };
typedef void ble_sm_rx_fn(uint16_t,struct os_mbuf **,struct ble_sm_result *);
static void ble_sm_pair_req_rx(uint16_t,struct os_mbuf **,struct ble_sm_result *);
static ble_sm_rx_fn *ble_sm_dispatch_get(uint8_t op) { return op==BLE_SM_OP_PAIR_REQ ? ble_sm_pair_req_rx : NULL; }
static uint16_t ble_l2cap_get_conn_handle(struct ble_l2cap_chan *c) { return c->conn_handle; }
static int os_mbuf_copydata(struct os_mbuf *m,size_t offset,size_t size,void *out) {
    if(offset+size>m->size) return BLE_HS_EBADDATA;
    memcpy(out,(uint8_t *)m->om_data+offset,size); return 0;
}
static void os_mbuf_adj(struct os_mbuf *m,size_t size) { m->om_data=(uint8_t *)m->om_data+size; m->size-=size; }
static struct ble_hs_conn connection;
static struct ble_sm_proc procedure;
static int active_proc,allocations,removals,frees,capacity_calls;
static int hook_calls,lease_valid=1,hook_epoch=1,session_epoch=1,identity_revokes;
static uint16_t lease_handle=42;
static int our_attempts,peer_attempts,rpa_attempts,identity_events;
static int rpa_result;
static int enc_events,enc_status,enc_bonded,enc_restore,pair_events,pair_status;
static int pair_observed_our,pair_observed_peer,ltk_replies,ltk_nacks,tx_failures;
static uint8_t restored_ltk[16];
static int counted_store_write(int type,const union ble_store_value *v) {
    if(type==BLE_STORE_OBJ_TYPE_OUR_SEC) ++our_attempts;
    if(type==BLE_STORE_OBJ_TYPE_PEER_SEC) ++peer_attempts;
    return ble_store_config_write(type,v);
}
static struct ble_hs_conn *ble_hs_conn_find(uint16_t h) { return h==42 ? &connection : NULL; }
static struct ble_hs_conn *ble_hs_conn_find_assert(uint16_t h) {
    BLE_HS_DBG_ASSERT(host_locked && h==42); return &connection;
}
static void ble_hs_conn_addrs(struct ble_hs_conn *c,struct ble_hs_conn_addrs *out) {
    BLE_HS_DBG_ASSERT(host_locked); out->peer_id_addr=c->bhc_peer_addr;
    out->peer_id_addr.type&=1;
}
static uint8_t ble_hs_misc_peer_addr_type_to_id(uint8_t type) { return type&1; }
static struct ble_sm_proc *ble_sm_proc_find(uint16_t h,uint8_t s,int initiator,struct ble_sm_proc **prev) {
    (void)s; (void)initiator; BLE_HS_DBG_ASSERT(host_locked);
    if(prev) *prev=NULL;
    return active_proc && procedure.conn_handle==h ? &procedure : NULL;
}
static struct ble_sm_proc *ble_sm_proc_alloc(void) {
    ++allocations; memset(&procedure,0,sizeof procedure); return &procedure;
}
static void ble_sm_insert(struct ble_sm_proc *p) { BLE_HS_DBG_ASSERT(host_locked && p==&procedure); active_proc=1; }
static void ble_sm_proc_remove(struct ble_sm_proc *p,struct ble_sm_proc *prev) {
    (void)p; (void)prev; BLE_HS_DBG_ASSERT(host_locked); active_proc=0; ++removals;
}
static void ble_sm_proc_free(struct ble_sm_proc *p) { (void)p; ++frees; }
static void ble_sm_proc_set_timer(struct ble_sm_proc *p) { (void)p; }
static int ble_sm_chk_store_overflow(uint16_t h) { (void)h; ++capacity_calls; return 0; }
static int ble_sm_chk_repeat_pairing(uint16_t h,ble_sm_proc_flags f,uint8_t k) { (void)h; (void)f; (void)k; return 0; }
static int ble_hs_mbuf_pullup_base(struct os_mbuf **om,size_t size) { return (*om)->size<size ? BLE_HS_EINVAL : 0; }
static bool ble_sm_verify_auth_requirements(uint8_t auth) { (void)auth; return true; }
static void ble_sm_pair_rsp_fill(struct ble_sm_proc *p) { (void)p; }
static void ble_sm_pair_cfg(struct ble_sm_proc *p) { p->flags=BLE_SM_PROC_F_BONDING|BLE_SM_PROC_F_SC; p->key_size=16; }
static void ble_sm_pair_fail_tx(uint16_t h,uint8_t reason) { (void)h; (void)reason; ++tx_failures; }
static int ble_sm_pair_initiate(uint16_t h) { (void)h; abort(); }
static int ble_sm_slave_initiate(uint16_t h) { (void)h; abort(); }
static int ble_sm_ltk_req_reply_tx(uint16_t h,const uint8_t *ltk) {
    (void)h; ++ltk_replies; memcpy(restored_ltk,ltk,sizeof restored_ltk); return 0;
}
static int ble_sm_ltk_req_neg_reply_tx(uint16_t h) { (void)h; ++ltk_nacks; return 0; }
static int ble_hs_pvcy_add_entry(const uint8_t *a,uint8_t t,const uint8_t *i) { (void)a; (void)t; (void)i; return 0; }
static int ble_store_write_rpa_rec(const struct ble_store_value_rpa_rec *r) { (void)r; ++rpa_attempts; return rpa_result; }
static void ble_gap_identity_event(uint16_t h,const ble_addr_t *a) {
    (void)h; (void)a; BLE_HS_DBG_ASSERT(!host_locked); ++identity_events;
    if(identity_revokes) lease_valid=0;
}
static void ble_gap_pairing_complete_event(uint16_t h,int status) {
    (void)h; BLE_HS_DBG_ASSERT(!host_locked); ++pair_events; pair_status=status;
    pair_observed_our=disk_count(1); pair_observed_peer=disk_count(2);
}
static void ble_gap_enc_event(uint16_t h,int status,int restore,int bonded) {
    (void)h; BLE_HS_DBG_ASSERT(!host_locked); ++enc_events; enc_status=status;
    enc_bonded=bonded; enc_restore=restore;
}
static void ble_gap_passkey_event(uint16_t h,struct ble_gap_passkey_params *p) { (void)h; (void)p; abort(); }
#if WITH_ADMISSION_HOOK
int LitchSdkPairingAdmission(uint16_t h) {
    BLE_HS_DBG_ASSERT(!host_locked); ++hook_calls;
    return lease_valid && h==lease_handle && hook_epoch==session_epoch ? 0 : BLE_HS_EAUTHOR;
}
#endif
static void ble_sm_ltk_restore_exec(struct ble_sm_proc *,struct ble_sm_result *,void *);
static void ble_sm_exec(struct ble_sm_proc *p,struct ble_sm_result *r,void *arg) {
    BLE_HS_DBG_ASSERT(host_locked);
    if(p->state==BLE_SM_PROC_STATE_LTK_RESTORE) ble_sm_ltk_restore_exec(p,r,arg);
    else r->execute=0; /* RF/crypto waits; the full pairing engine is not faked. */
}
/* SDK_SMP_FUNCTIONS */

static void begin_claim(void) {
    memset(&procedure,0,sizeof procedure); procedure.conn_handle=42;
    procedure.flags=BLE_SM_PROC_F_BONDING|BLE_SM_PROC_F_SC;
    procedure.key_size=16;
    procedure.our_keys.key_size=procedure.peer_keys.key_size=16;
    procedure.our_keys.ltk_valid=procedure.peer_keys.ltk_valid=1;
    procedure.our_keys.ediv_rand_valid=procedure.peer_keys.ediv_rand_valid=1;
    memset(procedure.our_keys.ltk,17,16); memset(procedure.peer_keys.ltk,17,16);
    procedure.peer_keys.addr_valid=1; procedure.peer_keys.addr_type=1;
    procedure.peer_keys.addr[0]=42; active_proc=1;
}

int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    ble_store_config_init(); ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    ble_hs_cfg.store_write_cb=counted_store_write;
    memset(&connection,0,sizeof connection);
    connection.bhc_peer_addr.type=1; connection.bhc_peer_addr.val[0]=42;
    connection.bhc_peer_rpa_addr.val[0]=43;
    if(which<=4 || which==15) {
        /* NoIO, SC, bonding and no MITM: the selected Just Works request. */
        struct ble_sm_pair_cmd req={3,0,9,16,3,3};
        struct os_mbuf packet={&req,sizeof req},*om=&packet;
        struct ble_sm_result result={0};
        if(which==0) lease_valid=0;
        if(which==1) lease_handle=43;
        if(which==2) hook_epoch=2;
        if(which==4) packet.size=1;
        if(which==15) { begin_claim(); procedure.state=BLE_SM_PROC_STATE_SEC_REQ; lease_valid=0; }
        struct ble_sm_proc before=procedure;
        ble_sm_pair_req_rx(42,&om,&result);
        int denied=which<3 || which==15;
        if(denied) {
            CHECK(result.app_status==BLE_HS_EAUTHOR && result.sm_err==BLE_SM_ERR_PAIR_NOT_SUPP);
            CHECK(allocations==0 && removals==0 && frees==0 && capacity_calls==0);
            CHECK(!memcmp(&procedure,&before,sizeof before));
        } else if(which==4) CHECK(result.app_status==BLE_HS_EINVAL && hook_calls==0);
        else {
            CHECK(result.app_status==0 && result.execute && allocations==1 && capacity_calls==1);
            CHECK(procedure.conn_handle==42 && !memcmp(procedure.pair_req+1,&req,sizeof req));
        }
        CHECK(writes==0 && commits==0 && our_attempts==0 && peer_attempts==0);
        printf("pairReq case%d status%d hooks%d capacity%d alloc%d stores%d/%d writes%d\n",
            which,result.app_status,hook_calls,capacity_calls,allocations,our_attempts,peer_attempts,writes);
    } else if(which>=18 && which<=20) {
        begin_claim();
        rpa_result=which==20?BLE_HS_ESTORE_CAP:BLE_HS_ESTORE_FAIL;
        if(which==19) memset(connection.bhc_peer_rpa_addr.val,0,6);
        struct ble_sm_result result={0};
        ble_hs_lock();ble_sm_key_exch_success(&procedure,&result);ble_hs_unlock();
        ble_sm_process_result(42,&result,true);
        CHECK(enc_events==1 && pair_events==1 && our_attempts==1 && peer_attempts==1);
        CHECK(disk_count(1)==1 && disk_count(2)==1); /* Failure can follow key durability. */
        if(which==19) {
            CHECK(rpa_attempts==0 && enc_status==0 && enc_bonded && pair_status==0);
        } else {
            CHECK(rpa_attempts==1 && enc_status==rpa_result && result.app_status==rpa_result &&
                  !enc_bonded && !result.bonded && pair_status==BLE_SM_ERR_UNSPECIFIED);
        }
        printf("rpaResult case%d native%d bonded%d attempts%d directIdentity%d\n",
               which,enc_status,enc_bonded,rpa_attempts,which==19);
    } else if(which>=16) {
        uint8_t request[]={BLE_SM_OP_PAIR_REQ,3,0,9,16,3,3};
        struct os_mbuf packet={request,sizeof request};
        struct ble_l2cap_chan channel={42,&packet};
        if(which==16) lease_valid=0;
        int status=ble_sm_rx(&channel);
        if(which==16) {
            CHECK(status==BLE_HS_EAUTHOR && tx_failures==1);
            CHECK(allocations==0 && capacity_calls==0 && removals==0 && !active_proc);
        } else CHECK(status==0 && allocations==1 && capacity_calls==1 && active_proc);
        CHECK(writes==0 && commits==0 && our_attempts==0 && peer_attempts==0);
        printf("l2capPairReq case%d status%d hooks%d failures%d alloc%d stores%d/%d\n",
            which,status,hook_calls,tx_failures,allocations,our_attempts,peer_attempts);
    } else if(which==14) {
        struct ble_store_value_sec saved=value(42,17); saved.authenticated=0;
        CHECK(put(1,&saved)==0 && put(2,&saved)==0);
        int prior_writes=writes,prior_commits=commits;
        lease_valid=0;
        struct ble_hci_ev_le_subev_lt_key_req request={42,0,0};
        CHECK(ble_sm_ltk_req_rx(&request)==0);
        CHECK(ltk_replies==1 && ltk_nacks==0 && !memcmp(restored_ltk,saved.ltk,16));
        ble_sm_enc_event_rx(42,0,1);
        CHECK(hook_calls==0 && enc_events==1 && enc_status==0 && enc_restore && enc_bonded);
        CHECK(connection.bhc_sec_state.encrypted && connection.bhc_sec_state.bonded);
        CHECK(writes==prior_writes && commits==prior_commits && our_attempts==0 && peer_attempts==0);
        printf("quietRestore exactLTK1 hooks%d writes%d status%d restore%d\n",hook_calls,writes-prior_writes,enc_status,enc_restore);
    } else {
        begin_claim();
        if(which>=6 && which<=9) fault(which<8?BEFORE:AFTER,which%2?2:1);
        if(which==10 || which==11) {
            int full=which==10?1:2;
            for(int id=1;id<=3;++id) { struct ble_store_value_sec v=value(id,17); CHECK(put(full,&v)==0); }
        }
        if(which==12) lease_valid=0;
        if(which==13) identity_revokes=1;
        struct ble_sm_result result={0};
        ble_hs_lock(); ble_sm_key_exch_success(&procedure,&result); ble_hs_unlock();
        CHECK(connection.bhc_sec_state.bonded); /* Native flag precedes persistence. */
        int prior_writes=writes;
        ble_sm_process_result(42,&result,true);
        CHECK(enc_events==1 && pair_events==1 && removals==1 && frees==1);
        if(which==5) {
            CHECK(enc_status==0 && enc_bonded && pair_status==0);
            CHECK(our_attempts==1 && peer_attempts==1 && disk_count(1)==1 && disk_count(2)==1);
            CHECK(pair_observed_our==1 && pair_observed_peer==1);
            CHECK(exact_disk(1,&ble_store_config_our_secs[0]) && exact_disk(2,&ble_store_config_peer_secs[0]));
        } else {
            int expected=which>=12?BLE_HS_EAUTHOR:which>=10?BLE_HS_ESTORE_CAP:BLE_HS_ESTORE_FAIL;
            CHECK(enc_status==expected && result.app_status==expected && !enc_bonded && !result.bonded);
            CHECK(pair_status==BLE_SM_ERR_UNSPECIFIED && rpa_attempts==0);
            if(which>=12) CHECK(our_attempts==0 && peer_attempts==0 && writes==prior_writes);
            else if(which%2==0) CHECK(our_attempts==1 && peer_attempts==0);
            else CHECK(our_attempts==1 && peer_attempts==1);
            if(which==7 || which==9) CHECK(disk_count(1)==1 && ble_store_config_num_peer_secs==0);
            if(which==8) CHECK(disk_count(1)==1 && ble_store_config_num_our_secs==0);
            if(which==9) CHECK(disk_count(2)==1); /* Failure stays failure after durable visibility. */
        }
        printf("keyResult case%d status%d bond%d SMPstatus%d attempts%d/%d disk%d/%d readonly%d\n",
            which,enc_status,enc_bonded,pair_status,our_attempts,peer_attempts,disk_count(1),disk_count(2),read_opens);
    }
    CHECK(opened==closed && host_locked==0);
    CHECK(unpair_oldest_requests==0 && unpair_except_requests==0);
    printf("journey%d PASS\n",which); return 0;
}
