// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "../src/btstack/volume_sync.h"
#include "../src/btstack/volume_scale.h"
#include "../src/btstack/volume_trace.h"
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
static int ack_physical;
static uint8_t transfer_report[3];
static uint32_t now,revision,set_generation,receipt_snapshot;
static uint32_t volume_operations_up,volume_operations_down,volume_target_requests;
static uint32_t volume_changed_reports,volume_interim_reports,volume_ack_reports;
static uint8_t peer[6]={1,2,3,4,5,6},physical;
static unsigned absolute_sets,send_attempts,keys_up,keys_down,ack_delay,host_delay,host_mode;
static uint8_t gain_writes[128];
static uint32_t gain_write_times[128],last_host_set_at;
static int host_index;
static struct {uint16_t avrcp_cid;} media_tracker;
static struct {bool pending;uint32_t due;uint8_t label,level;} ack;
static struct {bool pending;uint32_t due;uint8_t level;} host_update;
static struct {bool pending;uint32_t due,epoch;uint8_t level;} interim;
static uint32_t registration_generation,event_generation;
static unsigned query_delay,queries;
static unsigned clock_reads;
static bool drop_interim,query_failed;
enum {SOFTWARE_HOST,HARDWARE_HOST,IGNORING_HOST};
void dongle_control_available(bool v){(void)v;}
void dongle_rpc_available(bool v){(void)v;}
uint16_t dongle_control_get(uint8_t *p,uint16_t n){(void)p;(void)n;return 0;}
uint16_t dongle_rpc_get(uint8_t *p,uint16_t n){(void)p;(void)n;return 0;}
void dongle_control_set(const uint8_t *p,uint16_t n){(void)p;(void)n;}
void dongle_rpc_set(const uint8_t *p,uint16_t n){(void)p;(void)n;}
uint32_t time_us_32(void){++clock_reads;return now*1000u;}
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
static void queue_interim(void){
 if(++registration_generation==0)++registration_generation;
 interim.pending=true;interim.due=now+query_delay;interim.epoch=registration_generation;interim.level=physical;++queries;
}
static uint8_t avrcp_controller_refresh_volume_notification(uint16_t cid){
 assert(cid==sink_volume.cid);if(query_failed){++queries;return 0x11;}
 queue_interim();return 0;
}
static uint32_t avrcp_controller_volume_registration_generation(uint16_t cid){assert(cid==sink_volume.cid);return registration_generation;}
static uint32_t avrcp_controller_volume_event_generation(uint16_t cid){assert(cid==sink_volume.cid);return event_generation;}
static void avrcp_controller_cancel_volume_refresh(uint16_t cid){assert(cid==sink_volume.cid);interim.pending=false;}
static uint16_t little_endian_read_16(const uint8_t *p,unsigned i){return (uint16_t)(p[i]|(p[i+1]<<8));}
static uint8_t avrcp_controller_set_absolute_volume(uint16_t cid,uint8_t level){
 assert(cid==sink_volume.cid && level<=127);++send_attempts;if(bt_busy)return 0x0c;
 assert(absolute_sets<128);gain_writes[absolute_sets]=level;gain_write_times[absolute_sets]=now;++absolute_sets;
 connection.transaction_id=(uint8_t)((connection.transaction_id+1u)&15u);
 ack_applied=false;
 ack.pending=true;ack.due=now+ack_delay;ack.level=level;ack.label=connection.transaction_id;return 0;
}
static avrcp_connection_t *avrcp_get_connection_for_avrcp_cid_for_role(unsigned role,uint16_t cid){
 assert(role==AVRCP_CONTROLLER);return cid==sink_volume.cid?&connection:NULL;
}
// Actual source handlers/state and separately compiled production HID queue.
#include "playback_volume.inc"
static void event_epoch(uint8_t subtype,uint8_t ctype,uint8_t value,uint32_t epoch){
 event_generation=epoch;
 uint8_t wire[]={0xc0,5,subtype,7,0,ctype,value};
 assert(bt_volume_controller_event(wire,7)==(sink_volume.cid==7));
}
static void event(uint8_t subtype,uint8_t ctype,uint8_t value){event_epoch(subtype,ctype,value,registration_generation);}
static void changed(uint8_t value){physical=value;event(1,AVRCP_CTYPE_RESPONSE_CHANGED_STABLE,value);queue_interim();}
static void host_set(uint8_t value){
 last_host_set_at=now;
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
   if(!ack_reject && !ack_applied)physical=ack_physical<0?level:(uint8_t)ack_physical;
   connection.controller_last_confirmed_transaction_id=label;
   event(2,ack_reject?AVRCP_CTYPE_RESPONSE_REJECTED:AVRCP_CTYPE_RESPONSE_ACCEPTED,level);}
  if(interim.pending && (int32_t)(now-interim.due)>=0){
   uint8_t level=interim.level;uint32_t epoch=interim.epoch;interim.pending=false;
   if(!drop_interim)event_epoch(1,AVRCP_CTYPE_RESPONSE_INTERIM,level,epoch);
  }
  usb_media_task();bt_volume_poll();
 }
}
static void fixture(uint8_t level,unsigned mode,bool baseline){
 transfer_pending=drop_transfer=false;
 usb_media_set_available(false);usb_media_set_available(true);hid_ready=true;usb_media_task();complete_report();
 memset(&sink_volume,0,sizeof(sink_volume));memset(&usb_volume_relay,0,sizeof(usb_volume_relay));
 connection=(avrcp_connection_t){0};now=revision=set_generation=receipt_snapshot=0;
 absolute_sets=send_attempts=keys_up=keys_down=volume_target_requests=0;ack.pending=host_update.pending=false;
 last_host_set_at=UINT32_MAX;
 ack_delay=host_delay=query_delay=1;queries=0;registration_generation=1;event_generation=0;
 interim.pending=drop_interim=query_failed=false;
 mic=phone_owner=drop_ack=ack_reject=ack_applied=bt_busy=false;ack_clamp=ack_physical=-1;
 is_usb_streaming=true;peer[0]=1;_bt_sink_volume_changed=false;
 host_mode=mode;host_index=mode==HARDWARE_HOST?level:15;physical=level;
 bt_volume_select_peer();volume_sync_connection(&sink_volume,7,now);media_tracker.avrcp_cid=7;bt_volume_relay_reset(true);
 if(baseline)event(1,AVRCP_CTYPE_RESPONSE_INTERIM,level);
 bt_volume_poll();advance(5);
}
static void wait_for_repair(bool inflight){
 for(unsigned i=0;i<1000 && !(usb_volume_relay.restoring && sink_volume.pending && sink_volume.inflight==inflight);++i)advance(1);
 assert(usb_volume_relay.restoring && sink_volume.pending && sink_volume.inflight==inflight);
}
static void software_host_one_gain(void){
 fixture(47,SOFTWARE_HOST,true);assert(!revision && !absolute_sets);
 changed(37);advance(200);assert(keys_down==1 && host_index==14 && physical==47 && !revision);
 assert(usb_volume_relay.anchor==47 && !usb_volume_relay.waiting && !_bt_sink_volume_changed);
 advance(6000);changed(57);advance(200);assert(keys_up==1 && host_index==15 && physical==47);
 host_index=8;changed(37);advance(200);assert(host_index==7 && physical==47);
 changed(57);advance(200);assert(host_index==8 && physical==47);
 changed(37);changed(27);changed(37);advance(250);
 assert(host_index==7 && physical==47 && keys_down==3 && keys_up==2);
 // Changes coalesced before a fresh readback produce one verified net change.
}
static void hardware_host_and_late_sets(void){
 const unsigned delays[]={1,50,500,3000};
 for(unsigned i=0;i<sizeof(delays)/sizeof(delays[0]);++i){
  fixture(64,HARDWARE_HOST,true);host_delay=delays[i];changed(54);advance(delays[i]+250);
  uint8_t readback=0;assert(host_index==56 && physical==56 && usb_volume_relay.anchor==56);
  assert(revision==1 && get_bt_volume(&readback) && readback==56 && bt_volume_usb_ready());
  for(unsigned j=0;j<absolute_sets;++j){
   assert(gain_writes[j]==64 || gain_writes[j]==56);
   if(gain_write_times[j]>=last_host_set_at)assert(gain_writes[j]==56);
  }
  assert(_bt_sink_volume_changed); // Confirmed native UAC readback stays available.
  changed(66);advance(delays[i]+250);assert(host_index==64 && physical==64 && keys_up==1 && keys_down==1);
 }
 fixture(47,SOFTWARE_HOST,true);ack_delay=50;changed(37);wait_for_repair(true);
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
 for(unsigned level=90;level>=10;level-=10){changed((uint8_t)level);advance(2);}
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
 fixture(47,SOFTWARE_HOST,true);ack_delay=50;changed(37);wait_for_repair(true);assert(usb_volume_relay.restoring && sink_volume.inflight);
 changed(46);advance(100);assert(keys_down==1 && !keys_up && physical==47);
 ++revision;uint32_t old_revision=revision,generation=usb_media_generation();++revision;set_generation=generation;
 set_bt_volume_from_usb(-5000,old_revision,generation,usb_media_volume_sent());assert(!sink_volume.pending);
 set_bt_volume_from_usb(-5000,revision,generation-2,usb_media_volume_sent());assert(!sink_volume.pending);
 fixture(47,SOFTWARE_HOST,true);hid_ready=false;changed(57);advance(2);changed(67);advance(2);hid_ready=true;advance(2);
 ++revision;uint32_t receipt=usb_media_volume_sent();advance(2);assert(usb_media_volume_sent()!=receipt);
 set_bt_volume_from_usb(bt_volume_to_usb(60),revision,usb_media_generation(),receipt);
 assert(usb_volume_relay.waiting && usb_volume_relay.delivered);advance(250);assert(physical==60);
}
static void partial_delivery_and_inflight_gesture(void){
 fixture(47,SOFTWARE_HOST,true);is_usb_streaming=false;bt_volume_poll();is_usb_streaming=true;
 changed(37);advance(250);assert(keys_down==1 && physical==47); // Gesture before the first owned poll.
 fixture(47,SOFTWARE_HOST,true);hid_ready=false;changed(37);advance(2);changed(27);advance(2);changed(17);advance(2);
 hid_ready=true;advance(2);assert(keys_down==1);hid_ready=false;advance(500);
 assert(!absolute_sets && physical==17 && !usb_volume_relay.delivered);
 hid_ready=true;advance(2);hid_ready=false;advance(500);
 assert(keys_down==2 && !absolute_sets && physical==17 && !usb_volume_relay.delivered);
 hid_ready=true;advance(250);assert(keys_down==3 && physical==47 && host_index==12);
 fixture(47,SOFTWARE_HOST,true);ack_delay=50;changed(37);wait_for_repair(true);
 assert(usb_volume_relay.restoring && sink_volume.inflight);
 ack_applied=true;physical=47;changed(37); // A real local event before ACK, then current readback still37.
 advance(300);assert(keys_down==2 && host_index==13 && physical==47 && sink_volume.confirmed==47);
 fixture(47,SOFTWARE_HOST,true);drop_transfer=true;changed(37);advance(300);
 assert(transfer_pending && !keys_down && !absolute_sets && physical==37);
 advance(5000);assert(!absolute_sets && physical==37 && !usb_volume_relay.waiting);
 fixture(47,SOFTWARE_HOST,true);bt_busy=true;changed(37);wait_for_repair(false);
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
static void fresh_readback_rejects_feedback_loops(void){
 fixture(127,SOFTWARE_HOST,false);ack_physical=127;event(1,AVRCP_CTYPE_RESPONSE_INTERIM,127);
 advance(1000);assert(absolute_sets==1 && physical==127 && usb_volume_relay.anchor==127 && !keys_up && !keys_down);
 for(unsigned i=0;i<4;++i){changed(127);advance(10);}
 assert(absolute_sets==1 && !keys_up && !keys_down); // ACK126 cannot turn canonical127 into repeated UP.
 fixture(47,SOFTWARE_HOST,true);changed(37);advance(100);assert(keys_down==1 && physical==47);
 for(unsigned i=0;i<4;++i){
  event(1,AVRCP_CTYPE_RESPONSE_CHANGED_STABLE,37);queue_interim();advance(10);
 }
 assert(keys_down==1 && !keys_up && absolute_sets==1 && physical==47); // Delayed payload is not a new gesture.
 fixture(47,SOFTWARE_HOST,true);hid_ready=false;changed(37);advance(2);
 uint32_t older=usb_volume_relay.physical_generation;
 changed(57);advance(2);assert(sink_volume.confirmed==57);
 event_epoch(1,AVRCP_CTYPE_RESPONSE_INTERIM,37,older);
 assert(sink_volume.confirmed==57 && usb_volume_relay.physical_generation>older);
 hid_ready=true;advance(100);assert(keys_up==1 && keys_down==1 && host_index==15 && physical==47);
}
static void post_set_local_change_and_unsupported_readback(void){
 fixture(126,SOFTWARE_HOST,true);ack_delay=50;changed(116);wait_for_repair(true);
 ack_applied=true;physical=126;changed(108);advance(2); // A genuine local event's fresh INTERIM precedes SET ACK.
 assert(usb_volume_relay.local_change_pending && sink_volume.inflight);
 advance(250);assert(keys_down==2 && !keys_up && host_index==13 && physical==126 && usb_volume_relay.anchor==126);
 fixture(47,SOFTWARE_HOST,true);query_failed=true;changed(37);advance(1200);
 assert(usb_volume_relay.readback_unavailable && !usb_volume_relay.readback_pending && !usb_volume_relay.restoring);
 unsigned previous_keys=keys_down;changed(37);advance(10);
 uint8_t level=0;assert(keys_down==previous_keys && get_bt_volume(&level) && level==37 && bt_volume_usb_ready());
 host_set(64);advance(20);assert(physical==64 && get_bt_volume(&level) && level==64 && bt_volume_usb_ready());
 fixture(47,SOFTWARE_HOST,true);drop_interim=true;changed(37);advance(1600);
 assert(!keys_down && !absolute_sets && usb_volume_relay.readback_unavailable);
 drop_interim=false;changed(27);advance(10);assert(get_bt_volume(&level) && level==27 && !keys_down && bt_volume_usb_ready());
 fixture(47,SOFTWARE_HOST,true);hid_ready=false;changed(37);advance(2);hid_ready=true;advance(1);
 ++revision;set_generation=usb_media_generation();uint32_t receipt=usb_media_volume_sent();
 advance(100);assert(!absolute_sets && physical==37); // A SET observed in USB IRQ fences repair before main consumes it.
 set_bt_volume_from_usb(bt_volume_to_usb(20),revision,set_generation,receipt);advance(50);
 assert(absolute_sets==1 && gain_writes[0]==20 && physical==20);
 fixture(47,SOFTWARE_HOST,true);drop_interim=true;changed(37);advance(1600);
 assert(usb_volume_relay.readback_unavailable && !usb_volume_relay.readback_pending);
 event_epoch(1,AVRCP_CTYPE_RESPONSE_INTERIM,37,registration_generation);
 assert(get_bt_volume(&level) && level==37 && bt_volume_usb_ready() && !keys_down && !usb_volume_relay.enabled);
}
static void trace_is_bounded_and_default_off(void){
 fixture(47,SOFTWARE_HOST,true);clock_reads=0;
 event(1,AVRCP_CTYPE_RESPONSE_INTERIM,47);assert(!clock_reads); // No timestamp/counter work when tracing is off.
 bt_volume_trace_control(true);changed(37);advance(100);
 unsigned total=bt_volume_trace_control(false),rows=0;uint32_t record[4];
 assert(total>0);while(bt_volume_trace_take(record))++rows;
 assert(rows>0 && rows<=VOLUME_TRACE_EVENTS && rows<=total);
}
int main(void){
 software_host_one_gain();hardware_host_and_late_sets();endpoints_and_unknown();target_request_is_not_physical();
 queue_failure_and_lifecycle();echoes_stale_set_and_receipts();bounded_repair_and_ignored_host();
 partial_delivery_and_inflight_gesture();headroom_no_progress_is_bounded();
 fresh_readback_rejects_feedback_loops();post_set_local_change_and_unsupported_readback();trace_is_bounded_and_default_off();
 volume_sync_t saved=sink_volume;uint8_t relay[sizeof(usb_volume_relay)];memcpy(relay,&usb_volume_relay,sizeof(relay));
 unsigned sent=usb_media_volume_sent();bt_volume_report();
 assert(!memcmp(&saved,&sink_volume,sizeof(saved)) && !memcmp(relay,&usb_volume_relay,sizeof(relay)) && sent==usb_media_volume_sent());
 puts("USB volume: real HID queue, software/hardware hosts, anchor/SET races, endpoints and target intent passed");
}
