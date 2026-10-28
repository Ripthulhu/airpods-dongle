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
static unsigned emitted,enabled_events,requests,failed_events,completed_events,wire_sends;
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
static int l2cap_send(uint16_t cid,const uint8_t *p,uint16_t n){assert(cid==9 && n==18);memcpy(wire,p,n);++wire_sends;return send_status;}
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
 emitted=enabled_events=requests=failed_events=completed_events=wire_sends=0;
 received_epoch=0;send_status=0;
}
static uint32_t register_volume(void){
 // The real can-send dispatcher removes the queue bit before sending.
 connection.controller_notifications_to_register &= (uint16_t)~VOLUME_MASK;
 assert(avrcp_send_register_notification(&connection,13)==0);
 assert(wire[0]>>4==connection.controller_volume_label && wire[13]==13);
 return connection.controller_volume_generation;
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
static void native_registration_flow(void){
 reset();uint32_t first=register_volume();uint8_t old=connection.controller_volume_label;
 assert(wire_sends==1 && !emitted);
 // A second normal request cannot replace one still awaiting INTERIM.
 assert(avrcp_send_register_notification(&connection,13)==ERROR_CODE_COMMAND_DISALLOWED && wire_sends==1);
 notify(old,13,37);assert(!emitted); // CHANGED before INTERIM is invalid.
 notify(old,15,47);assert(emitted==1 && enabled_events==1 && received_epoch==first && last_event[5]==15 && last_event[6]==47);
 notify(old,15,47);assert(emitted==1);
 notify(old,13,37);assert(emitted==2 && requests==1 && (connection.controller_notifications_to_register&VOLUME_MASK));
 notify(old,13,37);notify(old,15,47);assert(emitted==2);
 uint32_t second=register_volume();uint8_t current=connection.controller_volume_label;
 notify(old,13,17);notify(old,15,17);notify(current,13,27);assert(emitted==2);
 uint8_t malformed[]={13};connection.controller_last_confirmed_transaction_id=current;
 avrcp_controller_handle_notification(&connection,15,malformed,1);
 avrcp_controller_handle_notification(&connection,15,malformed,0);
 notify(current,7,90);assert(emitted==2 && !connection.controller_volume_interim);
 // Initial-report suppression must not hide the re-registration's current value.
 notify(current,15,37);assert(emitted==3 && enabled_events==1 && received_epoch==second && last_event[6]==37);
 notify(current,15,37);assert(emitted==3);
 notify(current,13,27);assert(emitted==4 && received_epoch==second);
 uint32_t third=register_volume();notify(connection.controller_volume_label,15,27);
 assert(emitted==5 && received_epoch==third && wire_sends==3);
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
 // A stale rejected label cannot retire a newer normal registration.
 register_volume();uint8_t next=connection.controller_volume_label;
 reject(current);assert(failed_events==1 && connection.controller_volume_active);
 notify(next,15,47);assert(emitted==1);
 reject(next);assert(failed_events==1 && connection.controller_volume_active); // Already-armed registration is not a pending command.
}
static void send_failure_and_wrap(void){
 reset();connection.controller_volume_generation=UINT32_MAX;
 assert(register_volume()==1);notify(connection.controller_volume_label,15,57);assert(received_epoch==1);
 notify(connection.controller_volume_label,13,47);
 send_status=0x0c;
 assert(avrcp_send_register_notification(&connection,13)==0x0c);
 uint8_t failed=connection.controller_volume_label;
 notify(failed,15,90);assert(emitted==2 && !connection.controller_volume_active);
 send_status=0;
 for(unsigned i=0;i<40;++i){
  uint8_t previous=connection.transaction_id_counter;
  uint32_t epoch=register_volume();
  assert(connection.controller_volume_label==(previous==15?1:previous+1));
  uint8_t label=connection.controller_volume_label;notify(label,15,47);assert(received_epoch==epoch);
  notify(label,13,37);notify(label,13,37); // Duplicate consumed response remains inert across wrap.
 }
 assert(emitted==82);
 assert(!avrcp_controller_volume_event_generation(8));
}
static void unrelated_notifications(void){
 reset();uint8_t payload[]={1,0};
 connection.controller_last_confirmed_transaction_id=9;
 avrcp_controller_handle_notification(&connection,15,payload,2);
 assert(enabled_events==1 && completed_event==1 && connection.notifications_enabled==(1u<<1));
 avrcp_controller_handle_notification(&connection,15,payload,2);assert(enabled_events==1);
 avrcp_controller_handle_notification(&connection,13,payload,2);
 assert(!(connection.notifications_enabled&(1u<<1)) && (connection.controller_notifications_to_register&(1u<<1)) && requests==1);
 uint16_t queued=connection.controller_notifications_to_register;
 reject(9);assert(!failed_events && queued==connection.controller_notifications_to_register);
 register_volume();uint8_t current=connection.controller_volume_label;
 reject((uint8_t)(current+1));assert(!failed_events && queued==connection.controller_notifications_to_register);
 reject(current);assert(failed_events==1 && queued==connection.controller_notifications_to_register);
 // Normal explicit disable behavior remains unchanged after CHANGED.
 reset();register_volume();current=connection.controller_volume_label;notify(current,15,47);
 connection.controller_notifications_to_deregister=VOLUME_MASK;
 notify(current,13,37);
 assert(!connection.notifications_enabled && !connection.controller_notifications_to_deregister && !connection.controller_notifications_to_register);
 assert(completed_event==13 && completed_status==0 && !requests);
}
int main(void){
 native_registration_flow();status_only_rejection();send_failure_and_wrap();unrelated_notifications();
 puts("Pinned AVRCP SDK: normal CHANGED/re-registration, fresh INTERIM, real reject, stale labels, send failure, wrap and unrelated events passed");
}
