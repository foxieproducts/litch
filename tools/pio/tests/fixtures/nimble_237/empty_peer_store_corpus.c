/* Exact generated config/NVS owners; controlled faultable NVS and host lock. */
#define BLE_HS_EBUSY 15
/* SDK_EMPTY_RECONCILIATION */

static int write_record(int type,int id) {
    union ble_store_value v={0};
    if(type<=2) v.sec=value(id,17);
    else if(type==3) { v.cccd.peer_addr=value(id,17).peer_addr; v.cccd.chr_val_handle=7; v.cccd.flags=1; }
    else { v.csfc.peer_addr=value(id,17).peer_addr; v.csfc.data[0]=1; }
    return ble_store_write(type==4?BLE_STORE_OBJ_TYPE_CSFC:type,&v);
}
static int ram_count(int type) {
    if(type==1) return ble_store_config_num_our_secs;
    if(type==2) return ble_store_config_num_peer_secs;
    if(type==3) return ble_store_config_num_cccds;
    return ble_store_config_num_csfcs;
}
static int peer_disk_empty(void) {
    for(int t=0;t<4;++t) for(int s=0;s<8;++s) if(disk[t][s].present) return 0;
    return 1;
}
static void seed_nonpeer_records(void) {
    struct ble_store_value_local_irk identity={0}; identity.addr=value(91,0).peer_addr; identity.irk[0]=19;
    struct ble_store_value_rpa_rec rpa={0}; rpa.peer_addr=value(92,0).peer_addr; rpa.peer_rpa_addr=value(93,0).peer_addr;
    disk[4][2].present=disk[5][1].present=true;
    disk[4][2].size=sizeof identity; memcpy(disk[4][2].data,&identity,sizeof identity);
    disk[5][1].size=sizeof rpa; memcpy(disk[5][1].data,&rpa,sizeof rpa);
}
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    ble_store_config_init(); ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    seed_nonpeer_records();
    struct blob retained[2][8]; memcpy(retained,&disk[4],sizeof retained);
    if(which!=34) for(int type=1;type<=4;++type) {
        /* The actual SDK writer commits to NVS, receives failure, and restores
         * its empty RAM snapshot. This is the escaped durable-empty hazard. */
        fault(AFTER,type);
        CHECK(write_record(type,42)==BLE_HS_ESTORE_FAIL && ram_count(type)==0);
        CHECK(disk[type-1][0].present);
    }
    if(which>=30 && which<=33) CHECK(write_record(which-29,43)==0);
    struct blob before[6][8]; memcpy(before,disk,sizeof before);
    int prior_writes=writes,prior_reads=read_calls,prior_commits=commits,prior_opens=opened;
    if(which<28) {
        int table=which/7,mode=which%7;
        const int capacities[]={3,3,8,3};
        int prefix=0; for(int i=0;i<table;++i) prefix+=2*capacities[i];
        if(mode==0) open_fault=1;
        if(mode==1) read_fault_at=read_calls+prefix+1;
        if(mode==2) erase_failure=1;
        if(mode==3) fault(BEFORE,table+1);
        if(mode==4) fault(AFTER,table+1);
        if(mode==5) read_fault_at=read_calls+prefix+capacities[table]+1;
        if(mode==6) fault(DROP,table+1);
    }
    if(which==29) litch_nimble_store_restore_status=BLE_HS_ESTORE_FAIL;
    require_host_lock=1;
    int rc=LitchSdkReconcileEmptyPeerStore();
    if(which==29 || (which>=30 && which<=33)) {
        CHECK(rc==(which==29?BLE_HS_ESTORE_FAIL:BLE_HS_EBUSY));
        CHECK(writes==prior_writes && read_calls==prior_reads && commits==prior_commits && opened==prior_opens);
        CHECK(!memcmp(before,disk,sizeof before));
        printf("emptyGuard case%d rc%d noIO1 retained1\n",which,rc);
    } else {
        if(which<28) {
            CHECK(rc==BLE_HS_ESTORE_FAIL);
            CHECK(open_fault==0 && erase_failure==0 && fault_mode==0);
            read_fault_at=0;
            CHECK(LitchSdkReconcileEmptyPeerStore()==0);
        } else CHECK(rc==0);
        CHECK(peer_disk_empty());
        for(int type=1;type<=4;++type) CHECK(ram_count(type)==0);
        if(which==34) CHECK(writes==prior_writes && commits==prior_commits);
        CHECK(LitchSdkReconcileEmptyPeerStore()==0); /* Idempotent fresh proof. */
        require_host_lock=0;
        ble_store_config_init();
        CHECK(litch_nimble_store_restore_status==0 && peer_disk_empty());
        for(int type=1;type<=4;++type) CHECK(ram_count(type)==0);
        printf("emptyProof case%d firstRC%d retry0 reopenedEmpty1 writes%d commits%d freshReads%d\n",
            which,rc,writes-prior_writes,commits-prior_commits,read_calls-prior_reads);
    }
    CHECK(!memcmp(retained,&disk[4],sizeof retained));
    CHECK(opened==closed && host_locked==0);
    CHECK(unpair_oldest_requests==0 && unpair_except_requests==0);
    printf("journey%d PASS\n",which); return 0;
}
