// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/playback_options.h"

#define A2DP_CODEC_VENDOR_ID_APPLE 0x4c
#define A2DP_APPLE_CODEC_AAC_ELD 0x8001
#define AVDTP_CODEC_NON_A2DP 0xff
#define AVDTP_AUDIO 0
#define AVDTP_MEDIA_CODEC 7
#define AVDTP_DELAY_REPORTING 8
#define btstack_assert assert
typedef unsigned avdtp_media_codec_type_t;
typedef struct {
    struct { unsigned media_type, media_codec_type, media_codec_information_len;
             uint8_t *media_codec_information; } media_codec;
} avdtp_capabilities_t;
typedef struct {
    uint8_t *media_codec_configuration_info;
    unsigned media_codec_configuration_len;
    uint16_t remote_configuration_bitmap;
    avdtp_capabilities_t remote_configuration;
} endpoint_t;
static endpoint_t endpoint, *stream_endpoint_aaceld;
static struct { endpoint_t *local_stream_endpoint; } sc;
static struct { unsigned avdtp_cid, local_seid, remote_seid; } media_tracker={2,0,0};
static struct { unsigned vendor_id, codec_id, codec_type, seid; bool delay_reporting;
                uint8_t media_codec_event[14]; } remote_seps[1];
static uint16_t num_remote_seps=1, selected_remote_sep_index;
static uint8_t cur_codec=4;
static bool a2dp_is_connected_flag=true, model_supported=true;
static uint8_t media_aaceld_codec_capabilities[14]={0x4c,0,0,0,1,0x80,0,0x80,0,0x84,0,0x83,0xe8,0};
static uint8_t local_stream_endpoint_aaceld_media_codec_configuration[14], media_codec_config_data[14];
static unsigned media_codec_config_len;
static playback_options_t playback_options;
static uint16_t configured_services;
static playback_game_t playback_game;
static uint32_t delay_revision;
static uint16_t reported_delay;
static uint32_t btstack_run_loop_get_time_ms(void) { return 100; }
static uint32_t aacp_receive_buffer_serial(void) { return 0; }
static uint16_t aacp_receive_buffer_last(void) { return 0; }
static uint16_t avdtp_subevent_signaling_delay_report_get_avdtp_cid(const uint8_t *p) { return p[3]|(uint16_t)p[4]<<8; }
static uint8_t avdtp_subevent_signaling_delay_report_get_local_seid(const uint8_t *p) { return p[5]; }
static uint16_t avdtp_subevent_signaling_delay_report_get_delay_100us(const uint8_t *p) { return p[6]|(uint16_t)p[7]<<8; }
static endpoint_t *a2dp_source_create_stream_endpoint(unsigned type, unsigned codec,
        uint8_t *cap, unsigned cap_len, uint8_t *config, unsigned config_len) {
    assert(type==AVDTP_AUDIO && codec==AVDTP_CODEC_NON_A2DP);
    assert(cap && config && cap_len==14 && config_len==14); return &endpoint;
}
static void avdtp_source_register_delay_reporting_category(unsigned seid) { assert(seid==3); }
static unsigned avdtp_local_seid(endpoint_t *ep) { assert(ep==&endpoint); return 3; }
static uint16_t store_bit16(uint16_t bits, unsigned bit, unsigned value) {
    return value ? bits | (1u<<bit) : bits & ~(1u<<bit);
}
static const uint8_t *avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information(const uint8_t *event) { return event; }
static uint16_t avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information_len(const uint8_t *event) { (void)event; return 14; }
static int avdtp_source_set_configuration(unsigned cid, unsigned local, unsigned remote,
        uint16_t services, avdtp_capabilities_t configuration) {
    assert(cid==2 && local==3 && remote==4);
    assert(configuration.media_codec.media_codec_information_len==14);
    assert(playback_eld_configuration(configuration.media_codec.media_codec_information,14));
    configured_services=endpoint.remote_configuration_bitmap=services;
    return 0;
}
static bool aacp_model_game_supported(void) { return model_supported; }
#include "playback_delay.inc"
static void receive_report(uint8_t cid, uint8_t seid, uint16_t size) {
    uint8_t bytes[8]={0,6,0,cid,0,seid,0x58,2};
    const uint8_t *packet=bytes;
    switch (0) {
        case 0:
#include "playback_delay_report.inc"
        default: break;
    }
}

int main(void) {
    remote_seps[0].vendor_id=A2DP_CODEC_VENDOR_ID_APPLE;
    remote_seps[0].codec_id=A2DP_APPLE_CODEC_AAC_ELD;
    remote_seps[0].codec_type=AVDTP_CODEC_NON_A2DP; remote_seps[0].seid=4;
    memcpy(remote_seps[0].media_codec_event,media_aaceld_codec_capabilities,14);
    assert(setup_aaceld_configuration()==0);
    assert(configured_services==(1u<<AVDTP_MEDIA_CODEC));
    assert(!playback_game_eligible());
    remote_seps[0].delay_reporting=true;
    assert(!playback_game_eligible()); // Advertising alone does not negotiate reporting.
    assert(setup_aaceld_configuration()==0);
    assert(configured_services==((1u<<AVDTP_MEDIA_CODEC)|(1u<<AVDTP_DELAY_REPORTING)));
    assert(playback_game_eligible());
    model_supported=false; assert(!playback_game_eligible());
    receive_report(9,3,8); assert(delay_revision==0);
    receive_report(2,9,8); assert(delay_revision==0);
    receive_report(2,3,7); assert(delay_revision==0);
    receive_report(2,3,8); assert(delay_revision==1 && reported_delay==600);
    puts("playback delay: negotiated category required for Game Mode");
}
