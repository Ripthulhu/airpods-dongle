// SPDX-License-Identifier: GPL-3.0-only
#include "dongle_rpc.h"
#include "dongle_protocol.h"
#include "dongle_control.h"
#include "uac.h"
#include "../btstack/btstack_att.h"
#include "../btstack/btstack_aacp.h"
#include "hardware/sync.h"

#define RPC_LIMIT 512u
#define CHUNK 42u
enum { IDLE, ASSEMBLING, RUNNING, COMPLETE, FAILED };
enum { MALFORMED=1, BUSY, STALE, DISCONNECTED, TIMEOUT, TRANSPORT, UNSUPPORTED, CANCELLED };
static uint8_t incoming[64], published[64], payload[RPC_LIMIT];
static volatile bool queued, online;
static volatile uint32_t bus_epoch;
static uint32_t seen_epoch, target, sequence, touched;
static uint16_t total, received, page;
static uint8_t operation, phase, failure;

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void abort_operation(void) { dongle_att_cancel(); aacp_research_cancel(); }
static void fail(uint8_t why) {
    abort_operation(); phase=FAILED; failure=why; total=0; received=page=0;
}
bool dongle_rpc_busy(void) { return phase==ASSEMBLING || phase==RUNNING; }
void dongle_rpc_available(bool available) { online=available; queued=false; ++bus_epoch; }
uint16_t dongle_rpc_get(uint8_t *buffer, uint16_t capacity) {
    if (!buffer || capacity<63 || published[0]!=3) return 0;
    memcpy(buffer,published+1,63); return 63;
}
void dongle_rpc_set(const uint8_t *buffer, uint16_t length) {
    if (!online || queued || !buffer || length!=63) return;
    incoming[0]=3; memcpy(incoming+1,buffer,63); queued=true;
}
static bool header_valid(const uint8_t *p) {
    if (p[0]!=3 || memcmp(p+1,"APDG",4) || p[5]!=1 || p[6]!=64 || p[20]>CHUNK || p[21]) return false;
    for (unsigned i=22+p[20];i<64;i++) if (p[i]) return false;
    return true;
}
static void handle(const uint8_t *p, uint32_t now) {
    uint32_t seq=dongle_u32(p+12), gen=dongle_u32(p+8);
    uint16_t size=get16(p+16), offset=get16(p+18);
    uint8_t op=p[7], length=p[20];
    if (!header_valid(p) || !seq) { if (seq==sequence && dongle_rpc_busy()) fail(MALFORMED); return; }
    if (gen!=target) return;
    if (op==5 && seq==sequence && !size && !offset && !length) { fail(CANCELLED); return; }
    if (op==4 && seq==sequence && phase==COMPLETE && !size && !length && offset<=total) {
        page=offset; touched=now; return;
    }
    if (op==6 && seq==sequence && !size && !offset && !length) {
        if (phase!=ASSEMBLING) return; // Duplicate commits cannot repeat writes.
        if (received!=total) { fail(MALFORMED); return; }
        if (operation!=3 && (usb_mic_requested() || aacp_mic_active())) { fail(BUSY); return; }
        phase=RUNNING; touched=now;
        if (operation==1) { if (!dongle_att_request(payload,total)) fail(TRANSPORT); }
        else if (operation==2) { if (!aacp_research_send(payload,total)) fail(TRANSPORT); }
        else if (operation==3 && total==1 && payload[0]>=1 && payload[0]<=5) {
            uint8_t slot=payload[0];
            total=aacp_research_cached(slot,payload,sizeof(payload));
            phase=COMPLETE; page=0;
        } else fail(UNSUPPORTED);
        return;
    }
    if (op<1 || op>3 || !size || size>RPC_LIMIT || !length || offset>size || length>size-offset) {
        if (seq==sequence && dongle_rpc_busy()) fail(MALFORMED);
        return;
    }
    if (seq!=sequence) {
        if (dongle_rpc_busy() || offset || (sequence && (uint32_t)(seq-sequence)>=UINT32_C(0x80000000))) return;
        sequence=seq; operation=op; phase=ASSEMBLING; total=size; received=page=0; failure=0;
    }
    if (phase!=ASSEMBLING || operation!=op || total!=size) return;
    // Identical retransmitted fragments are harmless; overlapping changes are not.
    if (offset<received) {
        if (offset+length>received || memcmp(payload+offset,p+22,length)) fail(MALFORMED);
        return;
    }
    if (offset!=received) { fail(MALFORMED); return; }
    memcpy(payload+offset,p+22,length); received+=length; touched=now;
}
void dongle_rpc_poll(void) {
    uint32_t now=btstack_run_loop_get_time_ms(), gen=dongle_control_generation();
    if (seen_epoch!=bus_epoch || target!=gen) {
        abort_operation(); seen_epoch=bus_epoch; target=gen; sequence=0;
        phase=IDLE; total=received=page=0; failure=0; operation=0;
    }
    uint8_t request[64];
    uint32_t irq=save_and_disable_interrupts();
    bool take=queued && online;
    if (take) memcpy(request,incoming,sizeof(request));
    queued=false; restore_interrupts(irq);
    if (take) handle(request,now);
    dongle_att_poll();
    if (phase==RUNNING) {
        uint16_t count=0;
        int result=operation==1 ? (int)dongle_att_take(payload,sizeof(payload),&count) : aacp_research_take();
        if (result==1) { total=count; phase=COMPLETE; page=0; }
        else if (result==2) fail(TRANSPORT);
    }
    if (dongle_rpc_busy() && (uint32_t)(now-touched)>=3500) fail(TIMEOUT);
    uint8_t report[64]={3,'A','P','D','G',1,64};
    report[7]=operation; dongle_put32(report+8,target); dongle_put32(report+12,sequence);
    report[21]=phase;
    if (phase==ASSEMBLING) { put16(report+16,total); put16(report+18,received); }
    if (phase==COMPLETE) {
        uint16_t count=total-page; if(count>CHUNK)count=CHUNK;
        put16(report+16,total); put16(report+18,page); report[20]=(uint8_t)count;
        memcpy(report+22,payload+page,count);
    }
    if (phase==FAILED) { put16(report+16,1); report[20]=1; report[22]=failure; }
    irq=save_and_disable_interrupts(); memcpy(published,report,64); restore_interrupts(irq);
}
