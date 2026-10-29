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
#define AVDTP_MAX_SEP_NUM 2
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
static struct { unsigned vendor_id, codec_id, codec_type, seid; bool delay_reporting, have_media_codec_capabilities;
                uint8_t media_codec_event[14]; } remote_seps[2];
static uint16_t num_remote_seps=1, selected_remote_sep_index;
static uint16_t peer_cap_len=14;
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
static uint16_t avdtp_subevent_signaling_media_codec_other_capability_get_media_codec_information_len(const uint8_t *event) { (void)event; return peer_cap_len; }
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

static void incoming_eld_configuration_checks_current_endpoint_and_known_peer(void) {
    uint8_t config[14];memcpy(config,media_aaceld_codec_capabilities,sizeof(config));
    num_remote_seps=0; // Peer reconnect can configure before our asynchronous discovery completes.
    assert(playback_eld_peer_configuration(3,4,config,sizeof(config)));
    assert(!playback_eld_peer_configuration(9,4,config,sizeof(config)));
    assert(!playback_eld_peer_configuration(3,0,config,sizeof(config)));
    assert(!playback_eld_peer_configuration(3,0x3f,config,sizeof(config)));
    stream_endpoint_aaceld=NULL;
    assert(!playback_eld_peer_configuration(3,4,config,sizeof(config)));
    stream_endpoint_aaceld=&endpoint;
    num_remote_seps=2;selected_remote_sep_index=0;
    remote_seps[0].seid=8;remote_seps[0].codec_type=0;remote_seps[0].have_media_codec_capabilities=true;
    remote_seps[1]=remote_seps[0];remote_seps[1].seid=4;
    remote_seps[1].codec_type=AVDTP_CODEC_NON_A2DP;
    memcpy(remote_seps[1].media_codec_event,media_aaceld_codec_capabilities,14);
    assert(!playback_eld_peer_configuration(3,8,config,sizeof(config)));
    assert(playback_eld_peer_configuration(3,4,config,sizeof(config))); // Ignore an unrelated selected index.
    remote_seps[1].have_media_codec_capabilities=false;
    assert(playback_eld_peer_configuration(3,4,config,sizeof(config))); // Catalog entry still incomplete.
    remote_seps[1].have_media_codec_capabilities=true;
    remote_seps[1].vendor_id=0;
    assert(!playback_eld_peer_configuration(3,4,config,sizeof(config)));
    remote_seps[1].vendor_id=A2DP_CODEC_VENDOR_ID_APPLE;
    remote_seps[1].media_codec_event[11]&=0x7f;
    assert(!playback_eld_peer_configuration(3,4,config,sizeof(config))); // Known fixed-only peer.
    config[11]&=0x7f;
    assert(playback_eld_peer_configuration(3,4,config,sizeof(config)));
    config[11]|=0x80;remote_seps[1].media_codec_event[11]|=0x80;
    peer_cap_len=13;
    assert(!playback_eld_peer_configuration(3,4,config,sizeof(config)));
    peer_cap_len=14;
    remote_seps[1].media_codec_event[11]=0x81;remote_seps[1].media_codec_event[12]=0xf4;
    assert(!playback_eld_peer_configuration(3,4,config,sizeof(config))); // Config exceeds known peer ceiling.
    num_remote_seps=0;
    media_aaceld_codec_capabilities[11]&=0x7f;
    assert(!playback_eld_peer_configuration(3,4,config,sizeof(config))); // Local offer always applies.
    media_aaceld_codec_capabilities[11]|=0x80;
    assert(!playback_eld_peer_configuration(3,4,config,13));
    assert(!playback_eld_peer_configuration(3,4,NULL,sizeof(config)));
}
static void incoming_configuration_binds_before_open_and_late_catalog_enables_game(void) {
    num_remote_seps=2;remote_seps[0].seid=8;remote_seps[1].seid=4;
    selected_remote_sep_index=0;
    sc.local_stream_endpoint=NULL;media_tracker.local_seid=1;media_tracker.remote_seid=8;
    playback_eld_bind_configuration(3,4);
    assert(sc.local_stream_endpoint==stream_endpoint_aaceld);
    assert(media_tracker.local_seid==3 && media_tracker.remote_seid==4 && selected_remote_sep_index==1);
    num_remote_seps=0;
    playback_eld_bind_configuration(3,9);
    assert(sc.local_stream_endpoint==stream_endpoint_aaceld);
    assert(media_tracker.local_seid==3 && media_tracker.remote_seid==9 && selected_remote_sep_index==AVDTP_MAX_SEP_NUM);
    model_supported=true;
    assert(!playback_game_eligible());
    num_remote_seps=2;remote_seps[1].seid=9;remote_seps[1].delay_reporting=true;
    remote_seps[1].have_media_codec_capabilities=true;
    remote_seps[1].codec_type=AVDTP_CODEC_NON_A2DP;
    remote_seps[1].vendor_id=A2DP_CODEC_VENDOR_ID_APPLE;
    remote_seps[1].codec_id=A2DP_APPLE_CODEC_AAC_ELD;
    assert(playback_game_eligible()); // Uses current remote identity, not the stale sentinel index.
    remote_seps[1].have_media_codec_capabilities=false;assert(!playback_game_eligible());
    remote_seps[1].have_media_codec_capabilities=true;
    remote_seps[1].codec_type=0;assert(!playback_game_eligible());
    remote_seps[1].codec_type=AVDTP_CODEC_NON_A2DP;
    remote_seps[1].delay_reporting=false;assert(!playback_game_eligible());
}

int main(void) {
    remote_seps[0].vendor_id=A2DP_CODEC_VENDOR_ID_APPLE;
    remote_seps[0].codec_id=A2DP_APPLE_CODEC_AAC_ELD;
    remote_seps[0].codec_type=AVDTP_CODEC_NON_A2DP; remote_seps[0].seid=4;
    remote_seps[0].have_media_codec_capabilities=true;
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
    incoming_eld_configuration_checks_current_endpoint_and_known_peer();
    incoming_configuration_binds_before_open_and_late_catalog_enables_game();
    puts("playback delay: negotiated category required for Game Mode");
}
