#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modules/sdk_att_witness.hpp"
#define MYNEWT_VAL(x) MYNEWT_VAL_##x
#define MYNEWT_VAL_BLE_CONTROLLER 0
#define CONFIG_BT_NIMBLE_LEGACY_VHCI_ENABLE 1
#define SOC_ESP_NIMBLE_CONTROLLER 0
#define CONFIG_BT_CONTROLLER_ENABLED 0
#define BLE_MONITOR 1
#define BLE_HCI_DATA_HDR_SZ 4
#define BLE_HCI_PB_FIRST_NON_FLUSH 0
#define BLE_HCI_PB_MIDDLE 1
#define BLE_HS_CONN_F_TX_FRAG 4
#define BLE_HS_DBG_ASSERT(x) assert(x)
#define LITCH_ACL_OBSERVE(phase, conn, status, length) \
 LitchSdkAclObserve((phase),((uint32_t)(conn)->bhc_flags<<16)|(conn)->bhc_handle, \
 (status),(length),((uint32_t)(conn)->bhc_outstanding_pkts<<16)|ble_hs_hci_avail_pkts, \
 (unsigned)ble_hs_hci_frag_num_mbufs_free())
#define LITCH_ACL_GLOBAL(phase,status,amount) \
 LitchSdkAclObserve((phase),UINT16_MAX,(status),(amount), \
 ((uint32_t)UINT16_MAX<<16)|ble_hs_hci_avail_pkts,(unsigned)ble_hs_hci_frag_num_mbufs_free())
struct os_mbuf;
struct os_mbuf_pkthdr {uint16_t omp_len;struct os_mbuf *owner;struct os_mbuf_pkthdr *omp_next;};
struct os_mbuf {struct os_mbuf_pkthdr header;uint8_t data[128];uint8_t *om_data;int id;bool pool;};
struct queue {struct os_mbuf_pkthdr *first,*last;unsigned count;};
struct ble_hs_conn {uint16_t bhc_handle;uint8_t bhc_flags;uint16_t bhc_outstanding_pkts;struct queue bhc_tx_q;};
struct ble_l2cap_chan {uint16_t dcid;};
struct os_mbuf_pool {int unused;};
struct os_mempool {int mp_num_free,mp_num_blocks;};
struct hci_data_hdr {uint16_t hdh_handle_pb_bc,hdh_len;} __attribute__((packed));
typedef struct os_mbuf *mem_frag_alloc_fn(uint16_t,void*);
#define OS_MBUF_PKTHDR(x) (&(x)->header)
#define OS_MBUF_PKTLEN(x) ((x)->header.omp_len)
#define STAILQ_FIRST(q) ((q)->first)
#define STAILQ_INSERT_TAIL(q,elem,field) queue_add((q),(elem))
static uint16_t ble_hs_hci_buf_sz,ble_hs_hci_avail_pkts;static uint8_t ble_hs_hci_max_pkts;
static struct os_mbuf_pool ble_hs_hci_frag_mbuf_pool;
static struct os_mempool ble_hs_hci_frag_mempool={1,1};
static int next_id,split_alloc_fail,header_fail,transport_error,transport_calls,reset_calls;
static struct LitchSdkAclSnapshot inside_transport;
static struct os_mbuf *new_mbuf(unsigned length,bool pool){struct os_mbuf *p=calloc(1,sizeof(*p));assert(p);p->id=++next_id;p->header.owner=p;p->header.omp_len=length;p->om_data=p->data+16;p->pool=pool;return p;}
static void queue_add(struct queue *q,struct os_mbuf_pkthdr *p){assert(p);p->omp_next=NULL;if(q->last)q->last->omp_next=p;else q->first=p;q->last=p;++q->count;printf("queue:add:%d:length=%u\n",p->owner->id,p->omp_len);}
static int ble_hs_locked_by_cur_task(void){return 1;}
static void ble_hs_sched_reset(int rc){++reset_calls;printf("reset:%d\n",rc);}
static uint16_t ble_hs_hci_max_acl_payload_sz(void){return ble_hs_hci_buf_sz;}
static uint16_t ble_hs_hci_util_handle_pb_bc_join(uint16_t h,uint8_t pb,uint8_t bc){return h|((uint16_t)pb<<12)|((uint16_t)bc<<14);}
static void put_le16(void *dst,uint16_t n){uint8_t *p=dst;p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static struct os_mbuf *os_mbuf_get_pkthdr(struct os_mbuf_pool *pool,int size){
 if(split_alloc_fail||ble_hs_hci_frag_mempool.mp_num_free==0){printf("frag_pool:null\n");return NULL;}
 --ble_hs_hci_frag_mempool.mp_num_free;struct os_mbuf *p=new_mbuf(0,true);printf("frag_pool:%d\n",p->id);return p;
}
static struct os_mbuf *ble_hs_mbuf_acl_pkt(void){if(split_alloc_fail){printf("msys:null\n");return NULL;}struct os_mbuf *p=new_mbuf(0,false);printf("msys:%d\n",p->id);return p;}
static void os_mbuf_free_chain(struct os_mbuf *p){if(!p){printf("free:null\n");return;}printf("free:%d\n",p->id);if(p->pool)++ble_hs_hci_frag_mempool.mp_num_free;free(p);}
static int os_mbuf_appendfrom(struct os_mbuf *dst,struct os_mbuf *src,int offset,unsigned length){assert(offset==0&&length<=OS_MBUF_PKTLEN(src));memcpy(dst->om_data,src->om_data,length);dst->header.omp_len+=length;printf("append:%d:from=%d:length=%u\n",dst->id,src->id,length);return 0;}
static void os_mbuf_adj(struct os_mbuf *p,unsigned length){assert(length<=OS_MBUF_PKTLEN(p));p->om_data+=length;p->header.omp_len-=length;printf("adj:%d:%u\n",p->id,length);}
static struct os_mbuf *os_mbuf_trim_front(struct os_mbuf *p){printf("trim:%d\n",p->id);return p;}
static struct os_mbuf *os_mbuf_prepend(struct os_mbuf *p,unsigned length){if(header_fail){printf("header:null:%d\n",p->id);os_mbuf_free_chain(p);return NULL;}assert(p->om_data-p->data>=(int)length);p->om_data-=length;p->header.omp_len+=length;printf("prepend:%d:%u\n",p->id,length);return p;}
static struct os_mbuf *os_mbuf_pullup(struct os_mbuf *p,unsigned length){printf("pullup:%d:%u\n",p->id,length);return p;}
static struct os_mbuf *ble_l2cap_prepend_hdr(struct os_mbuf *p,uint16_t cid,uint16_t length){assert(cid==4&&length==OS_MBUF_PKTLEN(p));p->header.omp_len+=4;printf("l2cap_header:%d:%u\n",p->id,p->header.omp_len);return p;}
static int ble_transport_to_ll_acl(struct os_mbuf *p){++transport_calls;LitchSdkAclRead(&inside_transport);printf("transport:%d:length=%u:rc=%d\n",p->id,OS_MBUF_PKTLEN(p),transport_error);os_mbuf_free_chain(p);return transport_error;}
int ble_hs_hci_frag_num_mbufs_free(void);
int ble_hs_tx_data(struct os_mbuf*);
struct os_mbuf *mem_split_frag(struct os_mbuf**,uint16_t,mem_frag_alloc_fn*,void*);
/* EXTRACTED SDK FUNCTIONS */
int main(int argc,char **argv){
 assert(argc==2);
 if(!strcmp(argv[1],"null_now")||!strcmp(argv[1],"null_queue")){
  bool queue=!strcmp(argv[1],"null_queue");
  assert(ble_hs_hci_set_buf_sz(8,4)==0);
  struct ble_hs_conn c={.bhc_handle=0x1234};struct os_mbuf *om=NULL;
  if(queue)c.bhc_tx_q.first=(struct os_mbuf_pkthdr*)1;
  int rc=queue?ble_hs_hci_acl_tx(&c,&om):ble_hs_hci_acl_tx_now(&c,&om);
  assert(rc==(queue?BLE_HS_EAGAIN:0)&&om==NULL&&transport_calls==0);
  printf("result:null:%s:rc=%d:avail=%u:out=%u\n",queue?"queue":"now",rc,ble_hs_hci_avail_pkts,c.bhc_outstanding_pkts);
  struct LitchSdkAclSnapshot s;LitchSdkAclRead(&s);
  if(TRANSFORMED){assert(s.lengthOrDelta==0&&s.phase==(unsigned)(queue?LITCH_ACL_QUEUE_BLOCKED:LITCH_ACL_NOW_RETURN));}
  else assert(s.phase==LITCH_ACL_PHASE_COUNT);
  printf("WITNESS null phase=%u rc=%d\n",s.phase,s.status);return 0;
 }
 const char *test=argv[1];unsigned initial_credits=4,input_length=2,expected_transports=1,expected_queued=0;int expected_rc=0,expected_failure=LITCH_ACL_PHASE_COUNT;
 bool credit=false,disconnect=false;
 if(!strcmp(test,"zero_credit")){initial_credits=0;expected_transports=0;expected_queued=1;expected_failure=LITCH_ACL_REMAINDER;}
 else if(!strcmp(test,"queue_blocked")){expected_transports=0;expected_queued=2;expected_failure=LITCH_ACL_QUEUE_BLOCKED;}
 else if(!strcmp(test,"fragment_null")){input_length=12;split_alloc_fail=1;expected_transports=0;expected_queued=1;expected_failure=LITCH_ACL_FRAGMENT_NULL;}
 else if(!strcmp(test,"header_null")){input_length=12;header_fail=1;expected_transports=0;expected_rc=BLE_HS_ENOMEM;expected_failure=LITCH_ACL_HEADER_NULL;}
 else if(!strcmp(test,"partial_credit_exhausted")){input_length=12;initial_credits=1;expected_queued=1;expected_failure=LITCH_ACL_REMAINDER;}
 else if(!strcmp(test,"transport_error")){input_length=12;transport_error=BLE_HS_ENOTCONN;expected_rc=BLE_HS_ENOTCONN;expected_failure=LITCH_ACL_TRANSPORT_RETURN;}
 else if(!strcmp(test,"full_handoff")){input_length=12;expected_transports=2;}
 else if(!strcmp(test,"credit_add")){initial_credits=0;expected_transports=0;expected_queued=1;expected_failure=LITCH_ACL_REMAINDER;credit=true;}
 else if(!strcmp(test,"disconnect_credit_add")){initial_credits=0;expected_transports=0;expected_queued=1;expected_failure=LITCH_ACL_REMAINDER;credit=true;disconnect=true;}
 else if(!strcmp(test,"credit_overflow")){initial_credits=0;expected_transports=0;expected_queued=1;expected_failure=LITCH_ACL_REMAINDER;credit=true;}
 else assert(!strcmp(test,"success"));
 assert(ble_hs_hci_set_buf_sz(8,4)==0);ble_hs_hci_avail_pkts=initial_credits;
 struct ble_hs_conn conn={.bhc_handle=0x1234};struct ble_l2cap_chan chan={.dcid=4};
 if(!strcmp(test,"queue_blocked"))queue_add(&conn.bhc_tx_q,OS_MBUF_PKTHDR(new_mbuf(3,false)));
 struct os_mbuf *input=new_mbuf(input_length,false);int rc=ble_l2cap_tx(&conn,&chan,input);
 assert(rc==expected_rc&&transport_calls==(int)expected_transports&&conn.bhc_tx_q.count==expected_queued);
 if(expected_transports&&!transport_error)assert(conn.bhc_outstanding_pkts==expected_transports);
 if(credit){
  // These controlled callers deliberately have identical numerical observations.
  // A global update cannot distinguish actual completion from disconnect cleanup.
  if(disconnect)conn.bhc_outstanding_pkts=2;
  if(!strcmp(test,"credit_overflow")){ble_hs_hci_avail_pkts=UINT16_MAX;ble_hs_hci_add_avail_pkts(1);assert(reset_calls==1);}
  else ble_hs_hci_add_avail_pkts(2);
 }
 printf("result:rc=%d:queue=%u:transport=%d:avail=%u:out=%u:flags=%u:poolfree=%d:reset=%d\n",rc,conn.bhc_tx_q.count,transport_calls,ble_hs_hci_avail_pkts,conn.bhc_outstanding_pkts,conn.bhc_flags,ble_hs_hci_frag_mempool.mp_num_free,reset_calls);
 struct LitchSdkAclSnapshot s;LitchSdkAclRead(&s);assert(sizeof(s)==92);
 if(TRANSFORMED){
  assert(s.counts[LITCH_ACL_BUFFER_INIT]==1&&s.counts[LITCH_ACL_TRANSPORT_ENTER]==expected_transports&&s.counts[LITCH_ACL_TRANSPORT_RETURN]==expected_transports);
  if(expected_transports)assert(inside_transport.phase==LITCH_ACL_TRANSPORT_ENTER&&inside_transport.counts[LITCH_ACL_TRANSPORT_RETURN]==expected_transports-1);
  assert(s.failedPhase==(unsigned)expected_failure);
  if(expected_failure!=LITCH_ACL_PHASE_COUNT){
   assert((s.failedConnectionFlags&0xffff)==0x1234&&s.failedStatus==(expected_rc?expected_rc:BLE_HS_EAGAIN));
   if(credit&&reset_calls==0)assert(s.phase==LITCH_ACL_CREDIT_ADD&&s.connectionFlags==UINT16_MAX&&s.creditsOutstanding==0xffff0002U&&s.lengthOrDelta==2);
   LitchSdkAclObserve(LITCH_ACL_CREDIT_ADD,UINT16_MAX,0,4,0xffff0004U,1);
   LitchSdkAclObserve(LITCH_ACL_NOW_RETURN,0x5678,0,0,8,1);
   struct LitchSdkAclSnapshot later;LitchSdkAclRead(&later);
   assert(later.failedPhase==s.failedPhase&&later.failedStatus==s.failedStatus&&later.failedConnectionFlags==s.failedConnectionFlags&&later.failedLengthOrDelta==s.failedLengthOrDelta&&later.failedCreditsOutstanding==s.failedCreditsOutstanding&&later.failedFragmentPoolFree==s.failedFragmentPoolFree);
  }
  LitchSdkAclObserve(LITCH_ACL_PHASE_COUNT,0,1,0,0,0);LitchSdkAclRead(NULL);
 }else {for(unsigned i=0;i<LITCH_ACL_PHASE_COUNT;++i)assert(s.counts[i]==0);assert(s.phase==LITCH_ACL_PHASE_COUNT);}
 while(conn.bhc_tx_q.first){struct os_mbuf_pkthdr *head=conn.bhc_tx_q.first;conn.bhc_tx_q.first=head->omp_next;os_mbuf_free_chain(head->owner);}
 printf("WITNESS phase=%u status=%d failedPhase=%u failedStatus=%d failedConn=%08x failedN=%u failedCreditOut=%08x failedFree=%u now=%u queue=%u fragNull=%u headerNull=%u tx=%u/%u account=%u remainder=%u init=%u add=%u\n",s.phase,s.status,s.failedPhase,s.failedStatus,s.failedConnectionFlags,s.failedLengthOrDelta,s.failedCreditsOutstanding,s.failedFragmentPoolFree,s.counts[0],s.counts[1],s.counts[2],s.counts[3],s.counts[4],s.counts[5],s.counts[6],s.counts[7],s.counts[9],s.counts[10]);
 return 0;
}
