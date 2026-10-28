// SPDX-License-Identifier: GPL-3.0-only
#include "btstack.h"
#include "phone_control.h"
#include "phone_control_protocol.h"
#include "btstack_hci.h"
#include "../tinyusb/dongle_control.h"
#include "../tinyusb/dongle_rpc.h"

#define SERVER_CHANNEL 7
#define RESPONSE_COUNT 4u
#define IO_TIMEOUT_MS 3500u
static uint8_t sdp_record[256];
static const uint8_t service_uuid[16]={0xc6,0xc4,0xf8,0xa0,0x6d,0x47,0x4b,0xd1,0x9b,0x6a,0x41,0x50,0x44,0x47,0,1};
static phone_control_parser_t parser;
static bd_addr_t peer;
static uint16_t cid, frame_mtu;
static bool connected, send_requested;
static uint32_t received_at, blocked_at, connecting_at;
typedef struct { uint8_t bytes[72], length, sent; } response_t;
static response_t responses[RESPONSE_COUNT];
static unsigned response_head,response_count;
static response_t requests[RESPONSE_COUNT];
static unsigned request_head,request_count;

static void reset_client(void) {
    dongle_control_client_available(DONGLE_CLIENT_PHONE,false);
    dongle_rpc_client_available(DONGLE_CLIENT_PHONE,false);
    memset(&parser,0,sizeof(parser));memset(responses,0,sizeof(responses));
    memset(requests,0,sizeof(requests));request_head=request_count=0;
    memset(peer,0,sizeof(peer));response_head=response_count=0;
    cid=frame_mtu=0;connected=send_requested=false;
}
static void close_client(void) {
    uint16_t old=cid;reset_client();
    if(old)rfcomm_disconnect(old);
}
static void schedule_send(void) {
    if(connected && response_count && !send_requested){
        send_requested=true;
        if(rfcomm_request_can_send_now_event(cid)!=ERROR_CODE_SUCCESS)send_requested=false;
    }
}
static bool queue_request(void *context,const uint8_t *frame) {
    (void)context;
    if(!connected || request_count==RESPONSE_COUNT)return false;
    response_t *r=&requests[(request_head+request_count)%RESPONSE_COUNT];
    r->length=(uint8_t)(8+frame[7]);memcpy(r->bytes,frame,r->length);++request_count;
    return true;
}
static bool request(void *context,const uint8_t *frame) {
    (void)context;
    if(!connected || !bt_hci_phone_peer_connected(peer) || response_count==RESPONSE_COUNT)return false;
    response_t *r=&responses[(response_head+response_count)%RESPONSE_COUNT];
    memset(r,0,sizeof(*r));memcpy(r->bytes,"APDG",4);r->bytes[4]=1;
    r->bytes[5]=(uint8_t)(frame[5]|0x80);r->bytes[6]=frame[6];r->bytes[7]=64;r->length=72;
    uint8_t error=0;
    if(frame[5]==2){
        bool accepted=frame[6]==2 ? dongle_control_client_set(DONGLE_CLIENT_PHONE,frame+8) :
            dongle_rpc_client_set(DONGLE_CLIENT_PHONE,frame+8);
        if(!accepted)error=2;
    }
    if(!error){
        bool available=frame[6]==2 ? dongle_control_client_get(DONGLE_CLIENT_PHONE,r->bytes+8) :
            dongle_rpc_client_get(DONGLE_CLIENT_PHONE,r->bytes+8);
        if(!available)error=3;
    }
    if(error){r->bytes[5]=0xff;r->bytes[7]=1;r->bytes[8]=error;r->length=9;}
    if(!response_count)blocked_at=btstack_run_loop_get_time_ms();
    ++response_count;schedule_send();return true;
}
static void packet_handler(uint8_t type,uint16_t channel,uint8_t *packet,uint16_t size) {
    if(!packet || !size)return;
    uint32_t now=btstack_run_loop_get_time_ms();
    if(type==RFCOMM_DATA_PACKET){
        if(channel!=cid || !connected)return;
        if(!bt_hci_phone_peer_connected(peer) || !phone_control_feed(&parser,packet,size,queue_request,NULL))close_client();
        else received_at=now;
        return;
    }
    if(type!=HCI_EVENT_PACKET || size<2 || (unsigned)packet[1]+2!=size)return;
    switch(packet[0]){
        case RFCOMM_EVENT_INCOMING_CONNECTION:{
            if(size!=13)return;
            bd_addr_t address;rfcomm_event_incoming_connection_get_bd_addr(packet,address);
            uint16_t incoming=rfcomm_event_incoming_connection_get_rfcomm_cid(packet);
            if(cid || rfcomm_event_incoming_connection_get_server_channel(packet)!=SERVER_CHANNEL ||
                !bt_hci_phone_peer_connected(address)){rfcomm_decline_connection(incoming);return;}
            cid=incoming;memcpy(peer,address,6);connecting_at=now;
            if(rfcomm_accept_connection(cid)!=ERROR_CODE_SUCCESS)close_client();
            break;
        }
        case RFCOMM_EVENT_CHANNEL_OPENED:{
            if(size!=17 || rfcomm_event_channel_opened_get_rfcomm_cid(packet)!=cid)return;
            bd_addr_t address;rfcomm_event_channel_opened_get_bd_addr(packet,address);
            if(rfcomm_event_channel_opened_get_status(packet) || memcmp(address,peer,6) ||
                !bt_hci_phone_peer_connected(peer)){close_client();return;}
            frame_mtu=rfcomm_event_channel_opened_get_max_frame_size(packet);
            if(!frame_mtu){close_client();return;}
            connected=true;
            dongle_control_client_available(DONGLE_CLIENT_PHONE,true);
            dongle_rpc_client_available(DONGLE_CLIENT_PHONE,true);
            break;
        }
        case RFCOMM_EVENT_CAN_SEND_NOW:{
            if(size!=4 || rfcomm_event_can_send_now_get_rfcomm_cid(packet)!=cid || !connected)return;
            send_requested=false;
            if(!bt_hci_phone_peer_connected(peer)){close_client();return;}
            if(response_count){
                response_t *r=&responses[response_head];
                uint16_t count=(uint16_t)(r->length-r->sent);if(count>frame_mtu)count=frame_mtu;
                if(rfcomm_send(cid,r->bytes+r->sent,count)==ERROR_CODE_SUCCESS){
                    r->sent=(uint8_t)(r->sent+count);blocked_at=now;
                    if(r->sent==r->length){memset(r,0,sizeof(*r));response_head=(response_head+1)%RESPONSE_COUNT;--response_count;}
                }
            }
            // Failed sends retry from the main loop, without recursive callbacks.
            break;
        }
        case RFCOMM_EVENT_CHANNEL_CLOSED:
            if(size==4 && rfcomm_event_channel_closed_get_rfcomm_cid(packet)==cid)reset_client();
            break;
        default:break;
    }
}
bool phone_control_init(void) {
    // RFCOMM inherits the required encrypted security level; the saved peer and
    // persistent link key are independently checked before every request/send.
    gap_set_security_level(LEVEL_2);
    if(rfcomm_register_service(packet_handler,SERVER_CHANNEL,128)!=ERROR_CODE_SUCCESS)return false;
    spp_create_custom_sdp_record(sdp_record,sdp_create_service_record_handle(),service_uuid,SERVER_CHANNEL,"AirPods Dongle Controls");
    if(de_get_len(sdp_record)>sizeof(sdp_record))return false;
    return sdp_register_service(sdp_record)==ERROR_CODE_SUCCESS;
}
void phone_control_poll(void) {
    uint32_t now=btstack_run_loop_get_time_ms();
    if(!cid)return;
    if(!bt_hci_phone_peer_connected(peer) || (!connected && (uint32_t)(now-connecting_at)>=IO_TIMEOUT_MS) ||
        (parser.used && (uint32_t)(now-received_at)>=IO_TIMEOUT_MS) ||
        (response_count && (uint32_t)(now-blocked_at)>=IO_TIMEOUT_MS)){close_client();return;}
    for(unsigned i=0;i<RESPONSE_COUNT && request_count && response_count<RESPONSE_COUNT;i++){
        response_t *r=&requests[request_head];
        if(!request(NULL,r->bytes)){close_client();return;}
        memset(r,0,sizeof(*r));request_head=(request_head+1)%RESPONSE_COUNT;--request_count;
    }
    schedule_send();
}
