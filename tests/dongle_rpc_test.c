// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/tinyusb/dongle_rpc.c"
static uint32_t clock_ms=1, connection=10;
static unsigned att_sent, aacp_sent, cancellations;
static int att_result, aacp_result;
static bool microphone;
uint32_t save_and_disable_interrupts(void) {return 0;}
void restore_interrupts(uint32_t value) {(void)value;}
uint32_t btstack_run_loop_get_time_ms(void) {return clock_ms;}
uint32_t dongle_control_generation(void) {return connection;}
bool usb_mic_requested(void) {return microphone;}
bool aacp_mic_active(void) {return false;}
bool dongle_att_request(const uint8_t *p,uint16_t n) {assert(p && n);att_sent++;return true;}
void dongle_att_poll(void) {}
void dongle_att_cancel(void) {cancellations++;att_result=0;}
dongle_att_result_t dongle_att_take(uint8_t *out,uint16_t cap,uint16_t *n) {
    (void)cap;if(att_result==1){out[0]=0x0b;out[1]=7;*n=2;}return att_result;
}
bool aacp_research_send(const uint8_t *p,uint16_t n) {assert(p && n==140);aacp_sent++;return true;}
int aacp_research_take(void) {return aacp_result;}
void aacp_research_cancel(void) {aacp_result=0;}
uint16_t aacp_research_cached(uint8_t slot,uint8_t *out,uint16_t cap) {
    assert(slot==4 && cap>=144);for(unsigned i=0;i<144;i++)out[i]=(uint8_t)i;return 144;
}
static void send_packet(uint8_t op,uint32_t seq,uint16_t size,uint16_t offset,const uint8_t *data,uint8_t n) {
    uint8_t b[64]={3,'A','P','D','G',1,64};b[7]=op;
    dongle_put32(b+8,connection);dongle_put32(b+12,seq);put16(b+16,size);put16(b+18,offset);b[20]=n;
    if(n) memcpy(b+22,data,n);
    dongle_rpc_set(b+1,63);dongle_rpc_poll();
}
int main(void) {
    uint8_t buffer[512];for(unsigned i=0;i<512;i++)buffer[i]=(uint8_t)i;
    dongle_rpc_available(true);dongle_rpc_poll();assert(phase==IDLE);
    for(unsigned i=0;i<140;i+=42){unsigned n=140-i;if(n>42)n=42;send_packet(2,1,140,i,buffer+i,n);}
    assert(received==140 && !aacp_sent);send_packet(6,1,0,0,NULL,0);assert(aacp_sent==1 && phase==RUNNING);
    send_packet(6,1,0,0,NULL,0);assert(aacp_sent==1);
    aacp_result=1;dongle_rpc_poll();assert(phase==COMPLETE && total==0);
    uint8_t slot=4;send_packet(3,2,1,0,&slot,1);send_packet(6,2,0,0,NULL,0);assert(total==144 && phase==COMPLETE);
    for(unsigned i=0;i<144;i+=42){send_packet(4,2,0,i,NULL,0);assert(get16(published+18)==i);assert(published[22]==(uint8_t)i);}
    send_packet(1,3,100,0,buffer,42);send_packet(1,3,100,43,buffer+43,42);assert(phase==FAILED && !att_sent);
    send_packet(1,4,3,0,buffer,3);send_packet(1,4,3,0,buffer,3);assert(received==3); // identical fragment
    send_packet(6,4,0,0,NULL,0);assert(att_sent==1);send_packet(5,4,0,0,NULL,0);assert(phase==FAILED && failure==CANCELLED);
    send_packet(1,5,3,0,buffer,3);clock_ms+=3500;dongle_rpc_poll();assert(phase==FAILED && failure==TIMEOUT && att_sent==1);
    send_packet(1,6,3,0,buffer,3);connection++;dongle_rpc_poll();send_packet(6,6,0,0,NULL,0);assert(!sequence && att_sent==1);
    send_packet(1,7,3,0,buffer,3);dongle_rpc_available(false);dongle_rpc_poll();assert(phase==IDLE);
    dongle_rpc_available(true);dongle_rpc_poll();microphone=true;
    send_packet(1,8,3,0,buffer,3);send_packet(6,8,0,0,NULL,0);assert(phase==FAILED && failure==BUSY && att_sent==1);
    microphone=false;send_packet(1,9,3,0,buffer,3);send_packet(6,9,0,0,NULL,0);att_result=1;dongle_rpc_poll();
    assert(phase==COMPLETE && total==2 && published[22]==0x0b);
    send_packet(4,9,0,2,NULL,0);assert(phase==COMPLETE && published[20]==0 && get16(published+18)==2);
    send_packet(1,8,3,0,buffer,3);assert(sequence==9 && phase==COMPLETE); // old sequence
    send_packet(1,10,513,0,buffer,42);assert(sequence==9 && phase==COMPLETE);
    send_packet(1,10,3,0,buffer,3);
    uint8_t malformed[64]={3,'A','P','D','G',1,64};
    malformed[7]=6;dongle_put32(malformed+8,connection);dongle_put32(malformed+12,10);malformed[63]=1;
    dongle_rpc_set(malformed+1,63);dongle_rpc_poll();assert(phase==FAILED && failure==MALFORMED && att_sent==2);
    sequence=UINT32_MAX;send_packet(1,1,3,0,buffer,3);assert(sequence==1 && phase==ASSEMBLING);
    send_packet(5,1,0,0,NULL,0);
    assert(cancellations>0);
    puts("dongle RPC: explicit commits, bounded fragments, immutable pages, replay, cancellation and generation fencing passed");
}
