// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/tinyusb/dongle_rpc.c"
static uint32_t clock_ms=1, connection=10;
static unsigned att_sent, aacp_sent, cancellations;
static unsigned critical_sections, att_polls;
static int att_result, aacp_result, setting_result;
static bool microphone;
uint32_t save_and_disable_interrupts(void) {++critical_sections;return 0;}
void restore_interrupts(uint32_t value) {(void)value;}
uint32_t btstack_run_loop_get_time_ms(void) {return clock_ms;}
uint32_t dongle_control_generation(void) {return connection;}
uint32_t dongle_control_client_generation(uint8_t client) {(void)client;return connection;}
void dongle_features_disconnect(uint8_t client) {(void)client;}
uint8_t dongle_features_request(uint8_t client,const uint8_t *p,uint16_t n,uint8_t *out,uint16_t cap,uint16_t *len) {(void)client;(void)p;(void)n;(void)out;(void)cap;*len=0;return 0;}
bool usb_mic_requested(void) {return microphone;}
bool aacp_mic_active(void) {return false;}
bool dongle_att_request(const uint8_t *p,uint16_t n) {assert(p && n);att_sent++;att_result=0;return true;}
void dongle_att_poll(void) {++att_polls;}
void dongle_att_cancel(void) {cancellations++;att_result=0;}
dongle_att_result_t dongle_att_take(uint8_t *out,uint16_t cap,uint16_t *n) {
    (void)cap;if(att_result==1){out[0]=0x0b;out[1]=7;*n=2;}return att_result;
}
bool aacp_research_send(const uint8_t *p,uint16_t n) {assert(p && n==140);aacp_sent++;return true;}
int aacp_research_take(void) {return aacp_result;}
void aacp_research_cancel(void) {aacp_result=0;}
int aacp_setting_take(void) {int value=setting_result;setting_result=0;return value;}
void aacp_setting_cancel(void) {setting_result=0;}
uint16_t aacp_research_cached(uint8_t slot,uint8_t *out,uint16_t cap) {
    assert(slot==4 && cap>=144);for(unsigned i=0;i<144;i++)out[i]=(uint8_t)i;return 144;
}
static void send_packet(uint8_t op,uint32_t seq,uint16_t size,uint16_t offset,const uint8_t *data,uint8_t n) {
    // These fragmentation tests enter a workflow directly; lease tests use full wire requests.
    if((op==1 || op==2) && lease_owner<0){
        uint8_t p[6]={10},out[5];uint16_t length=0;
        assert(!lease_request(0,p,sizeof(p),out,&length,clock_ms));
    }
    uint8_t b[64]={3,'A','P','D','G',1,64};b[7]=op;
    dongle_put32(b+8,connection);dongle_put32(b+12,seq);put16(b+16,size);put16(b+18,offset);b[20]=n;
    if(n) memcpy(b+22,data,n);
    dongle_rpc_set(b+1,63);dongle_rpc_poll();
}
static void send_phone(uint8_t op,uint32_t seq,uint16_t size,uint16_t offset,const uint8_t *data,uint8_t n) {
    if((op==1 || op==2) && lease_owner<0){
        uint8_t p[6]={10},out[5];uint16_t length=0;
        assert(!lease_request(1,p,sizeof(p),out,&length,clock_ms));
    }
    uint8_t b[64]={3,'A','P','D','G',1,64};b[7]=op;
    dongle_put32(b+8,connection);dongle_put32(b+12,seq);put16(b+16,size);put16(b+18,offset);b[20]=n;
    if(n)memcpy(b+22,data,n);
    assert(dongle_rpc_client_set(DONGLE_CLIENT_PHONE,b));dongle_rpc_poll();
}
static void quiet_polls(unsigned count) {
    unsigned before=critical_sections,polled=att_polls;
    uint8_t published[DONGLE_CLIENT_COUNT][64];
    for(unsigned i=0;i<DONGLE_CLIENT_COUNT;i++)memcpy(published[i],clients[i].published,64);
    for(unsigned i=0;i<count;i++){++clock_ms;dongle_rpc_poll();}
    // Queue draining and ATT progress remain live, without any report publication.
    assert(critical_sections==before+count*DONGLE_CLIENT_COUNT);
    assert(att_polls==polled+count);
    for(unsigned i=0;i<DONGLE_CLIENT_COUNT;i++)assert(!memcmp(published[i],clients[i].published,64));
}
int main(void) {
    uint8_t buffer[512];for(unsigned i=0;i<512;i++)buffer[i]=(uint8_t)i;
    dongle_rpc_available(true);dongle_rpc_poll();assert(clients[0].phase==IDLE);
    assert(clients[0].published[21]==IDLE && clients[1].published[0]==0);
    quiet_polls(2000);
    for(unsigned i=0;i<140;i+=42){unsigned n=140-i;if(n>42)n=42;send_packet(2,1,140,i,buffer+i,n);}
    assert(clients[0].received==140 && !aacp_sent);send_packet(6,1,0,0,NULL,0);assert(aacp_sent==1 && clients[0].phase==RUNNING);
    assert(clients[0].published[21]==RUNNING);
    quiet_polls(20);
    send_packet(6,1,0,0,NULL,0);assert(aacp_sent==1);
    aacp_result=1;dongle_rpc_poll();assert(clients[0].phase==COMPLETE && clients[0].total==0);
    assert(clients[0].published[21]==COMPLETE);
    quiet_polls(2000);
    uint8_t slot=4;send_packet(3,2,1,0,&slot,1);send_packet(6,2,0,0,NULL,0);assert(clients[0].total==144 && clients[0].phase==COMPLETE);
    for(unsigned i=0;i<144;i+=42){send_packet(4,2,0,i,NULL,0);assert(get16(clients[0].published+18)==i);assert(clients[0].published[22]==(uint8_t)i);}
    unsigned before=critical_sections;
    send_packet(4,2,0,126,NULL,0);assert(critical_sections==before+DONGLE_CLIENT_COUNT);
    send_packet(1,3,100,0,buffer,42);send_packet(1,3,100,43,buffer+43,42);assert(clients[0].phase==FAILED && !att_sent);
    send_packet(1,4,3,0,buffer,3);send_packet(1,4,3,0,buffer,3);assert(clients[0].received==3); // identical fragment
    send_packet(6,4,0,0,NULL,0);assert(att_sent==1);send_packet(5,4,0,0,NULL,0);assert(clients[0].phase==FAILED && clients[0].failure==CANCELLED);
    assert(clients[0].published[21]==FAILED && clients[0].published[22]==CANCELLED);
    quiet_polls(2000);
    send_packet(1,5,3,0,buffer,3);clock_ms+=3500;dongle_rpc_poll();assert(clients[0].phase==FAILED && clients[0].failure==TIMEOUT && att_sent==1);
    assert(clients[0].published[21]==FAILED && clients[0].published[22]==TIMEOUT);
    send_packet(1,6,3,0,buffer,3);connection++;dongle_rpc_poll();send_packet(6,6,0,0,NULL,0);assert(!clients[0].sequence && att_sent==1);
    assert(dongle_u32(clients[0].published+8)==connection && clients[0].published[21]==IDLE);
    send_packet(1,7,3,0,buffer,3);dongle_rpc_available(false);dongle_rpc_poll();assert(clients[0].phase==IDLE);
    dongle_rpc_available(true);dongle_rpc_poll();microphone=true;
    send_packet(1,8,3,0,buffer,3);send_packet(6,8,0,0,NULL,0);assert(clients[0].phase==FAILED && clients[0].failure==BUSY && att_sent==1);
    microphone=false;send_packet(1,9,3,0,buffer,3);send_packet(6,9,0,0,NULL,0);att_result=1;dongle_rpc_poll();
    assert(clients[0].phase==COMPLETE && clients[0].total==2 && clients[0].published[22]==0x0b);
    send_packet(4,9,0,2,NULL,0);assert(clients[0].phase==COMPLETE && clients[0].published[20]==0 && get16(clients[0].published+18)==2);
    send_packet(1,8,3,0,buffer,3);assert(clients[0].sequence==9 && clients[0].phase==COMPLETE); // old clients[0].sequence
    send_packet(1,10,513,0,buffer,42);assert(clients[0].sequence==9 && clients[0].phase==COMPLETE);
    send_packet(1,10,3,0,buffer,3);
    uint8_t malformed[64]={3,'A','P','D','G',1,64};
    malformed[7]=6;dongle_put32(malformed+8,connection);dongle_put32(malformed+12,10);malformed[63]=1;
    dongle_rpc_set(malformed+1,63);dongle_rpc_poll();assert(clients[0].phase==FAILED && clients[0].failure==MALFORMED && att_sent==2);
    clients[0].sequence=UINT32_MAX;send_packet(1,1,3,0,buffer,3);assert(clients[0].sequence==1 && clients[0].phase==ASSEMBLING);
    send_packet(5,1,0,0,NULL,0);
    assert(cancellations>0);
    dongle_rpc_client_available(DONGLE_CLIENT_PHONE,true);dongle_rpc_poll();
    uint8_t phone_report[64];assert(dongle_rpc_client_get(1,phone_report) && phone_report[21]==IDLE);
    quiet_polls(2000);
    send_packet(1,2,3,0,buffer,3);send_packet(6,2,0,0,NULL,0);
    assert(transport_owner==0 && clients[0].phase==RUNNING);
    unsigned cancelled_before=cancellations;
    send_phone(1,1,3,0,buffer+3,3);send_phone(6,1,0,0,NULL,0);
    assert(clients[1].phase==FAILED && clients[1].failure==BUSY && transport_owner==0);
    send_phone(5,1,0,0,NULL,0);assert(transport_owner==0 && cancellations==cancelled_before);
    dongle_rpc_available(false);assert(!dongle_rpc_client_get(0,phone_report));dongle_rpc_poll();
    assert(transport_owner==-1 && cancellations==cancelled_before+1 && clients[1].phase==FAILED);
    send_phone(1,2,3,0,buffer+3,3);send_phone(6,2,0,0,NULL,0);assert(transport_owner==1);
    dongle_rpc_available(true);dongle_rpc_poll();assert(transport_owner==1);
    att_result=1;dongle_rpc_poll();assert(clients[1].phase==COMPLETE && clients[1].payload[0]==0x0b);
    assert(clients[1].published[21]==COMPLETE && clients[1].published[22]==0x0b);
    slot=4;send_packet(3,1,1,0,&slot,1);send_packet(6,1,0,0,NULL,0);
    assert(clients[0].total==144 && clients[1].total==2 && clients[1].payload[0]==0x0b);
    send_phone(7,3,1,0,buffer,1);send_phone(6,3,0,0,NULL,0);assert(clients[1].phase==COMPLETE);
    dongle_rpc_client_available(1,false);dongle_rpc_poll();assert(!clients[1].payload[0] && !clients[1].sequence);
    quiet_polls(2000);
    clock_ms=UINT32_MAX-10u;
    send_packet(1,2,3,0,buffer,3);send_packet(6,2,0,0,NULL,0);
    quiet_polls(20);assert(clients[0].phase==RUNNING);
    clock_ms+=3500;dongle_rpc_poll();
    assert(clients[0].published[21]==FAILED && clients[0].published[22]==TIMEOUT);
    send_packet(1,3,3,0,buffer,3);send_packet(6,3,0,0,NULL,0);
    att_result=2;dongle_rpc_poll();
    assert(clients[0].published[21]==FAILED && clients[0].published[22]==TRANSPORT);
    puts("dongle RPC: dirty publication, live transport/deadlines, fragments, pages, replay, cancellation and generation fencing passed");
    return 0;
}
