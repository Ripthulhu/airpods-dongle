// SPDX-License-Identifier: GPL-3.0-only
#include "dongle_rpc.h"
#include "dongle_protocol.h"
#include "dongle_control.h"
#include "dongle_features.h"
#include "uac.h"
#include "../btstack/btstack_att.h"
#include "../btstack/btstack_aacp.h"
#include "hardware/sync.h"

#define RPC_LIMIT 512u
#define CHUNK 42u
#define LEASE_IDLE_MS 5000u
#define LEASE_MAX_MS 30000u
enum { IDLE, ASSEMBLING, RUNNING, COMPLETE, FAILED };
enum { MALFORMED=1, BUSY, STALE, DISCONNECTED, TIMEOUT, TRANSPORT, UNSUPPORTED, CANCELLED };
typedef struct {
    uint8_t incoming[64], published[64], payload[RPC_LIMIT];
    volatile bool queued, online;
    volatile uint32_t epoch;
    uint32_t seen_epoch, target, sequence, touched;
    uint16_t total, received, page;
    uint8_t operation, phase, failure;
    bool dirty;
} rpc_client_t;
static rpc_client_t clients[DONGLE_CLIENT_COUNT];
static int transport_owner = -1;
static int lease_owner = -1;
static uint32_t lease_target, lease_token, lease_serial, lease_touched, lease_started;
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void abort_operation(unsigned client) {
    if (transport_owner != (int)client) return;
    dongle_att_cancel(); aacp_research_cancel(); aacp_setting_cancel(); transport_owner = -1;
}
static void release_lease(void) { lease_owner=-1; lease_token=0; }
static void fail(unsigned client, uint8_t why) {
    abort_operation(client);
    rpc_client_t *c = &clients[client];
    c->phase=FAILED; c->failure=why; c->total=0; c->received=c->page=0;
    c->dirty=true;
}
bool dongle_rpc_busy(void) {
    if(lease_owner>=0)return true;
    for (unsigned i=0;i<DONGLE_CLIENT_COUNT;i++)
        if (clients[i].phase==ASSEMBLING || clients[i].phase==RUNNING) return true;
    return false;
}
static bool lease_current(uint32_t now) {
    return lease_owner>=0 && clients[lease_owner].online &&
        clients[lease_owner].seen_epoch==clients[lease_owner].epoch &&
        lease_target==dongle_control_client_generation((uint8_t)lease_owner) &&
        (uint32_t)(now-lease_touched)<LEASE_IDLE_MS &&
        (uint32_t)(now-lease_started)<LEASE_MAX_MS;
}
bool dongle_rpc_transport_current(void) {
    if(transport_owner<0)return false;
    rpc_client_t *c=&clients[transport_owner];
    if(!c->online || c->seen_epoch!=c->epoch ||
        c->target!=dongle_control_client_generation((uint8_t)transport_owner))return false;
    if(c->operation==1 || c->operation==2 || lease_owner>=0)
        return lease_owner==transport_owner && lease_current(btstack_run_loop_get_time_ms());
    return true;
}
static uint8_t lease_request(unsigned client,const uint8_t *p,uint16_t size,
                              uint8_t *out,uint16_t *length,uint32_t now) {
    if(size!=6 || p[1]>2)return MALFORMED;
    uint8_t action=p[1];uint32_t token=dongle_u32(p+2);
    if(action==0){
        if(token)return MALFORMED;
        if(lease_owner>=0)return BUSY;
        lease_owner=(int)client;lease_target=clients[client].target;
        if(!++lease_serial)++lease_serial;
        lease_token=lease_serial;lease_touched=lease_started=now;
        out[0]=1;dongle_put32(out+1,lease_token);*length=5;
    }else{
        if(lease_owner!=(int)client || !token || token!=lease_token || !lease_current(now))return STALE;
        if(action==1)lease_touched=now;
        else release_lease();
    }
    return 0;
}
void dongle_rpc_client_available(uint8_t client, bool available) {
    if (client>=DONGLE_CLIENT_COUNT) return;
    clients[client].online=available; clients[client].queued=false; ++clients[client].epoch;
}
void dongle_rpc_available(bool on) { dongle_rpc_client_available(DONGLE_CLIENT_USB,on); }
bool dongle_rpc_client_get(uint8_t client, uint8_t out[64]) {
    if (client>=DONGLE_CLIENT_COUNT || !out || !clients[client].online ||
        clients[client].seen_epoch!=clients[client].epoch || clients[client].published[0]!=3) return false;
    memcpy(out,clients[client].published,64); return true;
}
uint16_t dongle_rpc_get(uint8_t *out,uint16_t capacity) {
    uint8_t report[64];
    if (!out || capacity<63 || !dongle_rpc_client_get(DONGLE_CLIENT_USB,report)) return 0;
    memcpy(out,report+1,63); return 63;
}
bool dongle_rpc_client_set(uint8_t client,const uint8_t report[64]) {
    if (client>=DONGLE_CLIENT_COUNT || !report) return false;
    rpc_client_t *c=&clients[client];
    if (!c->online || c->queued) return false;
    memcpy(c->incoming,report,64); c->queued=true; return true;
}
void dongle_rpc_set(const uint8_t *buffer,uint16_t length) {
    if (!buffer || length!=63) return;
    uint8_t report[64]={3}; memcpy(report+1,buffer,63);
    (void)dongle_rpc_client_set(DONGLE_CLIENT_USB,report);
}
static bool header_valid(const uint8_t *p) {
    if (p[0]!=3 || memcmp(p+1,"APDG",4) || p[5]!=1 || p[6]!=64 || p[20]>CHUNK || p[21]) return false;
    for (unsigned i=22+p[20];i<64;i++) if (p[i]) return false;
    return true;
}
static void handle(unsigned client,const uint8_t *p,uint32_t now) {
    rpc_client_t *c=&clients[client];
    uint32_t seq=dongle_u32(p+12),gen=dongle_u32(p+8);
    uint16_t size=get16(p+16),offset=get16(p+18);
    uint8_t op=p[7],length=p[20];
    bool busy=c->phase==ASSEMBLING || c->phase==RUNNING;
    if (!header_valid(p) || !seq) { if(seq==c->sequence && busy)fail(client,MALFORMED); return; }
    if (gen!=c->target) return;
    if (op==5 && seq==c->sequence && !size && !offset && !length) { fail(client,CANCELLED); return; }
    if (op==4 && seq==c->sequence && c->phase==COMPLETE && !size && !length && offset<=c->total) {
        if(c->page!=offset){c->page=offset;c->dirty=true;}
        c->touched=now; return;
    }
    if (op==6 && seq==c->sequence && !size && !offset && !length) {
        if(c->phase!=ASSEMBLING)return;
        if(c->received!=c->total){fail(client,MALFORMED);return;}
        if(transport_owner>=0){fail(client,BUSY);return;}
        if(c->operation==1 || c->operation==2){
            if(lease_owner!=(int)client || !lease_current(now)){fail(client,lease_owner<0?STALE:BUSY);return;}
            lease_touched=now;
        }else if(c->operation==7 && lease_owner>=0 && lease_owner!=(int)client &&
            (c->payload[0]==1 || c->payload[0]==3 || c->payload[0]==4)){
            fail(client,BUSY);return;
        }
        if(c->operation!=3 && c->operation!=7 && (usb_mic_requested() || aacp_mic_active())){fail(client,BUSY);return;}
        c->phase=RUNNING; c->touched=now;c->dirty=true;
        if(c->operation==1){
            transport_owner=(int)client;
            if(!dongle_att_request(c->payload,c->total))fail(client,TRANSPORT);
        }else if(c->operation==2){
            transport_owner=(int)client;
            if(!aacp_research_send(c->payload,c->total))fail(client,TRANSPORT);
        }else if(c->operation==3 && c->total==1 && c->payload[0]>=1 && c->payload[0]<=6){
            c->total=aacp_research_cached(c->payload[0],c->payload,sizeof(c->payload));
            c->phase=COMPLETE; c->page=0;
        }else if(c->operation==7){
            uint16_t count=0;
            bool setting=c->payload[0]==1;
            if(setting)transport_owner=(int)client;
            uint8_t result=c->payload[0]==10 ? lease_request(client,c->payload,c->total,c->payload,&count,now) :
                dongle_features_request((uint8_t)client,c->payload,c->total,c->payload,sizeof(c->payload),&count);
            if(result)fail(client,result);
            else if(!setting){c->total=count;c->phase=COMPLETE;c->page=0;}
        }else fail(client,UNSUPPORTED);
        return;
    }
    if((op!=1 && op!=2 && op!=3 && op!=7) || !size || size>RPC_LIMIT || !length || offset>size || length>size-offset){
        if(seq==c->sequence && busy)fail(client,MALFORMED);
        return;
    }
    if(seq!=c->sequence){
        if(busy || offset || (c->sequence && (uint32_t)(seq-c->sequence)>=UINT32_C(0x80000000)))return;
        c->sequence=seq;c->operation=op;c->phase=ASSEMBLING;c->total=size;c->received=c->page=0;c->failure=0;
    }
    if(c->phase!=ASSEMBLING || c->operation!=op || c->total!=size)return;
    if(offset<c->received){
        if(offset+length>c->received || memcmp(c->payload+offset,p+22,length))fail(client,MALFORMED);
        return;
    }
    if(offset!=c->received){fail(client,MALFORMED);return;}
    memcpy(c->payload+offset,p+22,length);c->received+=length;c->touched=now;c->dirty=true;
}
void dongle_rpc_poll(void) {
    uint32_t now=btstack_run_loop_get_time_ms();
    if(lease_owner>=0 && !lease_current(now)){
        unsigned owner=(unsigned)lease_owner;
        bool stale=!clients[owner].online || clients[owner].seen_epoch!=clients[owner].epoch ||
            lease_target!=dongle_control_client_generation((uint8_t)owner);
        fail(owner,stale?STALE:TIMEOUT);
        release_lease();
    }
    for(unsigned client=0;client<DONGLE_CLIENT_COUNT;client++){
        rpc_client_t *c=&clients[client];
        uint32_t gen=dongle_control_client_generation((uint8_t)client);
        if(c->seen_epoch!=c->epoch || c->target!=gen){
            abort_operation(client);
            if(lease_owner==(int)client)release_lease();
            if(c->seen_epoch!=c->epoch)dongle_features_disconnect((uint8_t)client);
            c->seen_epoch=c->epoch;c->target=gen;c->sequence=0;
            c->phase=IDLE;c->total=c->received=c->page=0;c->failure=c->operation=0;
            c->dirty=true;
            memset(c->payload,0,sizeof(c->payload));
        }
        uint8_t request[64];uint32_t irq=save_and_disable_interrupts();
        bool take=c->queued && c->online;
        if(take)memcpy(request,c->incoming,64);
        c->queued=false;restore_interrupts(irq);
        if(take)handle(client,request,now);
    }
    dongle_att_poll();
    if(transport_owner>=0){
        unsigned owner=(unsigned)transport_owner;rpc_client_t *c=&clients[owner];uint16_t count=0;
        int result=c->operation==1?(int)dongle_att_take(c->payload,sizeof(c->payload),&count):
            c->operation==7?aacp_setting_take():aacp_research_take();
        if(result==1){c->total=count;c->phase=COMPLETE;c->page=0;c->dirty=true;transport_owner=-1;}
        else if(result==2)fail(owner,TRANSPORT);
    }
    for(unsigned client=0;client<DONGLE_CLIENT_COUNT;client++){
        rpc_client_t *c=&clients[client];
        if((c->phase==ASSEMBLING || c->phase==RUNNING) && (uint32_t)(now-c->touched)>=3500)fail(client,TIMEOUT);
        // RPC reports contain no live ages. Publish only visible state changes;
        // queue draining, transport progress and deadlines still run every poll.
        if(!c->dirty || !c->online)continue;
        uint8_t report[64]={3,'A','P','D','G',1,64};
        report[7]=c->operation;dongle_put32(report+8,c->target);dongle_put32(report+12,c->sequence);report[21]=c->phase;
        if(c->phase==ASSEMBLING){put16(report+16,c->total);put16(report+18,c->received);}
        if(c->phase==COMPLETE){
            uint16_t count=c->total-c->page;if(count>CHUNK)count=CHUNK;
            put16(report+16,c->total);put16(report+18,c->page);report[20]=(uint8_t)count;
            memcpy(report+22,c->payload+c->page,count);
        }
        if(c->phase==FAILED){put16(report+16,1);report[20]=1;report[22]=c->failure;}
        uint32_t irq=save_and_disable_interrupts();memcpy(c->published,report,64);restore_interrupts(irq);
        c->dirty=false;
    }
}
