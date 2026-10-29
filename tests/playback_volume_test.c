// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "../src/btstack/volume_sync.h"
#include "../src/btstack/volume_scale.h"
#include "../src/btstack/volume_trace.h"
#include "../src/btstack/volume_authority.h"
#include "../src/tinyusb/media_control.h"
enum {AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED=1,AVRCP_SUBEVENT_SET_ABSOLUTE_VOLUME_RESPONSE=2,
 AVRCP_CTYPE_RESPONSE_INTERIM=15,AVRCP_CTYPE_RESPONSE_CHANGED_STABLE=13,AVRCP_CTYPE_NOTIFY=3,
 AVRCP_CTYPE_RESPONSE_ACCEPTED=9,AVRCP_CTYPE_RESPONSE_REJECTED=10,AVRCP_CTYPE_RESPONSE_NOT_IMPLEMENTED=8,
 AVRCP_CONTROLLER=0,ERROR_CODE_SUCCESS=0};
typedef struct {
 uint8_t transaction_id,controller_last_confirmed_transaction_id;
 uint32_t controller_volume_generation;
 bool controller_volume_active,controller_volume_interim;
} avrcp_connection_t;
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
static int16_t host_db256;
static struct {uint16_t avrcp_cid;} media_tracker;
static struct {bool pending;uint32_t due;uint8_t label,level;} ack;
static struct {bool pending;uint32_t due;uint8_t level;int16_t db256;} host_update;
static struct {bool pending;uint32_t due,epoch;uint8_t level;} interim;
static uint32_t registration_generation,event_generation;
static unsigned query_delay,queries;
static unsigned clock_reads;
static bool drop_interim;
static bool audio_owned=true;
static bool profile_connected=true;
static uint32_t control_generation=1;
static bool aacp_audio_owned(void){return audio_owned;}
static bool get_a2dp_connected_flag(void){return profile_connected;}
static bool usb_audio_available(void){return usb_media_generation()&1u;}
static uint32_t dongle_control_client_generation(uint8_t client){assert(client==0);return control_generation;}
enum {SOFTWARE_HOST,HARDWARE_HOST,IGNORING_HOST,FRACTIONAL_HOST};
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
 if(host_mode==FRACTIONAL_HOST){
  int db=(int)host_db256+step*40;
  host_db256=(int16_t)(db< -12800?-12800:db>0?0:db);
  host_update.pending=true;host_update.due=now+host_delay;host_update.db256=host_db256;
 }
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
 connection.controller_volume_generation=registration_generation;
 connection.controller_volume_active=true;connection.controller_volume_interim=false;
 interim.pending=true;interim.due=now+query_delay;interim.epoch=registration_generation;interim.level=physical;++queries;
}
static uint32_t avrcp_controller_volume_event_generation(uint16_t cid){assert(cid==sink_volume.cid);return event_generation;}
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
 if(subtype==AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED) {
  if(ctype==AVRCP_CTYPE_RESPONSE_INTERIM) connection.controller_volume_interim=true;
  else if(ctype==AVRCP_CTYPE_RESPONSE_CHANGED_STABLE) connection.controller_volume_active=connection.controller_volume_interim=false;
 }
 uint8_t wire[]={0xc0,5,subtype,7,0,ctype,value};
 assert(bt_volume_controller_event(wire,7)==(sink_volume.cid==7));
}
static void event(uint8_t subtype,uint8_t ctype,uint8_t value){event_epoch(subtype,ctype,value,registration_generation);}
static void changed(uint8_t value){physical=value;event(1,AVRCP_CTYPE_RESPONSE_CHANGED_STABLE,value);queue_interim();}
static void host_set_db(int16_t db256){
 if(host_mode==FRACTIONAL_HOST)host_db256=db256;
 last_host_set_at=now;
 ++revision;set_generation=usb_media_generation();receipt_snapshot=usb_media_volume_sent();
 set_bt_volume_from_usb(db256,revision,set_generation,receipt_snapshot);
}
static void host_set(uint8_t value){host_set_db(bt_volume_to_usb(value));}
static void advance(unsigned milliseconds){
 for(unsigned i=0;i<milliseconds;++i){++now;
  complete_report();
  if(host_update.pending && (int32_t)(now-host_update.due)>=0){
   uint8_t level=host_update.level;host_update.pending=false;
   if(host_mode==FRACTIONAL_HOST)host_set_db(host_update.db256);else host_set(level);}
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
  if(host_mode==FRACTIONAL_HOST && _bt_sink_volume_changed && bt_volume_usb_ready()) {
   uint8_t level;if(get_bt_volume(&level))host_db256=bt_volume_to_usb(level);
   _bt_sink_volume_changed=false;
  }
 }
}
static void fixture(uint8_t level,unsigned mode,bool baseline){
 transfer_pending=drop_transfer=false;
 usb_media_set_available(false);usb_media_set_available(true);hid_ready=true;usb_media_task();complete_report();
 memset(&sink_volume,0,sizeof(sink_volume));memset(&usb_volume_relay,0,sizeof(usb_volume_relay));
 memset(&usb_host_volume,0,sizeof(usb_host_volume));volume_observation_revision=0;host_volume_write=host_volume_quiet=false;
 connection=(avrcp_connection_t){0};now=revision=set_generation=receipt_snapshot=0;
 absolute_sets=send_attempts=keys_up=keys_down=volume_target_requests=0;ack.pending=host_update.pending=false;
 last_host_set_at=UINT32_MAX;
 ack_delay=host_delay=query_delay=1;queries=0;registration_generation=1;event_generation=0;
 connection.controller_volume_generation=registration_generation;
 connection.controller_volume_active=true;
 interim.pending=drop_interim=false;
 audio_owned=profile_connected=true;control_generation=1;
 mic=phone_owner=drop_ack=ack_reject=ack_applied=bt_busy=false;ack_clamp=ack_physical=-1;
 is_usb_streaming=true;peer[0]=1;_bt_sink_volume_changed=false;
 host_mode=mode;host_index=mode==HARDWARE_HOST?level:15;physical=level;
 host_db256=bt_volume_to_usb(level);
 bt_volume_select_peer();volume_sync_connection(&sink_volume,7,now);media_tracker.avrcp_cid=7;bt_volume_relay_reset(true);
 if(baseline)event(1,AVRCP_CTYPE_RESPONSE_INTERIM,level);
 bt_volume_poll();advance(5);
}
static void assert_native_volume(void) {
 assert(!keys_up && !keys_down && !usb_media_volume_sent());
}
static void accessory_steps_remain_authoritative(void) {
 fixture(47,FRACTIONAL_HOST,true);
 changed(39);advance(250);
 assert(physical==39 && sink_volume.confirmed==39 && host_db256==bt_volume_to_usb(39));
 assert(!absolute_sets && !sink_volume.pending && !sink_volume.inflight);
 is_usb_streaming=false;changed(31);advance(6000);
 assert(physical==31 && host_db256==bt_volume_to_usb(31) && !absolute_sets);
 is_usb_streaming=true;changed(39);advance(250);
 assert(physical==39 && host_db256==bt_volume_to_usb(39) && !absolute_sets);
 assert_native_volume();
 // No automatic maximum headroom write, and no feedback cue before actual bud maximum.
 fixture(127,FRACTIONAL_HOST,true);advance(6000);
 assert(physical==127 && !absolute_sets);
 changed(119);advance(250);assert(physical==119 && host_db256==bt_volume_to_usb(119));
 changed(127);advance(250);assert(physical==127 && host_db256==0 && !absolute_sets);
 fixture(0,FRACTIONAL_HOST,true);advance(6000);assert(!physical && !absolute_sets);
 changed(8);advance(250);assert(physical==8 && host_db256==bt_volume_to_usb(8));
 assert_native_volume();
}
static void host_requests_preserve_exact_db_and_actual_clamps(void) {
 fixture(121,FRACTIONAL_HOST,true);
 // Adjacent fine USB values may map to the same AVRCP level; do not round the host back up.
 host_set_db(-620);advance(20);
 assert(!absolute_sets && host_db256==-620 && !_bt_sink_volume_changed);
 for(unsigned i=1;i<=12;++i) {
  int16_t expected=(int16_t)(-620-(int)i*40);host_set_db(expected);advance(20);
  assert(host_db256==expected && physical==usb_volume_to_bt(expected));
 }
 for(unsigned i=1;i<=12;++i) {
  int16_t expected=(int16_t)(-1100+(int)i*40);host_set_db(expected);advance(20);
  assert(host_db256==expected && physical==usb_volume_to_bt(expected));
 }
 changed(113);advance(250);
 assert(physical==113 && host_db256==bt_volume_to_usb(113));
 unsigned writes=absolute_sets;advance(6000);assert(absolute_sets==writes && physical==113);
 host_set_db(-1000);advance(20);
 assert(host_db256==-1000 && physical==usb_volume_to_bt(-1000));
 event_epoch(1,AVRCP_CTYPE_RESPONSE_INTERIM,physical,++registration_generation);advance(1);
 assert(host_db256==-1000);
 ack_clamp=100;host_set_db(-2000);advance(20);
 assert(physical==100 && host_db256==bt_volume_to_usb(100) && !usb_volume_relay.host_exact);
 assert_native_volume();
 fixture(47,FRACTIONAL_HOST,true);ack_reject=true;host_set(20);advance(20);
 assert(physical==47 && host_db256==bt_volume_to_usb(47) && !sink_volume.pending);
 assert_native_volume();
}
static void native_observations_keep_receive_order(void) {
 for(unsigned managed=0;managed<2;++managed) {
  fixture(47,FRACTIONAL_HOST,true);ack_delay=50;
  if(managed) {
   volume_authority_scope_t scope=bt_volume_authority_scope();
   assert(volume_authority_manage(&usb_host_volume,0,&scope,0,0,0,now)==DONGLE_ACCEPTED);
  }
  host_set(70);
  if(managed) assert(volume_sync_request(&sink_volume,70,false,now));
  advance(1);assert(sink_volume.inflight);
  ack_applied=true;physical=70;
  changed(57);advance(2);
  assert(sink_volume.confirmed==57 && sink_volume.inflight);
  uint32_t observation=volume_observation_revision;advance(100);
  assert(physical==57 && sink_volume.confirmed==57 && volume_observation_revision==observation);
  assert(!usb_volume_relay.readback_pending && !sink_volume.pending && !sink_volume.inflight);
  assert_native_volume();
 }
}
static void accepted_set_fences_pending_interim(void) {
 for(unsigned initial=0;initial<2;++initial) {
  fixture(121,FRACTIONAL_HOST,!initial);query_delay=100;
  if(initial) queue_interim();else changed(121);
  host_set(80);advance(20);assert(physical==80 && sink_volume.confirmed==80);
  uint32_t observation=volume_observation_revision;advance(100);
  assert(physical==80 && sink_volume.confirmed==80 && host_db256==bt_volume_to_usb(80));
  assert(volume_observation_revision==observation && !usb_volume_relay.readback_pending);
  query_delay=1;changed(72);advance(250);
  assert(physical==72 && sink_volume.confirmed==72 && host_db256==bt_volume_to_usb(72));
  assert(absolute_sets==1);assert_native_volume();
 }
 fixture(121,FRACTIONAL_HOST,true);registration_generation=UINT32_MAX-1u;query_delay=100;
 usb_volume_relay.physical_generation=registration_generation;
 changed(121);host_set(80);advance(120);
 assert(sink_volume.confirmed==80 && usb_volume_relay.acknowledged_generation==UINT32_MAX);
 query_delay=1;changed(72);advance(250);
 assert(registration_generation==1 && physical==72 && host_db256==bt_volume_to_usb(72));
 assert_native_volume();
}
static void absent_or_stale_readback_never_restores_gain(void) {
 fixture(47,FRACTIONAL_HOST,false);advance(1000);assert(!sink_volume.known && !absolute_sets);
 changed(39);advance(20);assert(sink_volume.known && physical==39 && !absolute_sets);
 fixture(47,FRACTIONAL_HOST,true);drop_interim=true;changed(39);advance(6000);
 assert(physical==39 && !sink_volume.known && !usb_volume_relay.readback_pending && !absolute_sets);
 drop_interim=false;event(1,AVRCP_CTYPE_RESPONSE_INTERIM,39);advance(20);
 assert(sink_volume.known && host_db256==bt_volume_to_usb(39) && !absolute_sets);
 uint32_t older=usb_volume_relay.physical_generation;changed(31);advance(20);
 event_epoch(1,AVRCP_CTYPE_RESPONSE_INTERIM,47,older);advance(20);
 assert(physical==31 && sink_volume.confirmed==31 && host_db256==bt_volume_to_usb(31));
 assert_native_volume();
}
static void newer_host_requests_and_stale_receipts(void) {
 fixture(47,FRACTIONAL_HOST,true);ack_delay=50;
 host_set(70);advance(1);assert(sink_volume.inflight);
 host_set(20);advance(200);
 assert(physical==20 && sink_volume.confirmed==20 && absolute_sets==2);
 ++revision;uint32_t older=revision,generation=usb_media_generation();++revision;set_generation=generation;
 set_bt_volume_from_usb(-5000,older,generation,usb_media_volume_sent());
 set_bt_volume_from_usb(-5000,revision,generation-2,usb_media_volume_sent());
 assert(!sink_volume.pending && physical==20);
 assert_native_volume();
}
static void pause_reconnect_and_ownership_do_not_replay_gain(void) {
 fixture(47,FRACTIONAL_HOST,true);changed(39);advance(20);is_usb_streaming=false;
 advance(6000);is_usb_streaming=true;advance(20);assert(physical==39 && !absolute_sets);
 volume_sync_connection(&sink_volume,0,now);bt_volume_relay_reset(false);advance(20);
 volume_sync_connection(&sink_volume,7,now);bt_volume_relay_reset(false);
 event_epoch(1,AVRCP_CTYPE_RESPONSE_INTERIM,39,++registration_generation);advance(20);
 assert(physical==39 && host_db256==bt_volume_to_usb(39) && !absolute_sets);
 // A current explicit SET can await AVRCP reconnection, unlike retired automatic repairs.
 volume_sync_connection(&sink_volume,0,now);bt_volume_relay_reset(false);host_set_db(-5000);advance(20);
 assert(!absolute_sets && sink_volume.pending);
 volume_sync_connection(&sink_volume,7,now);bt_volume_relay_reset(false);advance(100);
 assert(physical==usb_volume_to_bt(-5000) && host_db256==-5000 && absolute_sets==1);
 fixture(47,FRACTIONAL_HOST,true);bt_busy=true;host_set(20);advance(20);
 audio_owned=false;volume_sync_connection(&sink_volume,0,now);bt_volume_relay_reset(false);advance(20);
 volume_sync_connection(&sink_volume,7,now);bt_volume_relay_reset(false);
 bt_busy=false;audio_owned=true;advance(6000);
 assert(!absolute_sets && physical==47);
 // An explicit request received while in standby is scoped and deferred until takeover.
 audio_owned=false;host_set(30);advance(20);audio_owned=true;advance(100);
 assert(absolute_sets==1 && physical==30);
 assert_native_volume();
}
static uint8_t host_authority_request(uint8_t client,uint8_t action,uint32_t token,uint8_t level,uint8_t out[24]) {
 uint8_t request[7]={17,action,0,0,0,0,level};dongle_put32(request+2,token);
 uint16_t length=0;uint8_t result=dongle_volume_request(client,request,sizeof(request),out,24,&length);
 if(result==DONGLE_ACCEPTED)assert(length==24);
 return result;
}
static uint32_t acquire_host_authority(void) {
 uint8_t out[24];assert(host_authority_request(0,0,0,0,out)==DONGLE_ACCEPTED);
 assert((out[1]&8) && dongle_u32(out+8));return dongle_u32(out+8);
}
static void request_host_volume(bool managed,uint32_t token,uint8_t level) {
 if(managed) {
  uint8_t out[24];assert(host_authority_request(0,3,token,level,out)==DONGLE_ACCEPTED);
 } else host_set(level);
}
static void overlapping_host_and_accessory_intents(void) {
 enum {GESTURE,ECHO,CLAMPED_ECHO,BASELINE,LATE_INTERIM,LATER_HOST,SECOND_GESTURE,MISSING_INTERIM,ECHO_AFTER_ACK};
 for(unsigned managed=0;managed<2;++managed)for(unsigned kind=0;kind<9;++kind) {
  fixture(47,FRACTIONAL_HOST,true);uint32_t token=managed?acquire_host_authority():0;
  ack_delay=50;query_delay=kind==LATE_INTERIM?100:1;
  request_host_volume(managed,token,80);advance(1);assert(sink_volume.inflight);
  // Model the SET applying before the native gesture, with only its ACK delayed.
  ack_applied=true;physical=80;
  request_host_volume(managed,token,50);
  if(kind==ECHO_AFTER_ACK) {
   ack.pending=false;connection.controller_last_confirmed_transaction_id=ack.label;
   event(2,AVRCP_CTYPE_RESPONSE_ACCEPTED,80);
  }
  if(kind==BASELINE)event_epoch(1,AVRCP_CTYPE_RESPONSE_INTERIM,80,++registration_generation);
  else {
   uint8_t observed=kind==ECHO || kind==ECHO_AFTER_ACK?80:kind==CLAMPED_ECHO?65:45;
   if(kind==CLAMPED_ECHO)ack.level=65;
   if(kind==MISSING_INTERIM)drop_interim=true;
   uint32_t changed_epoch=registration_generation;changed(observed);advance(2);
   if(kind==LATER_HOST) {
    request_host_volume(managed,token,30);
    // A duplicate old CHANGED cannot cancel that newer explicit request.
    event_epoch(1,AVRCP_CTYPE_RESPONSE_CHANGED_STABLE,45,changed_epoch);
   } else if(kind==SECOND_GESTURE) {
    request_host_volume(managed,token,60);changed(40);advance(2);
   }
  }
  if(kind==LATE_INTERIM) {
   advance(60);assert(absolute_sets==1 && !sink_volume.inflight && usb_volume_relay.change_pending);
  }
  advance(kind==MISSING_INTERIM?1100:200);
  uint8_t expected=kind==ECHO || kind==CLAMPED_ECHO || kind==BASELINE || kind==ECHO_AFTER_ACK?50:
      kind==LATER_HOST?30:kind==SECOND_GESTURE?40:45;
  assert(physical==expected && absolute_sets==((kind==ECHO || kind==CLAMPED_ECHO || kind==BASELINE || kind==LATER_HOST || kind==ECHO_AFTER_ACK)?2u:1u));
  assert(!sink_volume.pending && !sink_volume.inflight && !usb_volume_relay.change_pending);
  if(kind!=MISSING_INTERIM)assert(sink_volume.known && sink_volume.confirmed==expected);
  else {
   assert(!sink_volume.known);
   request_host_volume(managed,token,30);advance(100);
   assert(physical==30 && absolute_sets==2);
  }
  assert_native_volume();
 }
}
static void explicit_host_bridge_remains_isolated(void) {
 fixture(47,FRACTIONAL_HOST,true);uint32_t token=acquire_host_authority();uint8_t out[24];
 changed(39);advance(20);
 assert(physical==39 && !absolute_sets && host_db256==bt_volume_to_usb(47));
 assert(dongle_volume_snapshot(0,out,24)==24 && (out[1]&9)==9 && out[2]==39);
 assert(host_authority_request(0,3,token,70,out)==DONGLE_ACCEPTED);advance(20);
 assert(physical==70 && absolute_sets==1 && !_bt_sink_volume_changed);
 host_set(127);advance(20);assert(physical==70 && absolute_sets==1);
 is_usb_streaming=false;assert(host_authority_request(0,1,token,0,out)==DONGLE_ACCEPTED);
 assert(host_authority_request(0,3,token,127,out)==DONGLE_ACCEPTED);advance(20);
 assert(physical==127 && absolute_sets==2);
 assert(host_authority_request(0,2,token,0,out)==DONGLE_ACCEPTED);advance(6000);
 assert(physical==127 && absolute_sets==2 && host_volume_quiet);
 changed(119);advance(20);assert(physical==119 && absolute_sets==2);
 assert_native_volume();
 // Expiry cancels unsent commands and never falls back to relative-volume repair.
 fixture(47,FRACTIONAL_HOST,true);token=acquire_host_authority();bt_busy=true;
 assert(host_authority_request(0,3,token,80,out)==DONGLE_ACCEPTED);advance(5100);bt_busy=false;advance(100);
 assert(!usb_host_volume.active && host_volume_quiet && !sink_volume.pending && !absolute_sets && physical==47);
 assert(host_authority_request(0,3,token,90,out)==DONGLE_STALE);assert_native_volume();
}
static void authority_scope_and_validation(void) {
 uint8_t out[24];
 for(unsigned kind=0;kind<5;++kind) {
  fixture(47,FRACTIONAL_HOST,true);uint32_t token=acquire_host_authority();bt_busy=true;
  assert(host_authority_request(0,3,token,80,out)==DONGLE_ACCEPTED);advance(20);
  switch(kind) {
   case 0:usb_media_set_available(false);break;
   case 1:++control_generation;break;
   case 2:volume_sync_connection(&sink_volume,9,now);break;
   case 3:peer[0]=2;break;
   case 4:audio_owned=false;break;
  }
  advance(20);bt_busy=false;advance(100);
  assert(!usb_host_volume.active && !sink_volume.pending && !absolute_sets && physical==47);
 }
 fixture(47,FRACTIONAL_HOST,true);uint32_t token=acquire_host_authority();
 volume_authority_t authority=usb_host_volume;volume_sync_t saved=sink_volume;
 assert(host_authority_request(0,3,token,128,out)==DONGLE_MALFORMED);
 assert(host_authority_request(1,3,token,30,out)==DONGLE_UNSUPPORTED);
 assert(host_authority_request(0,3,token+1,30,out)==DONGLE_STALE);
 assert(!memcmp(&authority,&usb_host_volume,sizeof(authority)) && !memcmp(&saved,&sink_volume,sizeof(saved)));
 assert(!dongle_volume_snapshot(0,out,23));dongle_volume_client_disconnect(0);advance(20);
 assert(!usb_host_volume.active && host_volume_quiet && !absolute_sets);assert_native_volume();
}
static void target_requests_are_not_physical_gain(void) {
 fixture(47,FRACTIONAL_HOST,true);volume_sync_t before=sink_volume;
 uint8_t wire[]={0xc0,5,AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED,7,0,AVRCP_CTYPE_NOTIFY,127};
 assert(bt_volume_target_event(wire,7));assert(volume_target_requests==1 && usb_volume_relay.target_known);
 assert(!memcmp(&before,&sink_volume,sizeof(before)) && !absolute_sets);
 wire[5]=13;assert(!bt_volume_target_event(wire,7));wire[5]=3;wire[6]=128;
 assert(!bt_volume_target_event(wire,7));assert_native_volume();
}
static void trace_and_reports_remain_passive(void) {
 fixture(47,FRACTIONAL_HOST,true);clock_reads=0;
 event(1,AVRCP_CTYPE_RESPONSE_INTERIM,47);assert(!clock_reads);
 assert(bt_volume_trace_control(true)==0);
 for(unsigned i=0;i<24;++i){changed(i&1?47:39);advance(20);}
 assert(!absolute_sets);uint32_t total=bt_volume_trace_control(false),row[4];unsigned rows=0;
 while(bt_volume_trace_take(row))++rows;
 assert(total>0 && rows>0 && rows<=VOLUME_TRACE_EVENTS && rows<=total);
 volume_sync_t saved=sink_volume;uint8_t state[sizeof(usb_volume_relay)];memcpy(state,&usb_volume_relay,sizeof(state));
 bt_volume_report();assert(!memcmp(&saved,&sink_volume,sizeof(saved)) && !memcmp(state,&usb_volume_relay,sizeof(state)));
 assert_native_volume();
}
int main(void) {
 accessory_steps_remain_authoritative();host_requests_preserve_exact_db_and_actual_clamps();
 native_observations_keep_receive_order();accepted_set_fences_pending_interim();
 absent_or_stale_readback_never_restores_gain();newer_host_requests_and_stale_receipts();
 pause_reconnect_and_ownership_do_not_replay_gain();explicit_host_bridge_remains_isolated();
 overlapping_host_and_accessory_intents();
 authority_scope_and_validation();target_requests_are_not_physical_gain();trace_and_reports_remain_passive();
 puts("Native USB volume: accessory steps, UAC readback, exact host dB, epochs, pause/reconnect and leased host bridge passed; no HID volume or anchor repair");
 return 0;
}
