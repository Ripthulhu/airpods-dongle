// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/phone_control.c"
static uint32_t now;
static bool secure,accept_write=true,send_fail;
static unsigned accepts,declines,closed,writes,security;
static bool control_online,rpc_online;
static uint8_t output[1024];static unsigned output_length;
uint32_t btstack_run_loop_get_time_ms(void){return now;}
bool bt_hci_phone_peer_connected(const uint8_t address[6]){return secure && address[0]==1;}
void rfcomm_init(void){}
void gap_set_security_level(unsigned level){security=level;}
uint8_t rfcomm_register_service(btstack_packet_handler_t h,uint8_t channel,uint16_t mtu){assert(h==packet_handler && channel==7 && mtu==128 && security==2);return 0;}
uint32_t sdp_create_service_record_handle(void){return 0x10005;}
void spp_create_custom_sdp_record(uint8_t *r,uint32_t handle,const uint8_t *uuid,int channel,const char *name){assert(r && handle==0x10005 && uuid[0]==0xc6 && uuid[15]==1 && channel==7 && name);}
unsigned de_get_len(const uint8_t *p){(void)p;return 150;}
uint8_t sdp_register_service(const uint8_t *p){assert(p);return 0;}
uint8_t rfcomm_accept_connection(uint16_t c){assert(c==20);++accepts;return 0;}
uint8_t rfcomm_decline_connection(uint16_t c){assert(c);++declines;return 0;}
uint8_t rfcomm_disconnect(uint16_t c){assert(c);++closed;return 0;}
uint8_t rfcomm_request_can_send_now_event(uint16_t c){assert(c==cid);return 0;}
uint8_t rfcomm_send(uint16_t c,uint8_t *p,uint16_t n){assert(c==cid && n<=frame_mtu);if(send_fail)return 1;assert(output_length+n<=sizeof(output));memcpy(output+output_length,p,n);output_length+=n;return 0;}
void dongle_control_client_available(uint8_t client,bool on){assert(client==1);control_online=on;}
void dongle_rpc_client_available(uint8_t client,bool on){assert(client==1);rpc_online=on;}
bool dongle_control_client_get(uint8_t client,uint8_t p[64]){assert(client==1);memset(p,0,64);p[0]=2;memcpy(p+1,"APDG",4);p[5]=1;p[6]=64;return true;}
bool dongle_rpc_client_get(uint8_t client,uint8_t p[64]){assert(client==1);memset(p,0,64);p[0]=3;memcpy(p+1,"APDG",4);p[5]=1;p[6]=64;return true;}
bool dongle_control_client_set(uint8_t client,const uint8_t p[64]){assert(client==1 && p[0]==2);if(accept_write)++writes;return accept_write;}
bool dongle_rpc_client_set(uint8_t client,const uint8_t p[64]){assert(client==1 && p[0]==3);if(accept_write)++writes;return accept_write;}
static void incoming(void){uint8_t p[13]={RFCOMM_EVENT_INCOMING_CONNECTION,11,6,5,4,3,2,1,7,20,0,1,0};packet_handler(4,0,p,sizeof(p));}
static void opened(void){uint8_t p[17]={RFCOMM_EVENT_CHANNEL_OPENED,15,0,6,5,4,3,2,1,1,0,7,20,0,20,0,1};packet_handler(4,0,p,sizeof(p));}
static void writable(void){uint8_t p[4]={RFCOMM_EVENT_CAN_SEND_NOW,2,20,0};packet_handler(4,0,p,4);}
static void connect(void){secure=true;incoming();opened();assert(connected && control_online && rpc_online);}
static void drain(void){for(unsigned i=0;i<20 && response_count;i++){phone_control_poll();writable();}assert(!response_count);}
int main(void){
    assert(phone_control_init());incoming();assert(declines==1 && !accepts && !cid);
    connect();assert(accepts==1);incoming();assert(declines==2);
    uint8_t read[8]={'A','P','D','G',1,1,2,0};
    packet_handler(7,cid,read,8);assert(request_count==1 && !response_count && !output_length);
    phone_control_poll();assert(response_count==1);drain();assert(output_length==72 && output[5]==0x81 && output[8]==2);
    uint8_t write[72]={'A','P','D','G',1,2,3,64,3,'A','P','D','G',1,64};
    packet_handler(7,cid,write,72);assert(!writes);phone_control_poll();assert(writes==1);drain();
    accept_write=false;packet_handler(7,cid,write,72);phone_control_poll();output_length=0;drain();assert(output_length==9 && output[5]==0xff && output[8]==2 && writes==1);
    accept_write=true;packet_handler(7,cid,read,8);phone_control_poll();send_fail=true;writable();assert(response_count==1 && responses[response_head].sent==0);
    send_fail=false;output_length=0;drain();assert(output_length==72);
    packet_handler(7,cid,read,2);now+=3500;phone_control_poll();assert(!connected && !control_online && !rpc_online && closed==1);
    connect();packet_handler(7,cid,read,8);phone_control_poll();now+=3500;phone_control_poll();assert(!cid && closed==2);
    connect();secure=false;phone_control_poll();assert(!cid && closed==3);
    connect();write[0]='X';packet_handler(7,cid,write,72);assert(!cid && closed==4 && !request_count && !response_count);
    puts("phone service: encrypted bonded peer, deferred commands, MTU splits, backpressure, timeouts and cleanup passed");
}
