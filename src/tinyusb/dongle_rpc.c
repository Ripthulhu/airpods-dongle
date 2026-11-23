// SPDX-License-Identifier: GPL-3.0-only
#include "dongle_rpc.h"
#include "dongle_protocol.h"
#include "dongle_control.h"
#include "dongle_features.h"
#include "uac.h"
#include "../btstack/btstack_att.h"
#include "../btstack/btstack_aacp.h"
#include "hardware/sync.h"
#include "btstack_run_loop.h"

#define RPC_LIMIT DONGLE_RPC_MAXIMUM_PAYLOAD_LENGTH
#define CHUNK DONGLE_RPC_MAXIMUM_FRAGMENT_LENGTH
#define LEASE_IDLE_MS 5000u
#define LEASE_MAX_MS 30000u
enum { IDLE=DONGLE_RPC_PHASE_IDLE, ASSEMBLING=DONGLE_RPC_PHASE_ASSEMBLING, RUNNING=DONGLE_RPC_PHASE_RUNNING,
       COMPLETE=DONGLE_RPC_PHASE_COMPLETE, FAILED=DONGLE_RPC_PHASE_FAILED };
enum { MALFORMED=DONGLE_RPC_ERROR_MALFORMED, BUSY=DONGLE_RPC_ERROR_BUSY, STALE=DONGLE_RPC_ERROR_STALE,
       DISCONNECTED=DONGLE_RPC_ERROR_DISCONNECTED, TIMEOUT=DONGLE_RPC_ERROR_TIMEOUT,
       TRANSPORT=DONGLE_RPC_ERROR_TRANSPORT, UNSUPPORTED=DONGLE_RPC_ERROR_UNSUPPORTED,
       CANCELLED=DONGLE_RPC_ERROR_CANCELLED };
typedef struct {
    uint8_t incoming[DONGLE_REPORT_SIZE], published[DONGLE_REPORT_SIZE], payload[RPC_LIMIT];
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
    if(c->operation==DONGLE_RPC_OPERATION_SEND_ATT_REQUEST || c->operation==DONGLE_RPC_OPERATION_SEND_AACP_PACKET || lease_owner>=0)
        return lease_owner==transport_owner && lease_current(btstack_run_loop_get_time_ms());
    return true;
}
static uint8_t lease_request(unsigned client,const uint8_t *p,uint16_t size,
                              uint8_t *out,uint16_t *length,uint32_t now) {
    if(size!=6 || p[1]>DONGLE_SETTINGS_LEASE_RELEASE)return MALFORMED;
    uint8_t action=p[1];uint32_t token=dongle_u32(p+2);
    if(action==DONGLE_SETTINGS_LEASE_ACQUIRE){
        if(token)return MALFORMED;
        if(lease_owner>=0)return BUSY;
        lease_owner=(int)client;lease_target=clients[client].target;
        if(!++lease_serial)++lease_serial;
        lease_token=lease_serial;lease_touched=lease_started=now;
        out[0]=DONGLE_SETTINGS_LEASE_RESPONSE_VERSION;dongle_put32(out+1,lease_token);*length=5;
    }else{
        if(lease_owner!=(int)client || !token || token!=lease_token || !lease_current(now))return STALE;
        if(action==DONGLE_SETTINGS_LEASE_RENEW)lease_touched=now;
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
        clients[client].seen_epoch!=clients[client].epoch ||
        clients[client].target!=dongle_control_client_generation(client) ||
        clients[client].published[DONGLE_HEADER_OFFSET_REPORT_ID]!=DONGLE_RPC_REPORT_ID) return false;
    memcpy(out,clients[client].published,DONGLE_REPORT_SIZE); return true;
}
uint16_t dongle_rpc_get(uint8_t *out,uint16_t capacity) {
    uint8_t report[DONGLE_REPORT_SIZE];
    if (!out || capacity<DONGLE_REPORT_SIZE-1 || !dongle_rpc_client_get(DONGLE_CLIENT_USB,report)) return 0;
    memcpy(out,report+1,DONGLE_REPORT_SIZE-1); return DONGLE_REPORT_SIZE-1;
}
bool dongle_rpc_client_set(uint8_t client,const uint8_t report[64]) {
    if (client>=DONGLE_CLIENT_COUNT || !report) return false;
    rpc_client_t *c=&clients[client];
    if (!c->online || c->queued) return false;
    memcpy(c->incoming,report,DONGLE_REPORT_SIZE); c->queued=true; return true;
}
void dongle_rpc_set(const uint8_t *buffer,uint16_t length) {
    if (!buffer || length!=DONGLE_REPORT_SIZE-1) return;
    uint8_t report[DONGLE_REPORT_SIZE]={DONGLE_RPC_REPORT_ID}; memcpy(report+1,buffer,DONGLE_REPORT_SIZE-1);
    (void)dongle_rpc_client_set(DONGLE_CLIENT_USB,report);
}
static void handle(unsigned client,const uint8_t *p,uint32_t now) {
    rpc_client_t *c=&clients[client];
    uint32_t seq=dongle_u32(p+DONGLE_RPC_OFFSET_TRANSACTION_SEQUENCE),gen=dongle_u32(p+DONGLE_RPC_OFFSET_CONNECTION_GENERATION);
    uint16_t size=get16(p+DONGLE_RPC_OFFSET_TOTAL_LENGTH),offset=get16(p+DONGLE_RPC_OFFSET_FRAGMENT_OFFSET);
    uint8_t op=p[DONGLE_RPC_OFFSET_OPERATION],length=p[DONGLE_RPC_OFFSET_FRAGMENT_LENGTH];
    const uint8_t *payload=p+DONGLE_RPC_OFFSET_PAYLOAD;
    bool busy=c->phase==ASSEMBLING || c->phase==RUNNING;
    if (!dongle_rpc_header_valid(p) || !seq) { if(seq==c->sequence && busy)fail(client,MALFORMED); return; }
    if (gen!=c->target) return;
    if (op==DONGLE_RPC_OPERATION_CANCEL_TRANSACTION && seq==c->sequence && !size && !offset && !length) { fail(client,CANCELLED); return; }
    if (op==DONGLE_RPC_OPERATION_SELECT_RESPONSE_PAGE && seq==c->sequence && c->phase==COMPLETE && !size && !length && offset<=c->total) {
        if(c->page!=offset){c->page=offset;c->dirty=true;}
        c->touched=now; return;
    }
    if (op==DONGLE_RPC_OPERATION_COMMIT_TRANSACTION && seq==c->sequence && !size && !offset && !length) {
        if(c->phase!=ASSEMBLING)return;
        if(c->received!=c->total){fail(client,MALFORMED);return;}
        if(transport_owner>=0){fail(client,BUSY);return;}
        if(c->operation==DONGLE_RPC_OPERATION_SEND_ATT_REQUEST || c->operation==DONGLE_RPC_OPERATION_SEND_AACP_PACKET){
            if(lease_owner!=(int)client || !lease_current(now)){fail(client,lease_owner<0?STALE:BUSY);return;}
            lease_touched=now;
        }else if(c->operation==DONGLE_RPC_OPERATION_TYPED_FEATURE_REQUEST && lease_owner>=0 && lease_owner!=(int)client &&
            (c->payload[0]==DONGLE_FEATURE_WRITE_SETTING || c->payload[0]==DONGLE_FEATURE_CONFIGURE_AUDIO_PROFILE ||
             c->payload[0]==DONGLE_FEATURE_SET_GAME_MODE)){
            fail(client,BUSY);return;
        }
        if(c->operation==DONGLE_RPC_OPERATION_TYPED_FEATURE_REQUEST && lease_owner>=0 &&
            (c->payload[0]==DONGLE_FEATURE_REQUEST_ROUTING_HANDOVER ||
             (c->payload[0]==DONGLE_FEATURE_RECONNECT_POLICY && c->total>1))){fail(client,BUSY);return;}
        if(c->operation!=DONGLE_RPC_OPERATION_READ_CACHED_REPORT && c->operation!=DONGLE_RPC_OPERATION_TYPED_FEATURE_REQUEST &&
            (usb_mic_requested() || aacp_mic_active())){fail(client,BUSY);return;}
        c->phase=RUNNING; c->touched=now;c->dirty=true;
        if(c->operation==DONGLE_RPC_OPERATION_SEND_ATT_REQUEST){
            transport_owner=(int)client;
            if(!dongle_att_request(c->payload,c->total))fail(client,TRANSPORT);
        }else if(c->operation==DONGLE_RPC_OPERATION_SEND_AACP_PACKET){
            transport_owner=(int)client;
            if(!aacp_research_send(c->payload,c->total))fail(client,TRANSPORT);
        }else if(c->operation==DONGLE_RPC_OPERATION_READ_CACHED_REPORT && c->total==1 &&
            c->payload[0]>=DONGLE_CACHED_REPORT_MODEL_METADATA && c->payload[0]<=DONGLE_CACHED_REPORT_EQUALIZER){
            c->total=aacp_research_cached(c->payload[0],c->payload,sizeof(c->payload));
            c->phase=COMPLETE; c->page=0;
        }else if(c->operation==DONGLE_RPC_OPERATION_TYPED_FEATURE_REQUEST){
            uint16_t count=0;
            bool setting=c->payload[0]==DONGLE_FEATURE_WRITE_SETTING;
            if(setting)transport_owner=(int)client;
            uint8_t result=c->payload[0]==DONGLE_FEATURE_SETTINGS_WORKFLOW_LEASE ? lease_request(client,c->payload,c->total,c->payload,&count,now) :
                dongle_features_request((uint8_t)client,c->payload,c->total,c->payload,sizeof(c->payload),&count);
            if(result)fail(client,result);
            else if(!setting){c->total=count;c->phase=COMPLETE;c->page=0;}
        }else fail(client,UNSUPPORTED);
        return;
    }
    if((op!=DONGLE_RPC_OPERATION_SEND_ATT_REQUEST && op!=DONGLE_RPC_OPERATION_SEND_AACP_PACKET && op!=DONGLE_RPC_OPERATION_READ_CACHED_REPORT && op!=DONGLE_RPC_OPERATION_TYPED_FEATURE_REQUEST) || !size || size>RPC_LIMIT || !length || offset>size || length>size-offset){
        if(seq==c->sequence && busy)fail(client,MALFORMED);
        return;
    }
    if(seq!=c->sequence){
        if(busy || offset || (c->sequence && (uint32_t)(seq-c->sequence)>=UINT32_C(0x80000000)))return;
        c->sequence=seq;c->operation=op;c->phase=ASSEMBLING;c->total=size;c->received=c->page=0;c->failure=0;
    }
    if(c->phase!=ASSEMBLING || c->operation!=op || c->total!=size)return;
    if(offset<c->received){
        if(offset+length>c->received || memcmp(c->payload+offset,payload,length))fail(client,MALFORMED);
        return;
    }
    if(offset!=c->received){fail(client,MALFORMED);return;}
    memcpy(c->payload+offset,payload,length);c->received+=length;c->touched=now;c->dirty=true;
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
            // USB can read status while the main loop prepares the new generation.
            c->published[DONGLE_HEADER_OFFSET_REPORT_ID]=0;
            abort_operation(client);
            if(lease_owner==(int)client)release_lease();
            if(c->seen_epoch!=c->epoch)dongle_features_disconnect((uint8_t)client);
            c->seen_epoch=c->epoch;c->target=gen;c->sequence=0;
            c->phase=IDLE;c->total=c->received=c->page=0;c->failure=c->operation=0;
            c->dirty=true;
            memset(c->payload,0,sizeof(c->payload));
        }
        uint8_t request[DONGLE_REPORT_SIZE];uint32_t irq=save_and_disable_interrupts();
        bool take=c->queued && c->online;
        if(take)memcpy(request,c->incoming,DONGLE_REPORT_SIZE);
        c->queued=false;restore_interrupts(irq);
        if(take)handle(client,request,now);
    }
    dongle_att_poll();
    if(transport_owner>=0){
        unsigned owner=(unsigned)transport_owner;rpc_client_t *c=&clients[owner];uint16_t count=0;
        int result=c->operation==DONGLE_RPC_OPERATION_SEND_ATT_REQUEST?(int)dongle_att_take(c->payload,sizeof(c->payload),&count):
            c->operation==DONGLE_RPC_OPERATION_TYPED_FEATURE_REQUEST?aacp_setting_take():aacp_research_take();
        if(result==1){c->total=count;c->phase=COMPLETE;c->page=0;c->dirty=true;transport_owner=-1;}
        else if(result==2)fail(owner,TRANSPORT);
    }
    for(unsigned client=0;client<DONGLE_CLIENT_COUNT;client++){
        rpc_client_t *c=&clients[client];
        if((c->phase==ASSEMBLING || c->phase==RUNNING) && (uint32_t)(now-c->touched)>=3500)fail(client,TIMEOUT);
        // RPC reports contain no live ages. Publish only visible state changes;
        // queue draining, transport progress and deadlines still run every poll.
        if(!c->dirty || !c->online)continue;
        uint8_t report[DONGLE_REPORT_SIZE];
        dongle_header(report);report[DONGLE_HEADER_OFFSET_REPORT_ID]=DONGLE_RPC_REPORT_ID;
        report[DONGLE_RPC_OFFSET_OPERATION]=c->operation;
        dongle_put32(report+DONGLE_RPC_OFFSET_CONNECTION_GENERATION,c->target);
        dongle_put32(report+DONGLE_RPC_OFFSET_TRANSACTION_SEQUENCE,c->sequence);
        report[DONGLE_RPC_OFFSET_RESPONSE_PHASE]=c->phase;
        if(c->phase==ASSEMBLING){put16(report+DONGLE_RPC_OFFSET_TOTAL_LENGTH,c->total);put16(report+DONGLE_RPC_OFFSET_FRAGMENT_OFFSET,c->received);}
        if(c->phase==COMPLETE){
            uint16_t count=c->total-c->page;if(count>CHUNK)count=CHUNK;
            put16(report+DONGLE_RPC_OFFSET_TOTAL_LENGTH,c->total);put16(report+DONGLE_RPC_OFFSET_FRAGMENT_OFFSET,c->page);
            report[DONGLE_RPC_OFFSET_FRAGMENT_LENGTH]=(uint8_t)count;
            memcpy(report+DONGLE_RPC_OFFSET_PAYLOAD,c->payload+c->page,count);
        }
        if(c->phase==FAILED){
            put16(report+DONGLE_RPC_OFFSET_TOTAL_LENGTH,1);report[DONGLE_RPC_OFFSET_FRAGMENT_LENGTH]=1;
            report[DONGLE_RPC_OFFSET_PAYLOAD]=c->failure;
        }
        uint32_t irq=save_and_disable_interrupts();memcpy(c->published,report,DONGLE_REPORT_SIZE);restore_interrupts(irq);
        c->dirty=false;
    }
}
