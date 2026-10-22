#ifndef TEST_PHONE_SINK_BTSTACK_H
#define TEST_PHONE_SINK_BTSTACK_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define ENABLE_AVDTP_ACCEPTOR_EXPLICIT_START_STREAM_CONFIRMATION
#define ENABLE_A2DP_EXPLICIT_CONFIG
#define HCI_EVENT_PACKET 4
#define HCI_EVENT_A2DP_META 0xf0
#define HCI_EVENT_AVDTP_META 0xf1
#define HCI_EVENT_AVRCP_META 0xf2
#define ERROR_CODE_SUCCESS 0
#define ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE 0x11
#define AVDTP_AUDIO 0
#define AVDTP_CODEC_SBC 0
#define AVDTP_CODEC_NON_A2DP 0xff
#define AVDTP_CHANNEL_MODE_STEREO 3
#define AVDTP_CHANNEL_MODE_JOINT_STEREO 4
#define AVDTP_SBC_BLOCK_LENGTH_16 1
#define AVDTP_SBC_SUBBANDS_8 1
#define AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS 1
#define AVDTP_SBC_ALLOCATION_METHOD_SNR 2
#define AVDTP_ERROR_CODE_BAD_ACP_SEID 0x12
#define AVDTP_ERROR_CODE_SEP_IN_USE 0x13
#define AVDTP_ERROR_CODE_UNSUPPORTED_CONFIGURATION 0x29
#define AVDTP_SINK_FEATURE_MASK_HEADPHONE 1
#define AVDTP_SI_SET_CONFIGURATION 3
#define AVDTP_SUBEVENT_SIGNALING_ACCEPT 0x01
#define AVDTP_SUBEVENT_SIGNALING_REJECT 0x02
#define AVDTP_SUBEVENT_SIGNALING_GENERAL_REJECT 0x03
#define AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED 0x04
#define AVDTP_SUBEVENT_SIGNALING_CONNECTION_RELEASED 0x05
#define AVDTP_SUBEVENT_SIGNALING_SEP_FOUND 0x06
#define AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY 0x07
#define AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY 0x0b
#define AVDTP_SUBEVENT_SIGNALING_DELAY_REPORTING_CAPABILITY 0x11
#define AVDTP_SUBEVENT_SIGNALING_CAPABILITIES_DONE 0x13
#define AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION 0x14
#define AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION 0x18
#define AVDTP_SUBEVENT_STREAMING_CONNECTION_ESTABLISHED 0x19
#define AVDTP_SUBEVENT_STREAMING_CONNECTION_RELEASED 0x1a
#define AVDTP_SUBEVENT_SIGNALING_SEP_DICOVERY_DONE 0x1c
#define A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION 0x02
#define A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION 0x06
#define A2DP_SUBEVENT_STREAM_ESTABLISHED 0x07
#define A2DP_SUBEVENT_START_STREAM_REQUESTED 0x08
#define A2DP_SUBEVENT_STREAM_STARTED 0x09
#define A2DP_SUBEVENT_STREAM_SUSPENDED 0x0a
#define A2DP_SUBEVENT_STREAM_STOPPED 0x0b
#define A2DP_SUBEVENT_STREAM_RELEASED 0x0c
#define A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED 0x0f
#define A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED 0x10
#define A2DP_SUBEVENT_COMMAND_REJECTED 0x0e
#define A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY 0x13
#define A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY 0x17
#define A2DP_SUBEVENT_SIGNALING_CAPABILITIES_COMPLETE 0x1b
#define AVRCP_SUBEVENT_CONNECTION_ESTABLISHED 1
#define AVRCP_SUBEVENT_CONNECTION_RELEASED 2
#define AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED 3
#define AVRCP_OPERATION_ID_PLAY 0x44
#define AVRCP_OPERATION_ID_STOP 0x45
#define AVRCP_OPERATION_ID_PAUSE 0x46
#define AVRCP_OPERATION_ID_FORWARD 0x4b
#define AVRCP_OPERATION_ID_BACKWARD 0x4c
#define AVRCP_OPERATION_ID_VOLUME_UP 0x41
#define AVRCP_OPERATION_ID_VOLUME_DOWN 0x42
#define AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED 0x0d
typedef uint8_t bd_addr_t[6];
typedef struct { uint8_t seid; } avdtp_stream_endpoint_t;
typedef struct {
    uint16_t sampling_frequency;
    uint8_t channel_mode, block_length, subbands, allocation_method;
    uint8_t min_bitpool_value, max_bitpool_value;
} avdtp_configuration_sbc_t;
typedef enum { AVDTP_ROLE_SINK } avdtp_role_t;
typedef void (*btstack_packet_handler_t)(uint8_t,uint16_t,uint8_t*,uint16_t);
static inline uint16_t little_endian_read_16(const uint8_t *p,unsigned at) {
    return (uint16_t)(p[at] | ((uint16_t)p[at+1] << 8));
}
static inline void reverse_addr(const uint8_t *p,bd_addr_t address) {
    for(unsigned i=0;i<6;i++) address[i]=p[5-i];
}
static inline void a2dp_subevent_signaling_connection_established_get_bd_addr(const uint8_t*p,bd_addr_t address) {reverse_addr(p+5,address);}
static inline void avdtp_subevent_signaling_connection_established_get_bd_addr(const uint8_t*p,bd_addr_t address) {reverse_addr(p+5,address);}
static inline void a2dp_subevent_stream_established_get_bd_addr(const uint8_t*p,bd_addr_t address) {reverse_addr(p+5,address);}
static inline void avrcp_subevent_connection_established_get_bd_addr(const uint8_t*p,bd_addr_t address) {reverse_addr(p+6,address);}
void a2dp_sink_init(void);
void a2dp_sink_register_packet_handler(btstack_packet_handler_t handler);
void avdtp_sink_register_packet_handler(btstack_packet_handler_t handler);
void a2dp_sink_register_media_handler(void (*handler)(uint8_t,uint8_t*,uint16_t));
void a2dp_sink_register_media_config_validator(uint8_t (*validator)(const avdtp_stream_endpoint_t*,const uint8_t*,uint16_t));
avdtp_stream_endpoint_t *a2dp_sink_create_stream_endpoint(int media,int codec,const uint8_t*caps,uint16_t caplen,uint8_t*config,uint16_t configlen);
void a2dp_sink_finalize_stream_endpoint(avdtp_stream_endpoint_t*endpoint);
uint8_t avdtp_local_seid(avdtp_stream_endpoint_t*endpoint);
void a2dp_sink_create_sdp_record(uint8_t*buffer,uint32_t handle,uint16_t features,const char*name,const char*provider);
uint32_t sdp_create_service_record_handle(void);
void sdp_register_service(const uint8_t*service);
uint8_t a2dp_sink_start_stream_accept(uint16_t cid,uint8_t seid);
uint8_t a2dp_sink_start_stream_reject(uint16_t cid,uint8_t seid);
void a2dp_sink_disconnect(uint16_t cid);
uint8_t a2dp_sink_establish_stream(bd_addr_t address,uint16_t *cid);
uint8_t a2dp_sink_set_config_other(uint16_t cid,uint8_t local,uint8_t remote,const uint8_t *config,uint8_t length);
uint8_t a2dp_sink_set_config_sbc(uint16_t cid,uint8_t local,uint8_t remote,const avdtp_configuration_sbc_t *config);
uint32_t btstack_run_loop_get_time_ms(void);
uint8_t avrcp_controller_play(uint16_t cid);
uint8_t avrcp_controller_pause(uint16_t cid);
uint8_t avrcp_controller_stop(uint16_t cid);
uint8_t avrcp_controller_forward(uint16_t cid);
uint8_t avrcp_controller_backward(uint16_t cid);
uint8_t avrcp_controller_volume_up(uint16_t cid);
uint8_t avrcp_controller_volume_down(uint16_t cid);
uint8_t avrcp_target_support_event(uint16_t cid,uint8_t event);
uint8_t avrcp_target_volume_changed(uint16_t cid,uint8_t volume);
#endif
