#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modules/sdk_att_witness.hpp"
#define MYNEWT_VAL(x) MYNEWT_VAL_##x
#define MYNEWT_VAL_BLE_ATT_SVR_WRITE 1
#define MYNEWT_VAL_BLE_GATT_CACHING 0
#define BLE_L2CAP_CID_ATT 4
#define BLE_HS_DBG_ASSERT(x) assert(x)
#define STATS_INC(a,b) (++stats_error)
#define OS_MBUF_PKTLEN(x) ((x)->length)
#define le16toh(x) (x)
#define htole16(x) (x)
#define LITCH_ATT_OBSERVE(phase, conn, cid, op, handle, status) \
 LitchSdkAttObserve((phase), ((uint32_t)(conn)<<16)|(uint16_t)(cid), \
 ((uint32_t)(op)<<16)|(uint16_t)(handle), (status), \
 (unsigned)os_msys_count(), (unsigned)os_msys_num_free())
struct os_mbuf { uint8_t bytes[64]; uint8_t *om_data; int length,id; bool consumed; };
struct ble_att_write_req {uint16_t bawq_handle;} __attribute__((packed));
struct ble_att_error_rsp { uint8_t baep_req_op;uint16_t baep_handle;uint8_t baep_error_code;} __attribute__((packed));
struct ble_att_svr_entry { uint16_t ha_handle_id; int (*ha_cb)(uint16_t,uint16_t,int,uint16_t,struct os_mbuf**,void*);void *ha_cb_arg; };
typedef int ble_att_rx_fn(uint16_t,uint16_t,struct os_mbuf**);
struct ble_att_rx_dispatch_entry { uint8_t bde_op;ble_att_rx_fn *bde_fn; };
struct ble_hs_conn { bool client_att_busy; };
struct ble_l2cap_chan { int unused; };
static struct os_mbuf bufs[4];
static struct ble_hs_conn conn={true};static struct ble_l2cap_chan chan;
static int allocation_calls,allocation_fail_at,access_calls,access_error,consume_access,tx_calls,tx_error_1,tx_error_2,stats_error,free_calls,prepare_fail_at,prepare_calls;
static int pool_free=8;static struct LitchSdkAttSnapshot held,housekeeping;
static unsigned os_msys_count(void){return 8;}
static unsigned os_msys_num_free(void){return (unsigned)pool_free;}
static struct os_mbuf *ble_hs_mbuf_l2cap_pkt(void){
 ++allocation_calls;
 if(allocation_calls==allocation_fail_at){pool_free=0;printf("alloc:null\n");return NULL;}
 struct os_mbuf *p=&bufs[allocation_calls];p->om_data=p->bytes;p->length=0;p->id=allocation_calls;--pool_free;printf("alloc:%d\n",p->id);return p;
}
static void os_mbuf_adj(struct os_mbuf *p,int amount){assert(p&&!p->consumed);assert(amount<=p->length);printf("adj:%d:%d\n",p->id,amount);p->om_data+=amount;p->length-=amount;}
static int os_mbuf_copydata(struct os_mbuf *p,int off,int len,void *dst){if(off+len>p->length)return -1;memcpy(dst,p->om_data+off,len);return 0;}
static int ble_hs_mbuf_pullup_base(struct os_mbuf **p,int len){return (*p)->length>=len?0:BLE_HS_EMSGSIZE;}
static void *ble_att_cmd_prepare(int op,int size,struct os_mbuf *p){
 ++prepare_calls;if(prepare_calls==prepare_fail_at){printf("prepare:null\n");return NULL;}
 assert(p&&!p->consumed);p->om_data=p->bytes;p->bytes[0]=op;p->length=size+1;printf("prepare:%d:%d\n",p->id,op);return p->bytes+1;
}
static int ble_att_tx(uint16_t handle,uint16_t cid,struct os_mbuf *p){assert(handle==0x1234&&cid==4);assert(p&&!p->consumed);++tx_calls;int rc=tx_calls==1?tx_error_1:tx_error_2;printf("tx:%d:op=%d:len=%d:rc=%d\n",p->id,p->bytes[0],p->length,rc);p->consumed=true;return rc;}
static void os_mbuf_free_chain(struct os_mbuf *p){if(p){assert(!p->consumed);p->consumed=true;++free_calls;printf("free:%d\n",p->id);}else printf("free:null\n");}
static int ble_hs_locked_by_cur_task(void){return 0;}
static int ble_att_svr_check_perms(uint16_t c,int r,struct ble_att_svr_entry *e,uint8_t *out){*out=0;printf("perms:%d\n",e->ha_handle_id);return 0;}
static int callback(uint16_t c,uint16_t h,int op,uint16_t off,struct os_mbuf **p,void *arg){
 assert(c==0x1234&&h==0x4567&&op==BLE_ATT_ACCESS_OP_WRITE&&off==0);
 assert((*p)->length==2&&(*p)->om_data[0]==0xab&&(*p)->om_data[1]==0xcd);
 ++access_calls;LitchSdkAttRead(&held);printf("access:%d:rc=%d\n",(*p)->id,access_error);
 if(consume_access){(*p)->consumed=true;*p=NULL;printf("access:consume\n");}return access_error;
}
static struct ble_att_svr_entry entry={0x4567,callback,NULL};
static struct ble_att_svr_entry *ble_att_svr_find_by_handle(uint16_t h){assert(h==entry.ha_handle_id);return &entry;}
int ble_att_svr_rx_write(uint16_t,uint16_t,struct os_mbuf**);
static int response_handler(uint16_t c,uint16_t cid,struct os_mbuf **om){printf("response_dispatch\n");return 0;}
static const struct ble_att_rx_dispatch_entry *ble_att_rx_dispatch_entry_find(uint8_t op){static const struct ble_att_rx_dispatch_entry w={BLE_ATT_OP_WRITE_REQ,ble_att_svr_rx_write},r={BLE_ATT_OP_WRITE_RSP,response_handler};return op==BLE_ATT_OP_WRITE_REQ?&w:op==BLE_ATT_OP_WRITE_RSP?&r:NULL;}
static void ble_att_rx_handle_unknown_request(uint8_t op,uint16_t c,uint16_t cid,struct os_mbuf **om){assert(0);}
static void ble_att_inc_rx_stat(uint8_t op){printf("rx_stat:%d\n",op);}
static void ble_hs_lock(void){printf("lock\n");}
static void ble_hs_unlock(void){printf("unlock\n");}
static int ble_hs_misc_conn_chan_find_reqd(uint16_t c,uint16_t cid,struct ble_hs_conn **outc,struct ble_l2cap_chan **outch){*outc=&conn;*outch=&chan;return 0;}
static int ble_att_tx_with_conn(struct ble_hs_conn *c,struct ble_l2cap_chan *ch,struct os_mbuf *p){LitchSdkAttRead(&housekeeping);assert(!c->client_att_busy&&p==NULL);printf("housekeeping:busy=0\n");return 0;}
/* EXTRACTED SDK FUNCTIONS */
int main(int argc,char **argv){
 assert(argc==2);char *test=argv[1];int expected=0;bool house=false;
 if(!strcmp(test,"consume"))consume_access=1;
 else if(!strcmp(test,"allocation_null")){allocation_fail_at=1;expected=BLE_HS_ENOMEM;}
 else if(!strcmp(test,"access_error")){access_error=BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;expected=BLE_HS_EAPP;}
 else if(!strcmp(test,"normal_tx_error")){tx_error_1=BLE_HS_EBUSY;expected=BLE_HS_EBUSY;}
 else if(!strcmp(test,"error_reply_tx_error")){access_error=BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;tx_error_1=BLE_HS_ENOTCONN;expected=BLE_HS_EAPP;}
 else if(!strcmp(test,"fallback_allocation_null")){tx_error_1=BLE_HS_EBUSY;allocation_fail_at=2;expected=BLE_HS_EBUSY;}
 else if(!strcmp(test,"housekeeping"))house=true;
 else if(!strcmp(test,"prepare_null")){prepare_fail_at=1;expected=BLE_HS_ENOMEM;}
 else assert(!strcmp(test,"success"));
 struct os_mbuf *rx=&bufs[0];rx->id=0;rx->om_data=rx->bytes;rx->length=house?1:5;
 uint8_t bytes[]={BLE_ATT_OP_WRITE_REQ,0x67,0x45,0xab,0xcd};memcpy(rx->bytes,bytes,5);if(house)rx->bytes[0]=BLE_ATT_OP_WRITE_RSP;
 int rc=ble_att_rx_extended(0x1234,4,&rx);assert(rc==expected);
 printf("result:rc=%d:rx=%s:access=%d:tx=%d:alloc=%d:free=%d:stats=%d\n",rc,rx?"retained":"null",access_calls,tx_calls,allocation_calls,free_calls,stats_error);
 struct LitchSdkAttSnapshot s;LitchSdkAttRead(&s);
 if(TRANSFORMED){
  assert(sizeof(s)==92&&s.phase==LITCH_ATT_RETURN&&s.status==expected);
  assert(s.connectionCid==0x12340004&&s.counts[LITCH_ATT_ENTER]==1&&s.counts[LITCH_ATT_RETURN]==1);
  if(house){assert(housekeeping.phase==LITCH_ATT_ENTER&&housekeeping.counts[LITCH_ATT_DISPATCH]==0);assert(s.counts[LITCH_ATT_DISPATCH]==1);}
  else {
   assert(s.opcodeHandle==0x124567&&s.counts[LITCH_ATT_REPLY_ENTER]==1&&s.counts[LITCH_ATT_REPLY_RETURN]==1);
   assert(s.allocationAttempts==(unsigned)allocation_calls);
   if(access_calls){assert(held.phase==LITCH_ATT_WRITE_ACCESS_ENTER&&held.counts[LITCH_ATT_WRITE_ACCESS_RETURN]==0);}
   if(allocation_fail_at){assert(s.allocationFailures==1&&s.minimumObservedFree==0);}
   if(expected){
    int phase=access_error?LITCH_ATT_WRITE_ACCESS_RETURN:LITCH_ATT_WRITE_BUILD;
    int failure=expected;
    if(tx_error_1){phase=LITCH_ATT_TX_RETURN;failure=tx_error_1;}
    assert(s.failedPhase==(unsigned)phase&&s.failedStatus==failure&&s.failedConnectionCid==0x12340004&&s.failedOpcodeHandle==0x124567);
    // Later successful ATT operation must not erase the inner failure record.
    LitchSdkAttObserve(LITCH_ATT_RETURN,0x98760004,0x130000,0,8,7);
    struct LitchSdkAttSnapshot later;LitchSdkAttRead(&later);
    assert(later.status==0&&later.failedPhase==s.failedPhase&&later.failedStatus==s.failedStatus&&later.failedConnectionCid==s.failedConnectionCid&&later.failedOpcodeHandle==s.failedOpcodeHandle);
   }else assert(s.failedPhase==LITCH_ATT_PHASE_COUNT&&s.allocationFailures==0);
  }
 }else{for(int i=0;i<LITCH_ATT_PHASE_COUNT;++i)assert(s.counts[i]==0);assert(s.phase==LITCH_ATT_PHASE_COUNT&&s.allocationAttempts==0&&s.allocationFailures==0);}
 printf("WITNESS phase=%u status=%d failedPhase=%u failedStatus=%d allocFail=%u minFree=%u accessEnter=%u accessReturn=%u txEnter=%u txReturn=%u\n",s.phase,s.status,s.failedPhase,s.failedStatus,s.allocationFailures,s.minimumObservedFree,s.counts[LITCH_ATT_WRITE_ACCESS_ENTER],s.counts[LITCH_ATT_WRITE_ACCESS_RETURN],s.counts[LITCH_ATT_TX_ENTER],s.counts[LITCH_ATT_TX_RETURN]);
 return 0;
}
