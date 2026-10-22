#ifndef TEST_PHONE_CONTROL_BTSTACK_H
#define TEST_PHONE_CONTROL_BTSTACK_H
#include "../aacp_stubs/btstack.h"
#define RFCOMM_DATA_PACKET 7
#define RFCOMM_EVENT_INCOMING_CONNECTION 0x81
#define RFCOMM_EVENT_CHANNEL_OPENED 0x80
#define RFCOMM_EVENT_CHANNEL_CLOSED 0x84
#define RFCOMM_EVENT_CAN_SEND_NOW 0x89
#define LEVEL_2 2
void rfcomm_init(void);
void gap_set_security_level(unsigned level);
uint8_t rfcomm_register_service(btstack_packet_handler_t,uint8_t,uint16_t);
uint8_t rfcomm_accept_connection(uint16_t);
uint8_t rfcomm_decline_connection(uint16_t);
uint8_t rfcomm_disconnect(uint16_t);
uint8_t rfcomm_request_can_send_now_event(uint16_t);
uint8_t rfcomm_send(uint16_t,uint8_t *,uint16_t);
uint32_t sdp_create_service_record_handle(void);
void spp_create_custom_sdp_record(uint8_t *,uint32_t,const uint8_t *,int,const char *);
unsigned de_get_len(const uint8_t *);
uint8_t sdp_register_service(const uint8_t *);
static inline void rfcomm_event_incoming_connection_get_bd_addr(const uint8_t *p,bd_addr_t a){for(unsigned i=0;i<6;i++)a[i]=p[7-i];}
static inline uint8_t rfcomm_event_incoming_connection_get_server_channel(const uint8_t *p){return p[8];}
static inline uint16_t rfcomm_event_incoming_connection_get_rfcomm_cid(const uint8_t *p){return little_endian_read_16(p,9);}
static inline void rfcomm_event_channel_opened_get_bd_addr(const uint8_t *p,bd_addr_t a){for(unsigned i=0;i<6;i++)a[i]=p[8-i];}
static inline uint8_t rfcomm_event_channel_opened_get_status(const uint8_t *p){return p[2];}
static inline uint16_t rfcomm_event_channel_opened_get_rfcomm_cid(const uint8_t *p){return little_endian_read_16(p,12);}
static inline uint16_t rfcomm_event_channel_opened_get_max_frame_size(const uint8_t *p){return little_endian_read_16(p,14);}
static inline uint16_t rfcomm_event_can_send_now_get_rfcomm_cid(const uint8_t *p){return little_endian_read_16(p,2);}
static inline uint16_t rfcomm_event_channel_closed_get_rfcomm_cid(const uint8_t *p){return little_endian_read_16(p,2);}
#endif
