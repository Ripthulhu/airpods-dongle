// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum {AVRCP_NOTIFICATION_EVENT_FIRST_INDEX=1,AVRCP_NOTIFICATION_EVENT_LAST_INDEX=14,
 AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED=13,AVRCP_CONTROLLER=0,
 AVRCP_CTYPE_RESPONSE_INTERIM=15,AVRCP_CTYPE_RESPONSE_CHANGED_STABLE=13,
 AVRCP_CTYPE_RESPONSE_REJECTED=10,AVRCP_CTYPE_NOTIFY=3,AVRCP_SINGLE_PACKET=0,
 AVRCP_COMMAND_FRAME=0,BLUETOOTH_SERVICE_CLASS_AV_REMOTE_CONTROL=0x110e,
 AVRCP_SUBUNIT_TYPE_PANEL=9,AVRCP_SUBUNIT_ID=0,AVRCP_CMD_OPCODE_VENDOR_DEPENDENT=0,
 BT_SIG_COMPANY_ID=0x001958,AVRCP_PDU_ID_REGISTER_NOTIFICATION=0x31,
 HCI_EVENT_AVRCP_META=0xc0,AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED=0x1d,HCI_EVENT_PACKET=4,
 AVRCP_REMOTE_CAPABILITIES_NONE=0,AVRCP_REMOTE_CAPABILITIES_W4_QUERY_RESULT=1,
 AVRCP_REMOTE_CAPABILITIES_KNOWN=2,ERROR_CODE_SUCCESS=0,
 ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER=2,ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE=0x11,
 ERROR_CODE_COMMAND_DISALLOWED=0x0c};
typedef uint8_t avrcp_command_type_t;
typedef uint8_t avrcp_notification_event_id_t;
typedef struct {
 uint16_t avrcp_cid,l2cap_signaling_cid,notifications_enabled,notifications_supported_by_target;
 uint16_t controller_initial_status_reported,controller_notifications_to_register,controller_notifications_to_deregister;
 uint8_t transaction_id,transaction_id_counter,controller_last_confirmed_transaction_id,remote_capabilities_state;
 bool controller_notifications_supported_by_target_suppress_emit_result;
#include "volume_sdk_fields.inc"
} avrcp_connection_t;
static avrcp_connection_t connection;
static uint8_t wire[18],last_event[7];
static unsigned emitted,enabled_events,requests,failed_events,completed_events;
static uint8_t completed_event,completed_status;
static uint32_t received_epoch;
static int send_status;
static void callback(uint8_t type,uint16_t channel,uint8_t *p,uint16_t n){
 assert(type==HCI_EVENT_PACKET && !channel && n==7);memcpy(last_event,p,7);++emitted;
 received_epoch=connection.controller_volume_event_generation;
}
static struct {void (*avrcp_callback)(uint8_t,uint16_t,uint8_t *,uint16_t);} avrcp_controller_context={callback};
static avrcp_connection_t *avrcp_get_connection_for_avrcp_cid_for_role(unsigned role,uint16_t cid){
 assert(role==AVRCP_CONTROLLER);return cid==7?&connection:NULL;
}
static void little_endian_store_16(uint8_t *p,unsigned i,uint16_t v){p[i]=(uint8_t)v;p[i+1]=(uint8_t)(v>>8);}
static void big_endian_store_16(uint8_t *p,unsigned i,uint16_t v){p[i]=(uint8_t)(v>>8);p[i+1]=(uint8_t)v;}
static void big_endian_store_24(uint8_t *p,unsigned i,uint32_t v){p[i]=(uint8_t)(v>>16);p[i+1]=(uint8_t)(v>>8);p[i+2]=(uint8_t)v;}
static void big_endian_store_32(uint8_t *p,unsigned i,uint32_t v){for(unsigned b=0;b<4;++b)p[i+b]=(uint8_t)(v>>(24-8*b));}
static int l2cap_send(uint16_t cid,const uint8_t *p,uint16_t n){assert(cid==9 && n==18);memcpy(wire,p,n);return send_status;}
static void avrcp_request_can_send_now(avrcp_connection_t *c,uint16_t cid){assert(c==&connection && cid==9);++requests;}
static void avrcp_controller_get_capabilities_for_connection(avrcp_connection_t *c,uint8_t id){(void)c;(void)id;}
static void avrcp_controller_emit_notification_complete(avrcp_connection_t *c,uint8_t status,uint8_t event,bool enabled){
 assert(c==&connection && event>=1 && event<=14);
 ++completed_events;completed_event=event;completed_status=status;
 if(!status && enabled)++enabled_events;
 if(status){assert(event==13 && !enabled);++failed_events;}
}
#define AVRCP_CAPABILITY_ID_EVENT 3
#include "volume_sdk.inc"
#define VOLUME_MASK ((uint16_t)(1u<<13))
#define OTHER_MASK ((uint16_t)((1u<<1)|(1u<<3)|(1u<<7)))
static void reset(void){
 memset(&connection,0,sizeof(connection));
 connection.avrcp_cid=7;connection.l2cap_signaling_cid=9;
 connection.remote_capabilities_state=AVRCP_REMOTE_CAPABILITIES_KNOWN;
 connection.notifications_supported_by_target=VOLUME_MASK|OTHER_MASK;
 emitted=enabled_events=requests=failed_events=completed_events=0;
 received_epoch=0;send_status=0;
}
static uint32_t register_volume(void){
 // The real can-send dispatcher removes the queue bit before sending.
 connection.controller_notifications_to_register &= (uint16_t)~VOLUME_MASK;
 assert(avrcp_send_register_notification(&connection,13)==0);
 assert(wire[0]>>4==connection.controller_volume_label && wire[13]==13);
 return avrcp_controller_volume_registration_generation(7);
}
static void notify(uint8_t label,uint8_t type,uint8_t level){
 uint8_t payload[]={13,level};connection.controller_last_confirmed_transaction_id=label;
 avrcp_controller_handle_notification(&connection,type,payload,2);
}
static void reject(uint8_t label){
 uint8_t payload[]={0x03}; // Invalid parameter status, NOT notification event3.
 connection.controller_last_confirmed_transaction_id=label;
 avrcp_controller_handle_notification(&connection,10,payload,1);
}
static void initial_and_fresh_interim(void){
 reset();uint32_t first=register_volume();uint8_t old=connection.controller_volume_label;
 notify(old,15,47);assert(emitted==1 && enabled_events==1 && received_epoch==first && last_event[5]==15 && last_event[6]==47);
 notify(old,15,47);assert(emitted==1);
 notify(old,13,37);assert(emitted==2 && requests==1);
 notify(old,13,37);assert(emitted==2);
 uint32_t second=register_volume();uint8_t current=connection.controller_volume_label;
 notify(old,13,17);notify(old,15,17);assert(emitted==2);
 notify(current,15,37);assert(emitted==3 && enabled_events==1 && received_epoch==second);
 assert(avrcp_controller_refresh_volume_notification(7)==0);
 uint32_t third=register_volume();uint8_t fresh=connection.controller_volume_label;
 assert(avrcp_controller_refresh_volume_notification(7)==ERROR_CODE_COMMAND_DISALLOWED);
 notify(current,15,17);assert(emitted==3); // Old INTERIM is never a fresh query result.
 notify(fresh,15,47);assert(emitted==4 && received_epoch==third && last_event[6]==47);
 notify(current,13,27);assert(emitted==4); // Accepted replacement retires old label.
 notify(fresh,15,47);assert(emitted==4);
 notify(fresh,13,57);assert(emitted==5);
 register_volume();current=connection.controller_volume_label;
 notify(current,13,67);assert(emitted==5);
 uint8_t malformed[]={13};connection.controller_last_confirmed_transaction_id=current;
 avrcp_controller_handle_notification(&connection,15,malformed,1);assert(emitted==5);
 avrcp_controller_handle_notification(&connection,15,malformed,0);assert(emitted==5);
 notify(current,7,90);assert(emitted==5 && !connection.controller_volume_interim);
 notify(current,15,57);assert(emitted==6);
}
static void status_only_rejection(void){
 reset();register_volume();uint8_t current=connection.controller_volume_label;
 connection.notifications_enabled=OTHER_MASK;
 connection.controller_notifications_to_register=OTHER_MASK|VOLUME_MASK;
 connection.controller_notifications_to_deregister=OTHER_MASK;
 connection.controller_initial_status_reported=OTHER_MASK|VOLUME_MASK;
 reject((uint8_t)(current+1));
 assert(!failed_events && connection.notifications_enabled==OTHER_MASK && connection.controller_notifications_to_register==(OTHER_MASK|VOLUME_MASK));
 uint8_t malformed[]={13,3};connection.controller_last_confirmed_transaction_id=current;
 avrcp_controller_handle_notification(&connection,10,malformed,2);assert(!failed_events);
 reject(current);
 assert(failed_events==1 && completed_event==13 && completed_status==ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
 assert(!connection.controller_volume_active && connection.notifications_enabled==OTHER_MASK);
 assert(connection.controller_notifications_to_register==OTHER_MASK && connection.controller_notifications_to_deregister==OTHER_MASK);
 assert(connection.controller_initial_status_reported==OTHER_MASK);
 reject(current);notify(current,15,47);assert(failed_events==1 && !emitted);
}
static void rejected_replacement_keeps_old(bool consume_old){
 reset();uint32_t old_generation=register_volume();uint8_t old=connection.controller_volume_label;notify(old,15,47);
 assert(avrcp_controller_refresh_volume_notification(7)==0);
 uint32_t attempt=register_volume();uint8_t fresh=connection.controller_volume_label;
 connection.notifications_enabled|=OTHER_MASK;
 connection.controller_notifications_to_register|=OTHER_MASK;
 connection.controller_notifications_to_deregister|=OTHER_MASK;
 connection.controller_initial_status_reported|=OTHER_MASK;
 if(consume_old){notify(old,13,37);assert(emitted==2 && received_epoch==old_generation && !connection.controller_volume_previous_active);}
 reject(fresh);
 assert(failed_events==1 && connection.controller_volume_refresh_unsupported);
 assert(avrcp_controller_volume_registration_generation(7)==attempt);
 assert(avrcp_controller_refresh_volume_notification(7)==ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
 assert((connection.notifications_enabled&OTHER_MASK)==OTHER_MASK && (connection.controller_notifications_to_register&OTHER_MASK)==OTHER_MASK);
 assert((connection.controller_notifications_to_deregister&OTHER_MASK)==OTHER_MASK && (connection.controller_initial_status_reported&OTHER_MASK)==OTHER_MASK);
 notify(fresh,15,90);assert(emitted==(consume_old?2u:1u));
 if(!consume_old){
  assert(connection.controller_volume_active && connection.controller_volume_interim && connection.controller_volume_label==old);
  assert(connection.notifications_enabled&VOLUME_MASK);
  notify(old,13,37);assert(emitted==2 && received_epoch==old_generation);
 }else{
  assert(!connection.controller_volume_active);
 }
 assert(connection.controller_notifications_to_register&VOLUME_MASK); // Normal native registration can still continue.
 avrcp_controller_cancel_volume_refresh(7);
 assert(connection.controller_notifications_to_register&VOLUME_MASK); // Cancel cannot eat a native re-register queued by rejection/CHANGED.
 uint32_t normal=register_volume();unsigned count=emitted;
 notify(connection.controller_volume_label,13,37);assert(emitted==count); // Rejection alone never weakens normal INTERIM validation.
 notify(connection.controller_volume_label,15,37);
 assert(received_epoch==normal && connection.controller_volume_active && connection.controller_volume_interim);
}
static void canceled_refresh_keeps_native(unsigned response){
 reset();uint32_t old_generation=register_volume();uint8_t old=connection.controller_volume_label;notify(old,15,47);
 assert(avrcp_controller_refresh_volume_notification(7)==0);uint32_t fresh_generation=register_volume();uint8_t fresh=connection.controller_volume_label;
 avrcp_controller_cancel_volume_refresh(7);
 assert(connection.controller_volume_refresh_unsupported && connection.controller_volume_previous_active);
 assert(avrcp_controller_refresh_volume_notification(7)==ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
 if(response==0){ // Peer retained the old registration: its change starts a normal subscription.
  notify(old,13,37);assert(emitted==2 && received_epoch==old_generation);
  assert(!connection.controller_volume_active && !connection.controller_volume_previous_active);
  notify(fresh,15,90);assert(emitted==2); // Retired query cannot replace native re-registration.
 }else{ // Peer accepted replacement before timeout, but INTERIM was late or lost.
  if(response==1){notify(fresh,15,47);assert(emitted==2 && received_epoch==fresh_generation);}
  notify(fresh,13,37);assert(emitted==(response==1?3u:2u) && received_epoch==fresh_generation);
  notify(old,13,17);assert(emitted==(response==1?3u:2u));
 }
 assert(connection.controller_notifications_to_register&VOLUME_MASK);
 uint32_t normal=register_volume();notify(connection.controller_volume_label,15,37);
 assert(received_epoch==normal && connection.controller_volume_active && connection.controller_volume_interim);
 // Cancellation after fallback cannot kill a native armed subscription.
 uint8_t armed=connection.controller_volume_label;avrcp_controller_cancel_volume_refresh(7);
 assert(connection.controller_volume_active && connection.controller_volume_label==armed);
}
static void old_consumed_then_timeout(void){
 reset();uint32_t old_generation=register_volume();uint8_t old=connection.controller_volume_label;notify(old,15,47);
 assert(avrcp_controller_refresh_volume_notification(7)==0);uint32_t fresh_generation=register_volume();uint8_t fresh=connection.controller_volume_label;
 notify(old,13,37);assert(emitted==2 && received_epoch==old_generation);
 avrcp_controller_cancel_volume_refresh(7);
 notify(fresh,15,37);notify(fresh,13,27);assert(emitted==4 && received_epoch==fresh_generation);
 assert(connection.controller_notifications_to_register&VOLUME_MASK && !connection.controller_volume_previous_active);
}
static void send_failures_and_wrap(void){
 reset();register_volume();uint8_t old=connection.controller_volume_label;notify(old,15,47);
 connection.controller_volume_generation=UINT32_MAX;
 assert(avrcp_controller_refresh_volume_notification(7)==0);assert(register_volume()==1);
 notify(connection.controller_volume_label,15,57);assert(received_epoch==1);
 // A fresh query must not reuse the still-armed label even after unrelated label wrap.
 old=connection.controller_volume_label;
 connection.transaction_id_counter=(uint8_t)(old==1?15:old-1);
 assert(avrcp_controller_refresh_volume_notification(7)==0);register_volume();
 assert(connection.controller_volume_label!=old);
 notify(connection.controller_volume_label,15,57);
 // A failed replacement send preserves the live old registration and its generation.
 old=connection.controller_volume_label;uint32_t armed=connection.controller_volume_active_generation;
 send_status=0x0c;
 assert(avrcp_send_register_notification(&connection,13)==0x0c);
 uint8_t failed_label=(uint8_t)(wire[0]>>4);
 assert(connection.controller_volume_active && connection.controller_volume_label==old);
 notify(failed_label,15,90);assert(emitted==3);
 notify(old,13,37);assert(emitted==4 && received_epoch==armed);
 send_status=0;
 for(unsigned i=0;i<40;++i){
  uint8_t previous=connection.transaction_id_counter;
  register_volume();assert(connection.controller_volume_label>=1 && connection.controller_volume_label<=15);
  assert(connection.controller_volume_label==(previous==15?1:previous+1));
  uint8_t label=connection.controller_volume_label;notify(label,15,47);notify(label,13,37);
 }
 reset();send_status=0x0c;assert(avrcp_send_register_notification(&connection,13)==0x0c);
 notify((uint8_t)(wire[0]>>4),15,90);assert(!emitted && !connection.controller_volume_active);
}
static void unrelated_and_timeout(void){
 reset();
 uint8_t payload[]={1,0}; // Playback-status notification follows the unchanged SDK path.
 connection.controller_last_confirmed_transaction_id=9;
 avrcp_controller_handle_notification(&connection,15,payload,2);
 assert(enabled_events==1 && completed_event==1 && connection.notifications_enabled==(1u<<1));
 avrcp_controller_handle_notification(&connection,15,payload,2);assert(enabled_events==1);
 avrcp_controller_handle_notification(&connection,13,payload,2);
 assert(!(connection.notifications_enabled&(1u<<1)) && (connection.controller_notifications_to_register&(1u<<1)) && requests==1);
 uint16_t queued=connection.controller_notifications_to_register;
 reject(9);assert(!failed_events && queued==connection.controller_notifications_to_register);
 register_volume();uint8_t pending=connection.controller_volume_label;
 avrcp_controller_cancel_volume_refresh(7);notify(pending,15,47);
 assert(connection.controller_volume_active && connection.controller_volume_interim && emitted==1 && (connection.controller_notifications_to_register&(1u<<1)));
 assert(avrcp_controller_refresh_volume_notification(7)==ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
 assert(avrcp_controller_refresh_volume_notification(8)==ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
 avrcp_controller_cancel_volume_refresh(8);
 assert(!avrcp_controller_volume_registration_generation(8) && !avrcp_controller_volume_event_generation(8));
 // A first queued query without an armed subscription stays queued as native fallback.
 reset();assert(avrcp_controller_refresh_volume_notification(7)==0);avrcp_controller_cancel_volume_refresh(7);
 assert(connection.controller_notifications_to_register&VOLUME_MASK);
 register_volume();notify(connection.controller_volume_label,15,47);assert(emitted==1);
 // Canceling a queued-but-unsent refresh also leaves the original armed registration intact.
 reset();register_volume();pending=connection.controller_volume_label;notify(pending,15,47);
 assert(avrcp_controller_refresh_volume_notification(7)==0);avrcp_controller_cancel_volume_refresh(7);
 assert(connection.controller_volume_active && connection.controller_volume_label==pending && !(connection.controller_notifications_to_register&VOLUME_MASK));
 notify(pending,13,37);assert(emitted==2);
}
int main(void){
 initial_and_fresh_interim();status_only_rejection();
 rejected_replacement_keeps_old(false);rejected_replacement_keeps_old(true);
 canceled_refresh_keeps_native(0);canceled_refresh_keeps_native(1);canceled_refresh_keeps_native(2);old_consumed_then_timeout();
 send_failures_and_wrap();unrelated_and_timeout();
 puts("Pinned AVRCP SDK: fresh readbacks, status-only reject, preserved native subscriptions, timeout, labels/generations and send failure passed");
}
