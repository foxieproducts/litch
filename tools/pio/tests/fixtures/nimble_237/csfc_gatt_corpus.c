/* Controlled mbuf/connection/scheduler leaves; real SDK bodies inserted below.
 * This is host evidence, not ESP32 ABI, RF or physical NVS acceptance. */

#define MYNEWT_VAL_BLE_GATT_CSFC_SIZE 1
#define MYNEWT_VAL_BLE_GATT_CACHING 0
#define MYNEWT_VAL_BLE_DYNAMIC_SERVICE 0
#define BLE_GATT_CHR_CLI_SUP_FEAT_MASK 7
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
#define BLE_ATT_ERR_VALUE_NOT_ALLOWED 19
#define BLE_HS_ENOTCONN 7
#define BLE_GATT_ACCESS_OP_READ_CHR 0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_GATTS_CLT_CFG_F_MODIFIED 0x80
#define BLE_GAP_SUBSCRIBE_REASON_RESTORE 1
#define BLE_ATT_OP_NOTIFY_REQ 27
#define BLE_ATT_OP_INDICATE_REQ 29
struct os_mbuf { uint8_t data[8]; uint16_t len; };
static uint16_t os_mbuf_len(struct os_mbuf *om) { return om->len; }
static int os_mbuf_copydata(struct os_mbuf *om,int offset,int len,void *out) {
    if(offset<0 || len<0 || offset+len>om->len) return -1;
    memcpy(out,om->data+offset,(size_t)len); return 0;
}
static int os_mbuf_append(struct os_mbuf *om,const void *data,size_t len) {
    if(om->len+len>sizeof om->data) abort();
    memcpy(om->data+om->len,data,len); om->len+=(uint16_t)len; return 0;
}
struct ble_gatts_clt_cfg { uint16_t chr_val_handle,flags; };
struct ble_gatts_conn {
    uint8_t peer_cl_sup_feat[1]; int num_clt_cfgs; struct ble_gatts_clt_cfg *clt_cfgs;
};
struct ble_hs_conn {
    ble_addr_t bhc_peer_addr; struct { bool bonded; } bhc_sec_state;
    struct ble_gatts_conn bhc_gatt_svr;
};
static struct ble_hs_conn live;
static int conn_present=1;
static struct ble_hs_conn *ble_hs_conn_find(uint16_t handle) { return handle==1 && conn_present?&live:NULL; }
static int ble_hs_misc_peer_addr_type_to_id(int type) { return type; }
struct ble_gatt_access_ctxt { int op; struct os_mbuf *om; };
/* CCCD is empty in this focused journey; unexpected activity aborts. */
static struct ble_gatts_clt_cfg *ble_gatts_clt_cfg_find(struct ble_gatts_clt_cfg *cfg,uint16_t handle) {
    (void)cfg; (void)handle; abort();
}
static uint8_t ble_gatts_schedule_update(struct ble_hs_conn *conn,struct ble_gatts_clt_cfg *cfg) {
    (void)conn; (void)cfg; abort();
}
static void ble_gatts_subscribe_event(uint16_t a,uint16_t b,int c,int d,int e) {
    (void)a; (void)b; (void)c; (void)d; (void)e; abort();
}
static int ble_gatts_notify(uint16_t a,uint16_t b) { (void)a; (void)b; abort(); }
static void ble_gatts_indicate(uint16_t a,uint16_t b) { (void)a; (void)b; abort(); }


static int pending_action,callback_action,csfc_callbacks,request_active;
static int (*real_store_write)(int,const union ble_store_value *);
static ble_addr_t request_peer;
static void OnHostUnlocked(void) {
    int action=pending_action; pending_action=0;
    if(action==1) conn_present=0;
    else if(action==2) { live.bhc_peer_addr=value(99,0).peer_addr; live.bhc_gatt_svr.peer_cl_sup_feat[0]=4; }
    else if(action==3 && ble_store_util_delete_peer(&request_peer)!=0) abort();
}
static int ObserveStoreWrite(int type,const union ble_store_value *record) {
    if(request_active && type==BLE_STORE_OBJ_TYPE_CSFC) {
        if(CSFC_STRICT_LOCK && host_locked!=1) abort();
        ++csfc_callbacks; pending_action=callback_action;
    }
    return real_store_write(type,record);
}

/* SDK_GATT_FUNCTIONS */

static int request(int feature) {
    request_active=1; request_peer=live.bhc_peer_addr;
    struct os_mbuf mbuf={.data={(uint8_t)feature},.len=1};
    struct ble_gatt_access_ctxt ctxt={.op=BLE_GATT_ACCESS_OP_WRITE_CHR,.om=&mbuf};
    int rc=ble_svc_gatt_cl_sup_feat_access(1,99,&ctxt,NULL); request_active=0; return rc;
}
static int read_live(void) {
    struct os_mbuf mbuf={0};
    struct ble_gatt_access_ctxt ctxt={.op=BLE_GATT_ACCESS_OP_READ_CHR,.om=&mbuf};
    if(ble_svc_gatt_cl_sup_feat_access(1,99,&ctxt,NULL)!=0 || mbuf.len!=1) abort();
    return mbuf.data[0];
}
static int stored_feature(void) {
    struct ble_store_key_csfc key={.peer_addr=live.bhc_peer_addr};
    struct ble_store_value_csfc actual={0};
    int rc=ble_store_read_csfc(&key,&actual);
    return rc ? -rc : actual.csfc[0];
}
static int disk_feature(void) {
    for(int slot=0;slot<3;++slot) if(disk[3][slot].present) {
        const struct ble_store_value_csfc *record=(const void *)disk[3][slot].data;
        if(!ble_addr_cmp(&record->peer_addr,&live.bhc_peer_addr)) return record->csfc[0];
    }
    return -BLE_HS_ENOENT;
}
static void reconnect(int reopen) {
    if(reopen) ble_store_config_init();
    live.bhc_gatt_svr.peer_cl_sup_feat[0]=0;
    ble_gatts_bonding_restored(1);
}
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    ble_store_config_init(); ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    live.bhc_peer_addr=value(1,0).peer_addr; live.bhc_sec_state.bonded=true;
    union ble_store_value sec={0}; sec.sec=value(1,17);
    CHECK(ble_store_write(BLE_STORE_OBJ_TYPE_OUR_SEC,&sec)==0);
    CHECK(ble_store_write(BLE_STORE_OBJ_TYPE_PEER_SEC,&sec)==0);
    struct ble_store_value_csfc retained={.peer_addr=value(2,0).peer_addr,.csfc={1}};
    CHECK(ble_store_write_csfc(&retained)==0);
    struct ble_store_value_csfc before={.peer_addr=live.bhc_peer_addr,.csfc={1}};
    if(which!=5) CHECK(ble_store_write_csfc(&before)==0);
    live.bhc_gatt_svr.peer_cl_sup_feat[0]=which==5?0:1;
    int prior_live=read_live(), desired=which==6?1:3;
    real_store_write=ble_hs_cfg.store_write_cb; ble_hs_cfg.store_write_cb=ObserveStoreWrite;
    if(which>=7) {
        if(which==7) {
            struct ble_store_key_csfc key={.peer_addr=live.bhc_peer_addr};
            CHECK(ble_store_delete_csfc(&key)==0);
            for(int id=3;id<=4;++id) { struct ble_store_value_csfc item={.peer_addr=value(id,0).peer_addr,.csfc={1}}; CHECK(ble_store_write_csfc(&item)==0); }
        }
        if(which==8) ble_hs_cfg.store_write_cb=NULL;
        if(which==9) conn_present=0;
        if(which>=10) callback_action=which-9;
        int rc=request(desired);
        printf("edge%d rc%d live%d callbacks%d present%d pending%d\n",which,rc,live.bhc_gatt_svr.peer_cl_sup_feat[0],csfc_callbacks,conn_present,pending_action);
        CHECK(host_locked==0 && pending_action==0 && opened==closed);
        if(which==7) CHECK(rc==BLE_ATT_ERR_INSUFFICIENT_RES && read_live()==1 && stored_feature()==-BLE_HS_ENOENT && ble_store_config_num_csfcs==3);
        if(which==8) { CHECK(rc==BLE_ATT_ERR_UNLIKELY && read_live()==1 && stored_feature()==1); ble_hs_cfg.store_write_cb=ObserveStoreWrite; CHECK(request(desired)==0); }
        if(which==9) { CHECK(rc==BLE_ATT_ERR_UNLIKELY && csfc_callbacks==0 && disk_feature()==1); conn_present=1; CHECK(request(desired)==0); }
        if(which==10) CHECK(rc==0 && !conn_present && csfc_callbacks==1 && disk_feature()==3);
        if(which==11) { CHECK(rc==0 && csfc_callbacks==1 && live.bhc_peer_addr.val[0]==99 && read_live()==4); live.bhc_peer_addr=request_peer; CHECK(stored_feature()==3); }
        if(which==12) CHECK(rc==0 && csfc_callbacks==1 && stored_feature()==-BLE_HS_ENOENT && disk_feature()==-BLE_HS_ENOENT && ble_store_config_num_our_secs==0 && ble_store_config_num_peer_secs==0);
        CHECK(unpair_oldest_requests==0 && unpair_except_requests==0);
        return 0;
    }
    if(which==1 || which==6) set_failure=1; /* actual nvs_set_blob fails before visibility */
    if(which==2) set_failure=2; /* actual nvs_set_blob changes storage then fails */
    if(which==3) fault(BEFORE,4); /* fail first CSFC commit before visibility */
    if(which==4) fault(AFTER,4); /* first CSFC commit visible then reports failure */
    int prior_writes=writes,prior_commits=commits;
    int failed=request(desired), failed_live=read_live(), failed_ram=stored_feature(),failed_disk=disk_feature();
    if(which==6 && CSFC_STRICT_LOCK) CHECK(failed==0 && writes==prior_writes && commits==prior_commits && set_failure==1);
    int first_retry=-99,reconnect_live=-99,reconnect_retry=-99,reopen_live=-99;
    if(which==4) { reconnect(1); reopen_live=read_live(); }
    first_retry=request(desired);
    reconnect(0); reconnect_live=read_live(); reconnect_retry=request(desired);
    reconnect(1); reopen_live=read_live();
    int final_ram=stored_feature(),final_disk=disk_feature();
    struct ble_store_key_csfc other_key={.peer_addr=retained.peer_addr};
    struct ble_store_value_csfc other={0};
    int retained_ok=ble_store_read_csfc(&other_key,&other)==0 && !memcmp(&retained,&other,sizeof retained);
    printf("case%d first_rc%d failed_live%d prior_live%d failed_ram%d failed_disk%d retry%d reconnect_live%d reconnect_retry%d reopen_live%d final_ram%d final_disk%d retained%d\n",
        which,failed,failed_live,prior_live,failed_ram,failed_disk,first_retry,
        reconnect_live,reconnect_retry,reopen_live,final_ram,final_disk,retained_ok);
    CHECK(retained_ok && ble_store_config_num_our_secs==1 && ble_store_config_num_peer_secs==1);
    CHECK(opened==closed && host_locked==0 && unpair_oldest_requests==0 && unpair_except_requests==0);
    int injected_failure=which>=1 && which<=4;
    int expected_status=injected_failure?BLE_ATT_ERR_UNLIKELY:0;
    CHECK(failed==expected_status);
    if(injected_failure) {
        CHECK(failed_live==prior_live && failed_ram==prior_live);
        /* An error after visibility does not promise a flash rollback. */
        CHECK(failed_disk==((which==2 || which==4)?desired:prior_live));
        CHECK(set_failure==0 && fault_mode==0);
    } else {
        CHECK(failed_live==desired && failed_ram==desired && failed_disk==desired);
    }
    CHECK(first_retry==0 && reconnect_retry==0);
    CHECK(reopen_live==desired && final_ram==desired && final_disk==desired);
    return 0;
}
