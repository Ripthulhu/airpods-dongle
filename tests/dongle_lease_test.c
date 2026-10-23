// SPDX-License-Identifier: GPL-3.0-only
#define main rpc_fragment_tests
#include "dongle_rpc_test.c"
#undef main

static uint32_t tokens[DONGLE_CLIENT_COUNT];
static void fixture(void){
    memset(clients,0,sizeof(clients));memset(tokens,0,sizeof(tokens));
    transport_owner=lease_owner=-1;lease_token=0;
    clock_ms=100;connection=10;microphone=false;att_result=aacp_result=setting_result=0;
    att_sent=aacp_sent=cancellations=0;
    for(unsigned i=0;i<DONGLE_CLIENT_COUNT;++i)dongle_rpc_client_available((uint8_t)i,true);
    dongle_rpc_poll();
}
static void frame(unsigned client,uint8_t op,uint32_t seq,const uint8_t *p,uint8_t length){
    uint8_t report[64]={3,'A','P','D','G',1,64};report[7]=op;
    dongle_put32(report+8,connection);dongle_put32(report+12,seq);
    report[16]=report[20]=length;if(length)memcpy(report+22,p,length);
    assert(dongle_rpc_client_set((uint8_t)client,report));dongle_rpc_poll();
}
static void transact(unsigned client,uint8_t op,const uint8_t *p,uint8_t length){
    uint32_t seq=clients[client].sequence+1;if(!seq)++seq;
    frame(client,op,seq,p,length);frame(client,6,seq,NULL,0);
}
static void lease(unsigned client,uint8_t action,uint32_t token){
    uint8_t p[6]={10,action};dongle_put32(p+2,token);transact(client,7,p,sizeof(p));
}
static void acquire(unsigned client){
    lease(client,0,0);
    assert(clients[client].phase==COMPLETE && clients[client].total==5 && clients[client].payload[0]==1);
    tokens[client]=dongle_u32(clients[client].payload+1);assert(tokens[client]);
}
static void raw(unsigned client){
    const uint8_t p[]={0x0a,0x14,0};transact(client,1,p,sizeof(p));
}
static void complete(void){att_result=1;dongle_rpc_poll();}

int main(void){
    fixture();raw(0);assert(clients[0].failure==STALE && !att_sent);
    acquire(0);assert(dongle_rpc_busy());
    lease(1,0,0);assert(clients[1].failure==BUSY && lease_owner==0);
    lease(0,0,0);assert(clients[0].failure==BUSY && lease_owner==0);
    raw(0);assert(att_sent==1 && dongle_rpc_transport_current());complete();
    // Ownership spans the baseline read, write and verification, not just a PDU.
    assert(lease_owner==0 && transport_owner==-1);
    raw(1);assert(clients[1].failure==BUSY && att_sent==1);
    const uint8_t setting[]={1,0x28,1,0,0,0};
    transact(1,7,setting,sizeof(setting));assert(clients[1].failure==BUSY);
    const uint8_t snapshot[]={2};transact(1,7,snapshot,sizeof(snapshot));assert(clients[1].phase==COMPLETE);
    raw(0);complete();raw(0);complete();assert(att_sent==3);
    lease(1,2,tokens[0]);assert(clients[1].failure==STALE && lease_owner==0);
    lease(0,2,tokens[0]);assert(clients[0].phase==COMPLETE && lease_owner<0);
    acquire(1);raw(1);complete();assert(att_sent==4);

    fixture();acquire(0);raw(0);
    frame(0,5,clients[0].sequence,NULL,0);
    assert(transport_owner<0 && lease_owner==0 && clients[0].failure==CANCELLED);
    raw(0);complete(); // A failed PDU still permits owned mode-restoration cleanup.
    lease(0,2,tokens[0]);assert(lease_owner<0);

    fixture();acquire(0);clock_ms+=LEASE_IDLE_MS-1;
    lease(0,1,tokens[0]);assert(lease_owner==0 && clients[0].phase==COMPLETE);
    clock_ms+=LEASE_IDLE_MS;dongle_rpc_poll();assert(lease_owner<0 && clients[0].failure==TIMEOUT);
    acquire(1);uint32_t other_token=tokens[1];
    lease(0,1,tokens[0]);assert(clients[0].failure==STALE && lease_token==other_token);
    raw(0);assert(clients[0].failure==BUSY);

    fixture();acquire(0);
    for(unsigned elapsed=4000;elapsed<LEASE_MAX_MS;elapsed+=4000){
        clock_ms=100+elapsed;lease(0,1,tokens[0]);assert(lease_owner==0);
    }
    clock_ms=100+LEASE_MAX_MS;dongle_rpc_poll();assert(lease_owner<0 && clients[0].failure==TIMEOUT);

    for(unsigned loss=0;loss<3;++loss){
        fixture();acquire(0);raw(0);assert(dongle_rpc_transport_current());
        if(loss==0)dongle_rpc_client_available(0,false);
        if(loss==1)++connection;
        if(loss==2)clock_ms+=LEASE_IDLE_MS;
        assert(!dongle_rpc_transport_current()); // Checked before main-loop cleanup too.
        dongle_rpc_poll();assert(lease_owner<0 && transport_owner<0 && cancellations);
    }
    fixture();clock_ms=UINT32_MAX-100;acquire(0);clock_ms+=200;
    lease(0,1,tokens[0]);assert(lease_owner==0);lease(0,2,tokens[0]);assert(lease_owner<0);
    puts("settings lease: cross-client edit isolation, cached access, tokens, cleanup, deadlines and IRQ revocation passed");
}
