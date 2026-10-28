// SPDX-License-Identifier: GPL-3.0-only
#ifndef PHONE_CALL_TEST_BTSTACK_H
#define PHONE_CALL_TEST_BTSTACK_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef uint8_t bd_addr_t[6];
typedef uint16_t hci_con_handle_t;
typedef void (*btstack_packet_handler_t)(uint8_t,uint16_t,uint8_t*,uint16_t);
typedef struct { uint16_t con_handle; } hci_connection_t;
#define HCI_CON_HANDLE_INVALID 0xffffu
#define BD_ADDR_TYPE_ACL 0
#define LEVEL_2 2
#define HCI_EVENT_PACKET 4
#define HCI_SCO_DATA_PACKET 3
#define HCI_EVENT_SCO_CAN_SEND_NOW 0x6f
#define HCI_EVENT_HFP_META 0xe9
#define ERROR_CODE_SUCCESS 0
#define HFP_CODEC_CVSD 1
#define HFP_CODEC_MSBC 2
#define HFP_HFSF_CODEC_NEGOTIATION 7
#define HFP_HFSF_ESCO_S4 9
#define HFP_HFSF_REMOTE_VOLUME_CONTROL 4
#define HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED 1
#define HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED 2
#define HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED 3
#define HFP_SUBEVENT_AUDIO_CONNECTION_RELEASED 4
#define HFP_SUBEVENT_COMPLETE 5
#define HFP_SUBEVENT_AG_INDICATOR_STATUS_CHANGED 7
#define HFP_SUBEVENT_START_RINGING 10
#define HFP_SUBEVENT_RING 11
#define HFP_SUBEVENT_STOP_RINGING 12
#define HFP_SUBEVENT_CALL_ANSWERED 17
#define HFP_SUBEVENT_CALL_TERMINATED 18
#define HFP_SUBEVENT_SPEAKER_VOLUME 20
#define HFP_SUBEVENT_MICROPHONE_VOLUME 21
#define AVRCP_OPERATION_ID_PLAY 0x44
#define AVRCP_OPERATION_ID_STOP 0x45
#define AVRCP_OPERATION_ID_PAUSE 0x46
#define AVRCP_OPERATION_ID_FORWARD 0x4b
#define SBC_MODE_mSBC 1
static inline uint16_t little_endian_read_16(const uint8_t *p, int o) {return (uint16_t)p[o] | (uint16_t)p[o+1]<<8;}
static inline void little_endian_store_16(uint8_t *p,int o,uint16_t v) {p[o]=(uint8_t)v;p[o+1]=(uint8_t)(v>>8);}
static inline void reverse_bytes(const uint8_t *p,uint8_t *out,unsigned n) {for(unsigned i=0;i<n;++i)out[i]=p[n-1-i];}
uint32_t btstack_run_loop_get_time_ms(void);
void gap_set_security_level(int);
bool bridge_call_open(void);
void bridge_call_stop(void);
hci_connection_t *hci_connection_for_bd_addr_and_type(const uint8_t*,int);
bool hci_can_send_sco_packet_now_for_con_handle(uint16_t);
int hci_get_sco_packet_length_for_connection(uint16_t);
void hci_reserve_packet_buffer(void);
uint8_t *hci_get_outgoing_packet_buffer(void);
uint8_t hci_send_sco_packet_buffer(int);
void hci_request_sco_can_send_now_event_for_con_handle(uint16_t);
void hci_set_sco_voice_setting(uint16_t);
void hci_register_sco_packet_handler(btstack_packet_handler_t);
unsigned de_get_len(const uint8_t*);
uint32_t sdp_create_service_record_handle(void);
uint8_t sdp_register_service(uint8_t*);
#endif
