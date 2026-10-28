// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "../src/tinyusb/media_control.h"
#define ERROR_CODE_SUCCESS 0
#define HCI_EVENT_PACKET 4
#define HCI_EVENT_AVRCP_META 0xc0
#define AVRCP_SUBEVENT_PLAY_STATUS_QUERY 3
#define AVRCP_SUBEVENT_OPERATION 4
#define AVRCP_MAX_COMMAND_PARAMETER_LENGTH 11
#define AVRCP_OPERATION_ID_PLAY 0x44
#define AVRCP_OPERATION_ID_PAUSE 0x46
#define AVRCP_OPERATION_ID_FORWARD 0x4b
#define AVRCP_OPERATION_ID_BACKWARD 0x4c
#define AVRCP_OPERATION_ID_STOP 0x45
#define UNUSED(x) (void)(x)
typedef uint8_t avrcp_operation_id_t;
static struct {uint16_t avrcp_cid;} media_tracker={7};
static struct {unsigned song_length_ms,song_position_ms,status;} play_info;
static bool is_usb_streaming=true,phone_controls,mic;
static unsigned usb_events,phone_events;
static enum usb_media_command last;
static bool phone_sink_handle_target(uint8_t type,uint16_t channel,uint8_t *p,uint16_t size) {(void)type;(void)channel;(void)p;(void)size;return false;}
static uint8_t hci_event_packet_get_type(const uint8_t *p) {return p[0];}
static uint16_t little_endian_read_16(const uint8_t *p,unsigned at) {return p[at]|(uint16_t)p[at+1]<<8;}
static uint8_t avrcp_subevent_operation_get_operation_id(const uint8_t *p) {return p[5];}
static uint8_t avrcp_subevent_operation_get_button_pressed(const uint8_t *p) {return p[6];}
static const char *avrcp_operation2str(uint8_t operation) {(void)operation;return "test";}
static uint8_t avrcp_target_play_status(uint16_t cid,unsigned length,unsigned position,unsigned status) {(void)cid;(void)length;(void)position;(void)status;return 0;}
static bool phone_sink_owns_controls(void) {return phone_controls;}
static bool usb_mic_requested(void) {return mic;}
static bool aacp_mic_requested(void) {return mic;}
static bool call_controls;
static unsigned call_events;
static bool phone_call_media_control(uint8_t operation) {(void)operation;if(call_controls)++call_events;return call_controls;}
static void phone_sink_media_control(uint8_t operation) {(void)operation;++phone_events;}
bool usb_media_enqueue(enum usb_media_command command) {last=command;++usb_events;return true;}
#include "playback_buttons.inc"
int main(void) {
    // The pinned emitter sets length to 17 while delivering eight bytes for zero operands.
    uint8_t event[9]={HCI_EVENT_AVRCP_META,17,AVRCP_SUBEVENT_OPERATION,7,0,AVRCP_OPERATION_ID_PAUSE,1,0,0};
    avrcp_target_packet_handler(HCI_EVENT_PACKET,0,event,8);
    assert(usb_events==1 && last==USB_MEDIA_PAUSE);
    event[6]=0; avrcp_target_packet_handler(HCI_EVENT_PACKET,0,event,8); assert(usb_events==1);
    event[6]=1; event[5]=AVRCP_OPERATION_ID_PLAY; event[1]=6;
    avrcp_target_packet_handler(HCI_EVENT_PACKET,0,event,8); assert(usb_events==2 && last==USB_MEDIA_PLAY);
    event[7]=1; avrcp_target_packet_handler(HCI_EVENT_PACKET,0,event,8); assert(usb_events==2);
    avrcp_target_packet_handler(HCI_EVENT_PACKET,0,event,7); assert(usb_events==2);
    event[7]=0;event[1]=5;avrcp_target_packet_handler(HCI_EVENT_PACKET,0,event,8);assert(usb_events==2);
    event[1]=17;event[3]=8;avrcp_target_packet_handler(HCI_EVENT_PACKET,0,event,8);assert(usb_events==2);
    event[3]=7;phone_controls=true;is_usb_streaming=false;
    avrcp_target_packet_handler(HCI_EVENT_PACKET,0,event,8);assert(phone_events==1 && usb_events==2);
    is_usb_streaming=true;
    event[5]=AVRCP_OPERATION_ID_FORWARD;avrcp_target_packet_handler(HCI_EVENT_PACKET,0,event,8);
    assert(usb_events==3 && last==USB_MEDIA_NEXT);
    is_usb_streaming=false;call_controls=true;event[5]=AVRCP_OPERATION_ID_PLAY;
    avrcp_target_packet_handler(HCI_EVENT_PACKET,0,event,8);assert(call_events==1 && phone_events==1);
    event[3]=8;avrcp_target_packet_handler(HCI_EVENT_PACKET,0,event,8);assert(call_events==1);
    event[3]=7;is_usb_streaming=true;
    avrcp_target_packet_handler(HCI_EVENT_PACKET,0,event,8);assert(call_events==1 && usb_events==4);
    puts("stem buttons: actual SDK callback length, press/release, stale CID and USB/phone/call ownership passed");
}
