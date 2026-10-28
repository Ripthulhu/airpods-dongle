// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "../src/btstack/volume_sync.h"
#include "../src/btstack/volume_scale.h"
#include "../src/tinyusb/media_control.h"
enum {AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED=1,AVRCP_SUBEVENT_SET_ABSOLUTE_VOLUME_RESPONSE=2,
 AVRCP_CTYPE_RESPONSE_INTERIM=15,AVRCP_CTYPE_RESPONSE_CHANGED_STABLE=13,AVRCP_CTYPE_NOTIFY=3,
 AVRCP_CTYPE_RESPONSE_ACCEPTED=9,AVRCP_CTYPE_RESPONSE_REJECTED=10,AVRCP_CTYPE_RESPONSE_NOT_IMPLEMENTED=8,
 AVRCP_CONTROLLER=0,ERROR_CODE_SUCCESS=0};
typedef struct {uint8_t transaction_id,controller_last_confirmed_transaction_id;} avrcp_connection_t;
static avrcp_connection_t connection;
static volume_sync_t sink_volume;
static bool _bt_sink_volume_changed,is_usb_streaming,mic,phone_owner,hid_ready,drop_ack;
static bool transfer_pending,drop_transfer,ack_reject,ack_applied,bt_busy;
static int ack_clamp;
static uint8_t transfer_report[3];
static uint32_t now,revision,set_generation,receipt_snapshot;
static uint32_t volume_operations_up,volume_operations_down,volume_target_requests;
static uint32_t volume_changed_reports,volume_interim_reports,volume_ack_reports;
static uint8_t peer[6]={1,2,3,4,5,6},physical;
static unsigned absolute_sets,send_attempts,keys_up,keys_down,ack_delay,host_delay,host_mode;
static int host_index;
static struct {uint16_t avrcp_cid;} media_tracker;
static struct {bool pending;uint32_t due;uint8_t label,level;} ack;
static struct {bool pending;uint32_t due;uint8_t level;} host_update;
enum {SOFTWARE_HOST,HARDWARE_HOST,IGNORING_HOST};
void dongle_control_available(bool v){(void)v;}
void dongle_rpc_available(bool v){(void)v;}
uint16_t dongle_control_get(uint8_t *p,uint16_t n){(void)p;(void)n;return 0;}
uint16_t dongle_rpc_get(uint8_t *p,uint16_t n){(void)p;(void)n;return 0;}
void dongle_control_set(const uint8_t *p,uint16_t n){(void)p;(void)n;}
void dongle_rpc_set(const uint8_t *p,uint16_t n){(void)p;(void)n;}
uint32_t time_us_32(void){return now*1000u;}
void tud_hid_report_complete_cb(uint8_t,const uint8_t *,uint16_t);
bool tud_hid_ready(void){return hid_ready && !transfer_pending;}
bool tud_hid_report(uint8_t id,const void *data,uint16_t length){
 assert(id==1 && length==2 && hid_ready && !transfer_pending);
 const uint8_t *p=data;transfer_report[0]=id;transfer_report[1]=p[0];transfer_report[2]=p[1];transfer_pending=true;
 return true;
}
static void complete_report(void){
 if(!transfer_pending || drop_transfer)return;
 uint8_t report[3];memcpy(report,transfer_report,3);transfer_pending=false;
 tud_hid_report_complete_cb(0,report,3);
 const uint8_t *p=report+1;
 unsigned usage=p[0]|(unsigned)p[1]<<8;
 if(usage!=0xe9 && usage!=0xea)return;
 int step=usage==0xe9?1:-1;if(step>0)++keys_up;else ++keys_down;
 if(host_mode!=IGNORING_HOST){host_index+=step*(host_mode==HARDWARE_HOST?8:1);
  if(host_index<0)host_index=0;
  int maximum=host_mode==HARDWARE_HOST?127:25;if(host_index>maximum)host_index=maximum;}
 if(host_mode==HARDWARE_HOST){host_update.pending=true;host_update.due=now+host_delay;host_update.level=(uint8_t)host_index;}
 // The model applies OS effects only on completed USB transfer, never submission.
}
static const uint8_t *get_device_addr(void){return peer;}
static bool usb_host_active(void){return (usb_media_generation()&1u)!=0;}
static bool usb_mic_requested(void){return mic;}
static bool aacp_mic_requested(void){return mic;}
static bool phone_sink_owns_controls(void){return phone_owner;}
static void phone_sink_volume_invalidated(void){}
static void phone_sink_volume_changed(uint8_t n){(void)n;}
static uint32_t btstack_run_loop_get_time_ms(void){return now;}
static uint32_t usb_volume_set_revision(void){return revision;}
static uint32_t usb_volume_set_generation(void){return set_generation;}
static uint32_t usb_volume_set_receipt(void){return receipt_snapshot;}
static uint16_t little_endian_read_16(const uint8_t *p,unsigned i){return (uint16_t)(p[i]|(p[i+1]<<8));}
static uint8_t avrcp_controller_set_absolute_volume(uint16_t cid,uint8_t level){
 assert(cid==sink_volume.cid && level<=127);++send_attempts;if(bt_busy)return 0x0c;++absolute_sets;
 connection.transaction_id=(uint8_t)((connection.transaction_id+1u)&15u);
 ack_applied=false;
 ack.pending=true;ack.due=now+ack_delay;ack.level=level;ack.label=connection.transaction_id;return 0;
}
static avrcp_connection_t *avrcp_get_connection_for_avrcp_cid_for_role(unsigned role,uint16_t cid){
 assert(role==AVRCP_CONTROLLER);return cid==sink_volume.cid?&connection:NULL;
}
// Actual source handlers/state and separately compiled production HID queue.
#include "playback_volume.inc"
static void event(uint8_t subtype,uint8_t ctype,uint8_t value){
 uint8_t wire[]={0xc0,5,subtype,7,0,ctype,value};assert(bt_volume_controller_event(wire,7));
}
static void changed(uint8_t value){physical=value;event(1,AVRCP_CTYPE_RESPONSE_CHANGED_STABLE,value);}
static void host_set(uint8_t value){
 ++revision;set_generation=usb_media_generation();receipt_snapshot=usb_media_volume_sent();
 set_bt_volume_from_usb(bt_volume_to_usb(value),revision,set_generation,receipt_snapshot);
}
static void advance(unsigned milliseconds){
 for(unsigned i=0;i<milliseconds;++i){++now;
  complete_report();
  if(host_update.pending && (int32_t)(now-host_update.due)>=0){
   uint8_t level=host_update.level;host_update.pending=false;host_set(level);}
  if(ack.pending && !drop_ack && (int32_t)(now-ack.due)>=0){
   uint8_t level=ack_clamp<0?ack.level:(uint8_t)ack_clamp,label=ack.label;ack.pending=false;
   if(!ack_reject && !ack_applied)physical=level;
   connection.controller_last_confirmed_transaction_id=label;
   event(2,ack_reject?AVRCP_CTYPE_RESPONSE_REJECTED:AVRCP_CTYPE_RESPONSE_ACCEPTED,level);
   if(!ack_applied)changed(physical);}
  usb_media_task();bt_volume_poll();
 }
}
static void fixture(uint8_t level,unsigned mode,bool baseline){
 transfer_pending=drop_transfer=false;
 usb_media_set_available(false);usb_media_set_available(true);hid_ready=true;usb_media_task();complete_report();
 memset(&sink_volume,0,sizeof(sink_volume));memset(&usb_volume_relay,0,sizeof(usb_volume_relay));
 connection=(avrcp_connection_t){0};now=revision=set_generation=receipt_snapshot=0;
 absolute_sets=send_attempts=keys_up=keys_down=volume_target_requests=0;ack.pending=host_update.pending=false;
 ack_delay=host_delay=1;mic=phone_owner=drop_ack=ack_reject=ack_applied=bt_busy=false;ack_clamp=-1;
 is_usb_streaming=true;peer[0]=1;_bt_sink_volume_changed=false;
 host_mode=mode;host_index=mode==HARDWARE_HOST?level:15;physical=level;
 bt_volume_select_peer();volume_sync_connection(&sink_volume,7,now);media_tracker.avrcp_cid=7;bt_volume_relay_reset(true);
 if(baseline)event(1,AVRCP_CTYPE_RESPONSE_INTERIM,level);
 bt_volume_poll();advance(5);
}
static void software_host_one_gain(void){
 fixture(47,SOFTWARE_HOST,true);assert(!revision && !absolute_sets);
 changed(37);advance(200);assert(keys_down==1 && host_index==14 && physical==47 && !revision);
 assert(usb_volume_relay.anchor==47 && !usb_volume_relay.waiting && !_bt_sink_volume_changed);
 advance(6000);changed(57);advance(200);assert(keys_up==1 && host_index==15 && physical==47);
 host_index=8;changed(37);advance(200);assert(host_index==7 && physical==47);
 changed(57);advance(200);assert(host_index==8 && physical==47);
 changed(37);changed(27);changed(37);advance(250);
 assert(host_index==7 && physical==47 && keys_down==4 && keys_up==3);
}
static void hardware_host_and_late_sets(void){
 const unsigned delays[]={1,50,500,3000};
 for(unsigned i=0;i<sizeof(delays)/sizeof(delays[0]);++i){
  fixture(64,HARDWARE_HOST,true);host_delay=delays[i];changed(54);advance(delays[i]+250);
  uint8_t readback=0;assert(host_index==56 && physical==56 && usb_volume_relay.anchor==56);
  assert(revision==1 && get_bt_volume(&readback) && readback==56 && bt_volume_usb_ready());
  assert(_bt_sink_volume_changed); // Confirmed native UAC readback stays available.
  changed(66);advance(delays[i]+250);assert(host_index==64 && physical==64 && keys_up==1 && keys_down==1);
 }
 fixture(47,SOFTWARE_HOST,true);ack_delay=50;changed(37);advance(153);
 assert(usb_volume_relay.restoring && sink_volume.inflight);host_set(20);advance(200);
 assert(physical==20 && usb_volume_relay.anchor==20 && !sink_volume.pending);advance(1000);assert(physical==20);
}
static void endpoints_and_unknown(void){
 fixture(127,SOFTWARE_HOST,true);assert(physical==126 && usb_volume_relay.anchor==126 && absolute_sets==1 && !keys_up && !keys_down);
 changed(127);advance(200);assert(keys_up==1 && physical==126 && host_index==16);
 changed(116);advance(200);assert(keys_down==1 && physical==126 && host_index==15);
 fixture(0,SOFTWARE_HOST,true);advance(1000);assert(physical==0 && !absolute_sets);
 changed(10);advance(200);assert(physical==10 && usb_volume_relay.anchor==10 && keys_up==1 && !absolute_sets);
 fixture(0,SOFTWARE_HOST,false);advance(1000);assert(!absolute_sets && !usb_volume_relay.enabled);
 changed(10);advance(200);assert(!keys_up && !keys_down && !absolute_sets); // First report is a baseline.
}
static void target_request_is_not_physical(void){
 fixture(47,SOFTWARE_HOST,true);volume_sync_t before=sink_volume;
 // Exact pinned SDK avrcp_target_emit_volume_changed(cid=7,level=0) wire layout.
 uint8_t wire[]={0xc0,5,AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED,7,0,AVRCP_CTYPE_NOTIFY,0};
 assert(bt_volume_target_event(wire,7));assert(volume_target_requests==1 && usb_volume_relay.target_known);
 wire[6]=127;assert(bt_volume_target_event(wire,7));assert(bt_volume_target_event(wire,7));
 assert(volume_target_requests==3 && usb_volume_relay.target_level==127);
 assert(!memcmp(&before,&sink_volume,sizeof(before)) && physical==47 && !keys_up && !keys_down && !absolute_sets);
 wire[5]=13;assert(!bt_volume_target_event(wire,7));wire[5]=3;wire[6]=128;assert(!bt_volume_target_event(wire,7));
 wire[6]=10;wire[3]=8;assert(!bt_volume_target_event(wire,7));wire[3]=7;assert(!bt_volume_target_event(wire,6));
 assert(volume_target_requests==3);
}
static void queue_failure_and_lifecycle(void){
 fixture(47,SOFTWARE_HOST,true);hid_ready=false;changed(37);advance(5100);
 assert(!keys_down && !absolute_sets && physical==37 && !usb_volume_relay.waiting);
 hid_ready=true;changed(47);advance(200);assert(keys_up==1 && physical==37);
 fixture(100,SOFTWARE_HOST,true);hid_ready=false;
 for(unsigned level=90;level>=10;level-=10)changed((uint8_t)level);
 hid_ready=true;advance(500);assert(!keys_down && physical==10 && !absolute_sets);
 changed(20);advance(200);assert(keys_up==1 && physical==10);
 fixture(47,SOFTWARE_HOST,true);hid_ready=false;changed(37);
 usb_media_set_available(false);bt_volume_poll();usb_media_set_available(true);bt_volume_poll();
 hid_ready=true;advance(500);assert(!keys_down && !absolute_sets && physical==37);
 changed(27);is_usb_streaming=false;bt_volume_poll();advance(200);assert(physical==27 && !keys_down && !absolute_sets);
 is_usb_streaming=true;bt_volume_poll();changed(37);peer[0]=9;bt_volume_select_peer();advance(200);
 assert(!keys_up && !sink_volume.cid && physical==37);
}
static void echoes_stale_set_and_receipts(void){
 fixture(47,SOFTWARE_HOST,true);ack_delay=50;changed(37);advance(153);assert(usb_volume_relay.restoring && sink_volume.inflight);
 changed(46);advance(100);assert(keys_down==1 && !keys_up && physical==47);
 ++revision;uint32_t old_revision=revision,generation=usb_media_generation();++revision;set_generation=generation;
 set_bt_volume_from_usb(-5000,old_revision,generation,usb_media_volume_sent());assert(!sink_volume.pending);
 set_bt_volume_from_usb(-5000,revision,generation-2,usb_media_volume_sent());assert(!sink_volume.pending);
 fixture(47,SOFTWARE_HOST,true);changed(57);changed(67);advance(2);
 ++revision;uint32_t receipt=usb_media_volume_sent();advance(2);assert(usb_media_volume_sent()!=receipt);
 set_bt_volume_from_usb(bt_volume_to_usb(60),revision,usb_media_generation(),receipt);
 assert(usb_volume_relay.waiting && usb_volume_relay.delivered);advance(250);assert(physical==60);
}
static void partial_delivery_and_inflight_gesture(void){
 fixture(47,SOFTWARE_HOST,true);is_usb_streaming=false;bt_volume_poll();is_usb_streaming=true;
 changed(37);advance(250);assert(keys_down==1 && physical==47); // Gesture before the first owned poll.
 fixture(47,SOFTWARE_HOST,true);changed(37);advance(2);assert(keys_down==1);
 hid_ready=false;changed(27);changed(17);advance(500);
 assert(!absolute_sets && physical==17 && !usb_volume_relay.delivered);
 hid_ready=true;advance(2);hid_ready=false;advance(500);
 assert(keys_down==2 && !absolute_sets && physical==17 && !usb_volume_relay.delivered);
 hid_ready=true;advance(250);assert(keys_down==3 && physical==47 && host_index==12);
 fixture(47,SOFTWARE_HOST,true);ack_delay=50;changed(37);advance(153);
 assert(usb_volume_relay.restoring && sink_volume.inflight);
 ack_applied=true;changed(47);changed(37); // Repair observed, then an unambiguous NEW user DOWN before its ACK.
 advance(300);assert(keys_down==2 && host_index==13 && physical==47 && sink_volume.confirmed==47);
 fixture(47,SOFTWARE_HOST,true);drop_transfer=true;changed(37);advance(300);
 assert(transfer_pending && !keys_down && !absolute_sets && physical==37);
 advance(5000);assert(!absolute_sets && physical==37 && !usb_volume_relay.waiting);
 fixture(47,SOFTWARE_HOST,true);bt_busy=true;changed(37);advance(153);
 assert(usb_volume_relay.restoring && sink_volume.pending && !sink_volume.inflight);
 hid_ready=false;changed(27);unsigned attempts=send_attempts;advance(500);
 assert(send_attempts==attempts && !absolute_sets && physical==27 && !usb_volume_relay.delivered);
 bt_busy=false;hid_ready=true;advance(250);assert(keys_down==2 && physical==47 && host_index==13);
}
static void headroom_no_progress_is_bounded(void){
 fixture(127,SOFTWARE_HOST,false);ack_clamp=127;event(1,AVRCP_CTYPE_RESPONSE_INTERIM,127);
 advance(6000);assert(absolute_sets==1 && physical==127 && usb_volume_relay.headroom_attempted);
 fixture(127,SOFTWARE_HOST,false);ack_reject=true;event(1,AVRCP_CTYPE_RESPONSE_INTERIM,127);
 advance(6000);assert(absolute_sets==1 && physical==127 && usb_volume_relay.headroom_attempted);
 ack_reject=false;host_set(64);advance(100);assert(physical==64 && usb_volume_relay.anchor==64);
}
static void bounded_repair_and_ignored_host(void){
 fixture(47,SOFTWARE_HOST,true);drop_ack=true;changed(37);advance(5000);
 assert(absolute_sets==BT_VOLUME_ATTEMPTS && !usb_volume_relay.restoring && !sink_volume.inflight);
 fixture(47,IGNORING_HOST,true);changed(37);advance(250);
 assert(keys_down==1 && host_index==15 && physical==47);
 // Exposes the limit: successful USB submission cannot prove OS application.
}
int main(void){
 software_host_one_gain();hardware_host_and_late_sets();endpoints_and_unknown();target_request_is_not_physical();
 queue_failure_and_lifecycle();echoes_stale_set_and_receipts();bounded_repair_and_ignored_host();
 partial_delivery_and_inflight_gesture();headroom_no_progress_is_bounded();
 volume_sync_t saved=sink_volume;uint8_t relay[sizeof(usb_volume_relay)];memcpy(relay,&usb_volume_relay,sizeof(relay));
 unsigned sent=usb_media_volume_sent();bt_volume_report();
 assert(!memcmp(&saved,&sink_volume,sizeof(saved)) && !memcmp(relay,&usb_volume_relay,sizeof(relay)) && sent==usb_media_volume_sent());
 puts("USB volume: real HID queue, software/hardware hosts, anchor/SET races, endpoints and target intent passed");
}
