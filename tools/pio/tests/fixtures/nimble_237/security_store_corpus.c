/* Controlled NVS leaf. SDK function bodies are inserted without rewriting.
 * Host layouts are semantic fixtures; the ESP32-S3 toolchain determines target layout and linkage. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MYNEWT_VAL(x) MYNEWT_VAL_##x
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS 3
#define MYNEWT_VAL_BLE_STORE_MAX_CCCDS 8
#define MYNEWT_VAL_BLE_STORE_MAX_CSFCS 3
#define MYNEWT_VAL_BLE_STORE_CONFIG_PERSIST 1
#define MYNEWT_VAL_BLE_HOST_BASED_PRIVACY 0
#define MYNEWT_VAL_ENC_ADV_DATA 0
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_NVS_NOT_FOUND 2
#define ESP_ERR_NVS_INVALID_LENGTH 3
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NIMBLE_NVS_STR_NAME_MAX_LEN 16
#define NIMBLE_NVS_NAMESPACE "nimble_bond"
#define NIMBLE_NVS_PEER_SEC_KEY "peer_sec"
#define NIMBLE_NVS_OUR_SEC_KEY "our_sec"
#define NIMBLE_NVS_CCCD_SEC_KEY "cccd_sec"
#define NIMBLE_NVS_CSFC_SEC_KEY "csfc_sec"
#define NIMBLE_NVS_LOCAL_IRK_KEY "local_irk"
#define NIMBLE_NVS_RPA_RECORDS_KEY "rpa_rec"
#define NIMBLE_NVS_PEER_RECORDS_KEY "p_dev_rec"
#define BLE_STORE_OBJ_TYPE_OUR_SEC 1
#define BLE_STORE_OBJ_TYPE_PEER_SEC 2
#define BLE_STORE_OBJ_TYPE_CCCD 3
#define BLE_STORE_OBJ_TYPE_PEER_DEV_REC 4
#define BLE_STORE_OBJ_TYPE_PEER_ADDR 6
#define BLE_STORE_OBJ_TYPE_LOCAL_IRK 7
#define BLE_STORE_OBJ_TYPE_CSFC 8
#define BLE_HS_ENOENT 5
#define BLE_HS_ESTORE_CAP 27
#define BLE_HS_ESTORE_FAIL 28
#define BLE_HS_EUNKNOWN 17
#define BLE_HS_ENOTSUP 8
#define BLE_HS_ENOMEM 6
#define BLE_HS_EINVAL 3
#define BLE_HS_DBG_ASSERT(c) do { if (!(c)) abort(); } while(0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define BLE_HS_LOG(...) ((void)0)
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL case%d line%d: %s\n",which,__LINE__,#c); return 1; } } while(0)
typedef int esp_err_t;
typedef uint32_t nvs_handle_t;
typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
static const ble_addr_t any_addr = {0};
#define BLE_ADDR_ANY (&any_addr)
static int ble_addr_cmp(const ble_addr_t *a,const ble_addr_t *b) { return memcmp(a,b,sizeof *a); }
struct ble_store_key_sec { ble_addr_t peer_addr; uint8_t idx; };
struct ble_store_value_sec {
    ble_addr_t peer_addr; uint16_t bond_count; uint8_t key_size; uint16_t ediv;
    uint64_t rand_num; uint8_t ltk[16]; uint8_t ltk_present:1;
    uint8_t irk[16]; uint8_t irk_present:1; uint8_t csrk[16]; uint8_t csrk_present:1;
    uint32_t sign_counter; unsigned authenticated:1; uint8_t sc:1;
};
struct ble_store_value_cccd { ble_addr_t peer_addr; uint16_t chr_val_handle,flags; };
struct ble_store_value_csfc { ble_addr_t peer_addr; uint8_t data[1]; };
struct ble_store_value_local_irk { ble_addr_t addr; uint8_t irk[16]; };
struct ble_store_value_rpa_rec { ble_addr_t peer_rpa_addr,peer_addr; };
union ble_store_value { struct ble_store_value_sec sec; struct ble_store_value_cccd cccd;
    struct ble_store_value_csfc csfc; struct ble_store_value_local_irk local_irk; struct ble_store_value_rpa_rec rpa_rec; };
union ble_store_key { struct ble_store_key_sec sec; struct { ble_addr_t peer_addr; uint8_t idx; } cccd,csfc;
    struct { ble_addr_t addr; uint8_t idx; } local_irk;
    struct { ble_addr_t peer_rpa_addr; uint8_t idx; } rpa_rec; };
struct ble_store_value_sec ble_store_config_our_secs[3],ble_store_config_peer_secs[3];
struct ble_store_value_cccd ble_store_config_cccds[8];
struct ble_store_value_csfc ble_store_config_csfcs[3];
struct ble_store_value_local_irk ble_store_config_local_irks[3];
struct ble_store_value_rpa_rec ble_store_config_rpa_recs[3];
int ble_store_config_num_our_secs,ble_store_config_num_peer_secs,ble_store_config_num_cccds;
int ble_store_config_num_csfcs,ble_store_config_num_local_irks,ble_store_config_num_rpa_recs;
uint16_t ble_store_config_our_bond_count,ble_store_config_peer_bond_count;
static const char *LOG_TAG="controlled";
static void ble_store_config_print_value_sec(const struct ble_store_value_sec *s) {(void)s;}
static void ble_store_config_print_key_sec(const struct ble_store_key_sec *s) {(void)s;}
static void ble_store_key_from_value_sec(struct ble_store_key_sec *k,const struct ble_store_value_sec *v) { memset(k,0,sizeof *k); k->peer_addr=v->peer_addr; }
int ble_store_config_persist_our_secs(void),ble_store_config_persist_peer_secs(void);
int ble_restore_our_sec_nvs(void) { return 0; } /* bond-counter rollover not exercised */
int ble_restore_peer_sec_nvs(void) { return 0; }
struct blob { size_t size; bool present; unsigned char data[sizeof(struct ble_store_value_sec)+8]; };
static struct blob disk[6][8];
static struct { bool open; int mode,dirty_type; struct blob staged[6][8]; } handles[16];
static int opened,closed,writes,commits,read_opens;
static int fault_mode,fault_type,read_fault,open_fault,set_failure;
static int read_calls,read_fault_at,absent_namespace;
static int guard_required,guard_denied,guard_active,guard_begins,guard_ends,unguarded_ops;
int LitchSdkNvsBeginAccess(void) {
    ++guard_begins;
    if (guard_denied) return BLE_HS_ESTORE_FAIL;
    if (guard_active) abort();
    guard_active=1; return 0;
}
void LitchSdkNvsEndAccess(void) {
    if (!guard_active) abort();
    guard_active=0; ++guard_ends;
}
static void ObserveNvsAccess(void) { if(guard_required && !guard_active) ++unguarded_ops; }
enum { BEFORE=1,AFTER=2,DROP=3,CORRUPT=4 };
static void fault(int mode,int type) { fault_mode=mode; fault_type=type; }
static int key_index(const char *key,int *type) {
    if (!strncmp(key,"our_sec_",8)) *type=0;
    else if (!strncmp(key,"peer_sec_",9)) *type=1;
    else if (!strncmp(key,"cccd_sec_",9)) *type=2;
    else if (!strncmp(key,"csfc_sec_",9)) *type=3;
    else if (!strncmp(key,"local_irk_",10)) *type=4;
    else if (!strncmp(key,"rpa_rec_",8)) *type=5;
    else return -1;
    int index=atoi(strrchr(key,'_')+1)-1;
    return index>=0 && index<8 ? index : -1;
}
static esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *out) {
    ObserveNvsAccess();
    if (strcmp(name,NIMBLE_NVS_NAMESPACE)) abort();
    if (absent_namespace && mode==NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    if (open_fault) { --open_fault; return ESP_FAIL; }
    for (int i=1;i<16;++i) if (!handles[i].open) {
        handles[i].open=true; handles[i].mode=mode; handles[i].dirty_type=0;
        memcpy(handles[i].staged,disk,sizeof disk); *out=(uint32_t)i; ++opened;
        read_opens+=mode==NVS_READONLY; return ESP_OK;
    }
    abort();
}
static void nvs_close(nvs_handle_t h) { ObserveNvsAccess(); if (!handles[h].open) abort(); handles[h].open=false; ++closed; }
static esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size) {
    ObserveNvsAccess();
    ++read_calls;
    if (read_calls==read_fault_at) return ESP_FAIL;
    if (read_fault) { --read_fault; return ESP_FAIL; }
    int type,index=key_index(key,&type);
    if (index<0) return ESP_ERR_NVS_NOT_FOUND;
    struct blob *b=&handles[h].staged[type][index];
    if (!b->present) return ESP_ERR_NVS_NOT_FOUND;
    if (out && *size<b->size) { *size=b->size; return ESP_ERR_NVS_INVALID_LENGTH; }
    if (out) memcpy(out,b->data,b->size);
    *size=b->size; return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t size) {
    ObserveNvsAccess();
    if (handles[h].mode!=NVS_READWRITE) abort();
    int type,index=key_index(key,&type); if(index<0||size>sizeof disk[0][0].data) abort();
    struct blob *b=&handles[h].staged[type][index];
    b->present=true; b->size=size; memcpy(b->data,value,size);
    handles[h].dirty_type=type+1; ++writes;
    if(set_failure) { int failure=set_failure; set_failure=0;
        if(failure==2) memcpy(disk,handles[h].staged,sizeof disk);
        return ESP_FAIL;
    }
    return ESP_OK;
}
static esp_err_t nvs_erase_key(nvs_handle_t h,const char *key) {
    ObserveNvsAccess();
    if (handles[h].mode!=NVS_READWRITE) abort();
    int type,index=key_index(key,&type); if(index<0) abort();
    if(!handles[h].staged[type][index].present) return ESP_ERR_NVS_NOT_FOUND;
    handles[h].staged[type][index].present=false; handles[h].dirty_type=type+1; ++writes; return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h) {
    ObserveNvsAccess();
    ++commits;
    int mode=handles[h].dirty_type==fault_type ? fault_mode : 0;
    if(mode) fault_mode=0;
    if(mode==BEFORE) return ESP_FAIL;
    if(mode!=DROP) memcpy(disk,handles[h].staged,sizeof disk);
    if(mode==CORRUPT) for(int i=0;i<3;++i) if(disk[handles[h].dirty_type-1][i].present) {
        disk[handles[h].dirty_type-1][i].data[offsetof(struct ble_store_value_sec,ltk)]^=1; break;
    }
    return mode==AFTER ? ESP_FAIL : ESP_OK;
}
int ble_store_util_delete_all(int type,const union ble_store_key *key);
/* SDK_FUNCTIONS */
static int ble_store_delete(int type,const union ble_store_key *key) {
    if(type==BLE_STORE_OBJ_TYPE_OUR_SEC) return ble_store_config_delete_our_sec(&key->sec);
    if(type==BLE_STORE_OBJ_TYPE_PEER_SEC) return ble_store_config_delete_peer_sec(&key->sec);
    return BLE_HS_ENOENT; /* auxiliary tables excluded from this custody slice */
}
/* SDK_DELETE_UTILS */
static struct ble_store_value_sec value(int id,int marker) {
    struct ble_store_value_sec v; memset(&v,0,sizeof v);
    v.peer_addr.type=1; v.peer_addr.val[0]=(uint8_t)id;
    v.key_size=16; v.ltk_present=1; memset(v.ltk,marker,sizeof v.ltk);
    v.irk_present=1; memset(v.irk,marker+1,sizeof v.irk); v.sc=1; v.authenticated=1;
    return v;
}
static int put(int type,const struct ble_store_value_sec *v) { return type==1 ? ble_store_config_write_our_sec(v) : ble_store_config_write_peer_sec(v); }
static int disk_count(int type) { int n=0; for(int i=0;i<3;++i) n+=disk[type-1][i].present; return n; }
static int exact_disk(int type,const struct ble_store_value_sec *v) {
    for(int i=0;i<3;++i) if(disk[type-1][i].present && disk[type-1][i].size==sizeof *v && !memcmp(disk[type-1][i].data,v,sizeof *v)) return 1;
    return 0;
}
static int reboot(void) {
    memset(ble_store_config_our_secs,0,sizeof ble_store_config_our_secs);
    memset(ble_store_config_peer_secs,0,sizeof ble_store_config_peer_secs);
    ble_store_config_num_our_secs=ble_store_config_num_peer_secs=0;
    ble_store_config_num_cccds=ble_store_config_num_csfcs=0;
    ble_store_config_num_local_irks=ble_store_config_num_rpa_recs=0;
    ble_store_config_our_bond_count=ble_store_config_peer_bond_count=0;
    return ble_nvs_restore_sec_keys();
}
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    int type=which%2+1;
    struct ble_store_value_sec first=value(1,17), second=value(2,34), third=value(3,51);
    CHECK(put(1,&first)==0 && put(2,&first)==0);
    struct ble_store_value_sec before=type==1?ble_store_config_our_secs[0]:ble_store_config_peer_secs[0];
    union ble_store_key key={0}; key.sec.peer_addr=first.peer_addr;
    int rc=0;
    if(which==0) {
        CHECK(reboot()==0); CHECK(ble_store_config_num_our_secs==1 && ble_store_config_num_peer_secs==1);
        CHECK(exact_disk(1,&ble_store_config_our_secs[0]) && exact_disk(2,&ble_store_config_peer_secs[0]));
    } else if(which==1 || which==2) {
        first.ltk[0]=99; CHECK(put(type,&first)==0);
        struct ble_store_value_sec expected=type==1?ble_store_config_our_secs[0]:ble_store_config_peer_secs[0];
        printf("checkpoint case%d same_count returned0 exactPersisted%d\n",which,exact_disk(type,&expected));
        CHECK(exact_disk(type,&expected)); CHECK(reboot()==0);
        CHECK(!memcmp(type==1?&ble_store_config_our_secs[0]:&ble_store_config_peer_secs[0],&expected,sizeof expected));
    } else if(which>=3 && which<=6) {
        int failed_type=which%2+1; fault(which<=4?BEFORE:AFTER,failed_type);
        rc=ble_store_util_delete_peer(&first.peer_addr); CHECK(rc==BLE_HS_ESTORE_FAIL);
        int retained=failed_type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs;
        int retry=ble_store_util_delete_peer(&first.peer_addr);
        printf("checkpoint case%d failedDelete%d retainedRAM%d retry%d durable%d/%d\n",which,rc,retained,retry,disk_count(1),disk_count(2));
        CHECK(retained==1 && retry==0);
        CHECK(disk_count(1)==0 && disk_count(2)==0);
        CHECK(reboot()==0 && ble_store_config_num_our_secs==0 && ble_store_config_num_peer_secs==0);
    } else if(which==7 || which==8) {
        fault(BEFORE,type); rc=put(type,&second); CHECK(rc==BLE_HS_ESTORE_FAIL);
        CHECK((type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs)==1);
        CHECK(!memcmp(type==1?&ble_store_config_our_secs[0]:&ble_store_config_peer_secs[0],&before,sizeof before));
        CHECK(put(type,&second)==0); CHECK(disk_count(type)==2); CHECK(reboot()==0);
        CHECK((type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs)==2);
    } else if(which==9) {
        first.ltk[0]=99; fault(BEFORE,type); rc=put(type,&first); CHECK(rc==BLE_HS_ESTORE_FAIL);
        CHECK(!memcmp(type==1?&ble_store_config_our_secs[0]:&ble_store_config_peer_secs[0],&before,sizeof before));
        CHECK(put(type,&first)==0); CHECK(exact_disk(type,type==1?&ble_store_config_our_secs[0]:&ble_store_config_peer_secs[0]));
    } else if(which==10 || which==11) {
        fault(which==10?DROP:CORRUPT,type); rc=put(type,&second); CHECK(rc==BLE_HS_ESTORE_FAIL);
        CHECK((type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs)==1);
    } else if(which==12) {
        CHECK(put(type,&second)==0 && put(type,&third)==0);
        int prior=writes;
        struct ble_store_value_sec *v=type==1?ble_store_config_our_secs:ble_store_config_peer_secs;
        struct ble_store_value_sec swap=v[0]; v[0]=v[2]; v[2]=swap;
        CHECK((type==1?ble_store_config_persist_our_secs():ble_store_config_persist_peer_secs())==0);
        CHECK(writes==prior); CHECK(reboot()==0);
        CHECK((type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs)==3);
    } else if(which==13) {
        CHECK(put(type,&second)==0 && put(type,&third)==0);
        struct ble_store_value_sec *v=type==1?ble_store_config_our_secs:ble_store_config_peer_secs;
        struct ble_store_value_sec expected[3]; memcpy(expected,v,sizeof expected);
        key.sec.peer_addr=second.peer_addr; fault(BEFORE,type);
        rc=ble_store_util_delete_all(type,&key); CHECK(rc==BLE_HS_ESTORE_FAIL);
        CHECK(!memcmp(v,expected,sizeof expected));
        CHECK(ble_store_util_delete_all(type,&key)==0 && disk_count(type)==2);
        CHECK(reboot()==0 && exact_disk(type,&expected[0]) && exact_disk(type,&expected[2]));
    } else if(which==14) {
        read_fault=1; rc=put(type,&second); CHECK(rc==BLE_HS_ESTORE_FAIL);
        CHECK((type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs)==1);
        CHECK(disk_count(type)==1);
    } else if(which==15) {
        fault(AFTER,type); rc=put(type,&second); CHECK(rc==BLE_HS_ESTORE_FAIL);
        CHECK((type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs)==1);
        CHECK(disk_count(type)==2); CHECK(reboot()==0);
        CHECK((type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs)==2);
    } else if(which==16) {
        disk[type-1][0].size=sizeof first+1;
        rc=put(type,&second); CHECK(rc==BLE_HS_ESTORE_FAIL);
        CHECK((type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs)==1);
    } else if(which==17) {
        disk[type-1][1]=disk[type-1][0];
        rc=put(type,&second); CHECK(rc==BLE_HS_ESTORE_FAIL);
    } else if(which==18) {
        open_fault=1; rc=put(type,&second); CHECK(rc==BLE_HS_ESTORE_FAIL);
        CHECK(disk_count(type)==1);
        CHECK((type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs)==1);
    } else if(which==19 || which==20) {
        set_failure=which==19?1:2; rc=put(type,&second); CHECK(rc==BLE_HS_ESTORE_FAIL);
        CHECK((type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs)==1);
        CHECK(disk_count(type)==(which==19?1:2));
        if(which==19) CHECK(put(type,&second)==0);
        CHECK(reboot()==0);
        CHECK((type==1?ble_store_config_num_our_secs:ble_store_config_num_peer_secs)==2);
    } else return 2;
    CHECK(opened==closed);
    printf("case%d rc%d RAM%d/%d durable%d/%d writes%d commits%d freshReadonly%d PASS\n",which,rc,
           ble_store_config_num_our_secs,ble_store_config_num_peer_secs,disk_count(1),disk_count(2),writes,commits,read_opens);
    return 0;
}
