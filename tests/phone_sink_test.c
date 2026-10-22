// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/phone_sink.c"

static btstack_packet_handler_t sink_handler, raw_handler;
static void (*media_handler)(uint8_t,uint8_t*,uint16_t);
static uint8_t (*validator)(const avdtp_stream_endpoint_t*,const uint8_t*,uint16_t);
static avdtp_stream_endpoint_t endpoints[2];
static unsigned endpoint_count, finalized, framework_calls, accepted, rejected, disconnected;
static unsigned opens, claims, stops, pcm_frames, pcm_drops, codec_resets, decode_calls;
static unsigned prepares, closes, controls_sent, volume_updates;
static unsigned aptx_selections, sbc_selections, reconnects;
static uint8_t selected_remote;
static bool aptx_select_ok;
static uint8_t last_control, last_volume;
static uint32_t now;
static bool output_ready, open_allowed, claim_allowed, secure_peer;
static bool aptx_available, prepare_ok, codec_ready, decode_ok, control_busy;
static bool prefix_before_failure;
static bool prepare_waiting, close_waiting;
static phone_codec_t desired;
static uint8_t received_payload[PHONE_CODEC_MAX_PAYLOAD];
static size_t received_length;
static uint8_t received_frames;
static const bd_addr_t phone_address = {1,2,3,4,5,6};
static const bd_addr_t airpods_address = {7,8,9,10,11,12};

void a2dp_sink_init(void) {}
void a2dp_sink_register_packet_handler(btstack_packet_handler_t handler) {sink_handler=handler;}
void avdtp_sink_register_packet_handler(btstack_packet_handler_t handler) {raw_handler=handler;}
void a2dp_sink_register_media_handler(void (*handler)(uint8_t,uint8_t*,uint16_t)) {media_handler=handler;}
void a2dp_sink_register_media_config_validator(uint8_t (*callback)(const avdtp_stream_endpoint_t*,const uint8_t*,uint16_t)) {validator=callback;}
avdtp_stream_endpoint_t *a2dp_sink_create_stream_endpoint(int media,int codec,const uint8_t*caps,uint16_t caplen,uint8_t*config,uint16_t configlen) {
    assert(media==AVDTP_AUDIO && configlen==caplen && config && caps && endpoint_count<2);
    if (codec==AVDTP_CODEC_NON_A2DP) assert(endpoint_count==0 && caplen==7 && caps[6]==0x12);
    else assert(codec==AVDTP_CODEC_SBC && caplen==4 && caps[0]==0x13);
    endpoints[endpoint_count].seid=(uint8_t)(endpoint_count+4);
    return &endpoints[endpoint_count++];
}
void a2dp_sink_finalize_stream_endpoint(avdtp_stream_endpoint_t*endpoint) {assert(endpoint);finalized++;}
uint8_t avdtp_local_seid(avdtp_stream_endpoint_t*endpoint) {return endpoint->seid;}
void a2dp_sink_create_sdp_record(uint8_t*buffer,uint32_t handle,uint16_t features,const char*name,const char*provider) {(void)handle;(void)features;(void)name;(void)provider;buffer[0]=1;}
uint32_t sdp_create_service_record_handle(void) {return 0x10001;}
void sdp_register_service(const uint8_t*service) {assert(service[0]==1);}
uint8_t a2dp_sink_start_stream_accept(uint16_t cid,uint8_t seid) {assert(cid==phone.cid && seid==phone.seid);accepted++;return 0;}
uint8_t a2dp_sink_start_stream_reject(uint16_t cid,uint8_t seid) {assert(cid==phone.cid && seid==phone.seid);rejected++;return 0;}
void a2dp_sink_disconnect(uint16_t cid) {assert(cid);disconnected++;}
uint8_t a2dp_sink_establish_stream(bd_addr_t address,uint16_t *cid) {
    assert(memcmp(address,phone_address,6)==0);*cid=0x41;reconnects++;return 0;
}
uint8_t a2dp_sink_set_config_other(uint16_t cid,uint8_t local,uint8_t remote,const uint8_t *config,uint8_t length) {
    assert(cid==phone.cid && local==aptx_seid && remote && length==7 && memcmp(config,aptx_capabilities,7)==0);
    aptx_selections++;selected_remote=remote;return aptx_select_ok ? 0 : 0x11;
}
uint8_t a2dp_sink_set_config_sbc(uint16_t cid,uint8_t local,uint8_t remote,const avdtp_configuration_sbc_t *config) {
    assert(cid==phone.cid && local==sbc_seid && remote && config->sampling_frequency==48000 &&
        config->block_length==AVDTP_SBC_BLOCK_LENGTH_16 && config->subbands==AVDTP_SBC_SUBBANDS_8 &&
        config->min_bitpool_value>=2 && config->max_bitpool_value<=53);
    sbc_selections++;selected_remote=remote;return 0;
}
uint32_t btstack_run_loop_get_time_ms(void) {return now;}
bool bt_hci_phone_peer_allowed(const uint8_t address[6]) {return memcmp(address,phone_address,6)==0;}
bool bt_hci_phone_peer_connected(const uint8_t address[6]) {return secure_peer && bt_hci_phone_peer_allowed(address);}
bool phone_codec_available(phone_codec_t codec) {return codec==PHONE_CODEC_SBC || (codec==PHONE_CODEC_APTX && aptx_available);}
bool phone_worker_configure(phone_codec_t codec,const phone_sbc_config_t*sbc) {desired=codec;codec_ready=false;if(codec==PHONE_CODEC_SBC)assert(sbc&&sbc->blocks==16&&sbc->subbands==8&&sbc->channel_mode==2);return true;}
phone_worker_status_t phone_worker_prepare(void) {prepares++;if(prepare_waiting)return PHONE_WORKER_WAITING;codec_ready=prepare_ok;return prepare_ok ? PHONE_WORKER_READY : PHONE_WORKER_FAILED;}
bool phone_worker_close(void) {desired=PHONE_CODEC_NONE;codec_ready=false;closes++;return !close_waiting;}
bool phone_worker_released(void) {return !codec_ready && !close_waiting;}
bool phone_worker_poll(phone_codec_pcm_callback callback,void*context) {(void)callback;(void)context;return true;}
bool phone_worker_ready(void) {return codec_ready;}
void phone_worker_reset(void) {codec_resets++;}
bool phone_worker_submit(const uint8_t*payload,size_t length,uint8_t count) {
    decode_calls++;received_length=length;received_frames=count;
    assert(length<=sizeof(received_payload));memcpy(received_payload,payload,length);
    static int16_t pcm[8]={1,2,3,4,5,6,7,8};
    if(decode_ok || prefix_before_failure)decoded_pcm(pcm,4,NULL);
    return decode_ok;
}
bool bridge_phone_open(void) {opens++;return open_allowed;}
bool bridge_phone_ready(void) {return output_ready;}
bool bridge_phone_start(void) {claims++;return claim_allowed;}
void bridge_phone_stop(void) {stops++;}
void bridge_phone_pcm_push(const int16_t*pcm,uint16_t frames) {assert(pcm);pcm_frames+=frames;}
void bridge_phone_drop_pcm(void) {pcm_frames=0;pcm_drops++;}
void bridge_phone_volume(uint8_t volume) {last_volume=volume;volume_updates++;}
uint8_t bridge_phone_get_volume(void) {return 53;}
static uint8_t send(uint16_t cid,uint8_t operation) {assert(cid==phone.avrcp_cid);if(control_busy)return 0x0c;controls_sent++;last_control=operation;return 0;}
uint8_t avrcp_controller_play(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_PLAY);}
uint8_t avrcp_controller_pause(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_PAUSE);}
uint8_t avrcp_controller_stop(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_STOP);}
uint8_t avrcp_controller_forward(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_FORWARD);}
uint8_t avrcp_controller_backward(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_BACKWARD);}
uint8_t avrcp_controller_volume_up(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_VOLUME_UP);}
uint8_t avrcp_controller_volume_down(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_VOLUME_DOWN);}
uint8_t avrcp_target_support_event(uint16_t cid,uint8_t event) {assert(cid==phone.avrcp_cid&&event==AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);return 0;}
uint8_t avrcp_target_volume_changed(uint16_t cid,uint8_t volume) {assert(cid==phone.avrcp_cid);last_volume=volume;volume_updates++;return 0;}
void a2dp_config_process_avdtp_event_handler(avdtp_role_t role,uint8_t*packet,uint16_t size) {
    assert(role==AVDTP_ROLE_SINK);framework_calls++;
    if(packet[2]==AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED){
        packet[0]=HCI_EVENT_A2DP_META;packet[2]=A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED;
        sink_handler(HCI_EVENT_PACKET,0,packet,size);
        packet[0]=HCI_EVENT_AVDTP_META;packet[2]=AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED;
    }
}

static void fixture(bool aptx) {
    memset(&phone,0,sizeof(phone));memset(endpoints,0,sizeof(endpoints));
    initialized=aptx_disabled=retry_sbc=false;aptx_endpoint=sbc_endpoint=NULL;aptx_seid=sbc_seid=0;
    endpoint_count=finalized=framework_calls=accepted=rejected=disconnected=0;
    opens=claims=stops=pcm_frames=pcm_drops=codec_resets=decode_calls=prepares=closes=controls_sent=volume_updates=0;
    now=0;output_ready=false;open_allowed=claim_allowed=secure_peer=prepare_ok=decode_ok=true;
    codec_ready=control_busy=prefix_before_failure=false;aptx_available=aptx;desired=PHONE_CODEC_NONE;
    prepare_waiting=close_waiting=false;
    aptx_selections=sbc_selections=reconnects=0;selected_remote=0;aptx_select_ok=true;
    assert(phone_sink_init());
}
static void address_event(uint8_t*p,unsigned offset,const bd_addr_t address) {for(unsigned i=0;i<6;i++)p[offset+i]=address[5-i];}
static void signaling(uint16_t cid,const bd_addr_t address) {
    uint8_t p[14]={HCI_EVENT_AVDTP_META,12,AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED,(uint8_t)cid,(uint8_t)(cid>>8)};
    address_event(p,5,address);raw_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void stream_event(uint8_t event,uint8_t seid) {
    uint8_t p[6]={HCI_EVENT_A2DP_META,4,event,(uint8_t)phone.cid,(uint8_t)(phone.cid>>8),seid};
    sink_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void sbc_event(uint8_t*p) {
    uint8_t event[18]={HCI_EVENT_A2DP_META,16,A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION,
        (uint8_t)phone.cid,(uint8_t)(phone.cid>>8),sbc_seid,1,0,AVDTP_AUDIO,0x80,0xbb,3,2,16,8,1,2,53};
    memcpy(p,event,sizeof(event));
}
static void configure(bool aptx) {
    uint8_t p[20];
    if(aptx){
        uint8_t event[20]={HCI_EVENT_A2DP_META,18,A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION,
            (uint8_t)phone.cid,(uint8_t)(phone.cid>>8),aptx_seid,1,0,AVDTP_AUDIO,0xff,0,7,0};
        memcpy(p,event,sizeof(event));memcpy(p+13,aptx_capabilities,7);
        assert(validator(aptx_endpoint,p,20)==0);sink_handler(HCI_EVENT_PACKET,0,p,20);
    }else{
        sbc_event(p);assert(validator(sbc_endpoint,p,18)==0);sink_handler(HCI_EVENT_PACKET,0,p,18);
    }
    assert(phone.configured);phone_sink_poll();assert(codec_ready);
}
static void remote_sbc(uint8_t rate) {
    uint8_t p[14]={HCI_EVENT_A2DP_META,12,A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY,
        (uint8_t)phone.cid,(uint8_t)(phone.cid>>8),11,AVDTP_AUDIO,rate,3,1,1,3,2,53};
    sink_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void remote_aptx(uint8_t rate_and_channels) {
    uint8_t p[18]={HCI_EVENT_A2DP_META,16,A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY,
        (uint8_t)phone.cid,(uint8_t)(phone.cid>>8),12,AVDTP_AUDIO,0xff,0,7,0};
    memcpy(p+11,aptx_capabilities,7);p[17]=rate_and_channels;
    sink_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void capabilities_complete(void) {
    uint8_t p[6]={HCI_EVENT_A2DP_META,4,A2DP_SUBEVENT_SIGNALING_CAPABILITIES_COMPLETE,
        (uint8_t)phone.cid,(uint8_t)(phone.cid>>8),0xa5};
    sink_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
    // Configuration must wait until the framework finishes this callback.
    assert(aptx_selections==0 && sbc_selections==0);
    phone_sink_poll();
}
static void opened(void) {
    uint8_t p[14]={HCI_EVENT_A2DP_META,12,A2DP_SUBEVENT_STREAM_ESTABLISHED,
        (uint8_t)phone.cid,(uint8_t)(phone.cid>>8)};
    address_event(p,5,phone_address);p[11]=phone.seid;p[12]=1;
    sink_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void playing(bool aptx) {
    signaling(0x40,phone_address);configure(aptx);opened();
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);
    output_ready=true;phone_sink_poll();assert(accepted==1);
    stream_event(A2DP_SUBEVENT_STREAM_STARTED,phone.seid);assert(phone_sink_active());
}
static unsigned rtp(uint8_t*p,uint16_t sequence) {
    memset(p,0,32);p[0]=0x80;p[1]=96;p[2]=(uint8_t)(sequence>>8);p[3]=(uint8_t)sequence;p[11]=1;
    p[12]=1;p[13]=0x9c;p[14]=0x77;return 15;
}
static void test_roles_and_start_priming(void) {
    fixture(true);assert(endpoint_count==2 && aptx_seid<sbc_seid);
    signaling(0x30,airpods_address);assert(framework_calls==0 && !phone_sink_connected());
    signaling(0x40,phone_address);assert(framework_calls==1 && phone.cid==0x40);
    configure(false);opened();assert(opens==1 && !phone_sink_active());
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);
    for(unsigned i=0;i<20;i++)phone_sink_poll();
    assert(accepted==0 && claims==0 && phone.start_pending);
    output_ready=true;phone_sink_poll();assert(accepted==1 && claims==1 && phone.start_accepted);
    uint8_t packet[32];unsigned size=rtp(packet,1);
    media_handler(phone.seid,packet,size);assert(decode_calls==0);
    stream_event(A2DP_SUBEVENT_STREAM_STARTED,phone.seid);
    media_handler(phone.seid,packet,size);assert(decode_calls==1 && pcm_frames==4);
    stream_event(A2DP_SUBEVENT_STREAM_SUSPENDED,phone.seid);assert(!phone_sink_active() && phone_sink_owns_controls());
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);phone_sink_poll();assert(accepted==2);
    stream_event(A2DP_SUBEVENT_STREAM_STARTED,phone.seid);
    stream_event(A2DP_SUBEVENT_STREAM_STOPPED,phone.seid);assert(!phone_sink_owns_controls());
}
static void test_configuration_and_security(void) {
    fixture(true);signaling(0x40,phone_address);uint8_t p[20];sbc_event(p);
    assert(validator(sbc_endpoint,p,18)==0);
    const unsigned bad[]={3,5,9,11,12,13,14,15,16,17};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){
        uint8_t saved=p[bad[i]];p[bad[i]]=0;
        assert(validator(sbc_endpoint,p,18)!=0);p[bad[i]]=saved;
    }
    for(unsigned n=0;n<18;n++)assert(validator(sbc_endpoint,p,n)!=0);
    configure(true);secure_peer=false;opened();assert(!phone.opened && disconnected==1);
}
static void test_codec_selection(void) {
    fixture(true);signaling(0x40,phone_address);
    remote_sbc(1);remote_aptx(0x32);capabilities_complete();
    assert(aptx_selections==1 && sbc_selections==0 && selected_remote==12 && phone.requested_codec==PHONE_CODEC_APTX);
    configure(true); // inbound SET_CONFIG remains accepted independently of our discovery preference

    fixture(true);signaling(0x40,phone_address);
    remote_aptx(0x22);remote_sbc(1);capabilities_complete(); // aptX 44.1 kHz only
    assert(aptx_selections==0 && sbc_selections==1 && selected_remote==11);
    configure(false);

    fixture(false);signaling(0x40,phone_address);
    remote_aptx(0x12);remote_sbc(1);capabilities_complete();
    assert(aptx_selections==0 && sbc_selections==1 && endpoint_count==1);

    fixture(true);signaling(0x40,phone_address);
    remote_aptx(0x32);remote_sbc(2);aptx_select_ok=false;capabilities_complete();
    assert(aptx_selections==1 && sbc_selections==0 && disconnected==1); // SBC 44.1 kHz also unsupported

    fixture(true);signaling(0x40,phone_address);
    remote_aptx(0x32);remote_sbc(1);aptx_select_ok=false;capabilities_complete();
    assert(aptx_selections==1 && sbc_selections==1 && phone.requested_codec==PHONE_CODEC_SBC);

    fixture(true);signaling(0x40,phone_address);
    remote_sbc(1);remote_aptx(0x12);capabilities_complete();
    uint8_t reject[8]={HCI_EVENT_A2DP_META,6,A2DP_SUBEVENT_COMMAND_REJECTED,0x40,0,aptx_seid,1,AVDTP_SI_SET_CONFIGURATION};
    sink_handler(HCI_EVENT_PACKET,0,reject,8);
    assert(disconnected==1 && aptx_disabled && retry_sbc && phone.disconnecting);
    uint8_t release[5]={HCI_EVENT_A2DP_META,3,A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED,0x40,0};
    sink_handler(HCI_EVENT_PACKET,0,release,5);phone_sink_poll();
    assert(reconnects==1 && finalized==1 && aptx_seid==0);
    signaling(0x41,phone_address);remote_sbc(1);aptx_selections=sbc_selections=0;capabilities_complete();
    assert(aptx_selections==0 && sbc_selections==1);
}
static void test_raw_boundaries_and_yield(void) {
    fixture(true);signaling(0x40,phone_address);
    uint8_t p[18]={HCI_EVENT_AVDTP_META,16,AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY,0x40,0,1,0,0xff,0,7,0};
    unsigned before=framework_calls;
    for(unsigned n=3;n<18;n++){p[1]=(uint8_t)(n-2);raw_handler(HCI_EVENT_PACKET,0,p,n);}
    assert(framework_calls==before);
    p[1]=16;raw_handler(HCI_EVENT_PACKET,0,p,18);assert(framework_calls==before+1);
    p[9]=8;raw_handler(HCI_EVENT_PACKET,0,p,18);assert(framework_calls==before+1);
    p[9]=7;p[3]=0x30;raw_handler(HCI_EVENT_PACKET,0,p,18);assert(framework_calls==before+1);
    configure(true);opened();assert(codec_ready && phone.configured && !phone.started);
    unsigned closed_before=closes;
    phone_sink_yield();
    assert(closes==closed_before+1 && !codec_ready && !phone.configured && phone.disconnecting && disconnected==1 && !phone_sink_owns_controls());
    uint8_t config[18];sbc_event(config);assert(validator(sbc_endpoint,config,18)!=0);
    sink_handler(HCI_EVENT_PACKET,0,config,18);phone_sink_poll();
    assert(!phone.configured && !phone.started && disconnected==1 && desired==PHONE_CODEC_NONE);
}
static void test_aptx_raw_and_fallback(void) {
    fixture(true);playing(true);
    uint8_t raw[]={0x13,0x24,0x35,0x46,0x57,0x68,0x79,0x8a};
    media_handler(phone.seid,raw,sizeof(raw));
    assert(decode_calls==1 && received_length==8 && received_frames==0 && memcmp(raw,received_payload,8)==0);
    media_handler(phone.seid,raw,7);assert(decode_calls==1 && pcm_drops==1);
    fixture(false);assert(endpoint_count==1 && aptx_seid==0);playing(false);assert(desired==PHONE_CODEC_SBC);
    fixture(true);signaling(0x40,phone_address);configure(true);codec_ready=false;prepare_ok=false;
    phone_sink_poll();assert(aptx_disabled && disconnected==1 && finalized==0);
    uint8_t release[5]={HCI_EVENT_A2DP_META,3,A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED,0x40,0};
    sink_handler(HCI_EVENT_PACKET,0,release,5);phone_sink_poll();assert(finalized==1 && aptx_seid==0);
    assert(reconnects==1);phone_sink_poll();assert(reconnects==1);
}
static void test_worker_wait_and_release(void) {
    fixture(true);signaling(0x40,phone_address);configure(true);opened();
    codec_ready=false;prepare_waiting=true;output_ready=true;
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);
    phone_sink_poll();
    assert(phone.configured && phone.start_pending && !aptx_disabled);
    assert(accepted==0 && rejected==0 && disconnected==0);
    prepare_waiting=false;phone_sink_poll();assert(accepted==1);
    close_waiting=true;phone_sink_yield();
    assert(!phone_sink_decoder_released() && !phone_sink_active());
    phone_sink_poll();assert(!phone_sink_decoder_released() && disconnected==1);
    close_waiting=false;phone_sink_poll();assert(phone_sink_decoder_released());
}
static void test_rtp_bounds_sequence_and_faults(void) {
    fixture(true);playing(false);uint8_t p[64];unsigned size=rtp(p,65535);
    for(unsigned n=0;n<12;n++)media_handler(phone.seid,p,n);
    assert(decode_calls==0);
    media_handler(phone.seid,p,size);assert(decode_calls==1 && received_frames==1 && received_length==2);
    media_handler(phone.seid,p,size);assert(decode_calls==1); // duplicate
    rtp(p,0);media_handler(phone.seid,p,size);assert(decode_calls==2 && phone.sequence_gaps==0);
    rtp(p,2);media_handler(phone.seid,p,size);assert(decode_calls==3 && phone.sequence_gaps==1);
    p[12]=0x81;media_handler(phone.seid,p,size);assert(decode_calls==3);
    rtp(p,3);p[0]=0x9f;media_handler(phone.seid,p,size);assert(decode_calls==3); // truncated CSRC
    rtp(p,3);p[0]=0x90;media_handler(phone.seid,p,size);assert(decode_calls==3); // truncated extension
    rtp(p,3);p[0]=0xa0;p[size-1]=0;media_handler(phone.seid,p,size);assert(decode_calls==3); // padding=0
    rtp(p,3);p[0]=0xa0;p[size-1]=16;media_handler(phone.seid,p,size);assert(decode_calls==3);
    rtp(p,3);decode_ok=false;prefix_before_failure=true;media_handler(phone.seid,p,size);
    assert(decode_calls==4 && pcm_frames==0); // a bad packet cannot leave its decoded prefix queued
    decode_ok=true;prefix_before_failure=false;
    memset(p,0,sizeof(p));p[0]=0xb2;p[1]=96;p[3]=4;p[11]=1;
    p[23]=1;p[28]=1;p[29]=0x9c;p[30]=0x77;p[32]=2;
    media_handler(phone.seid,p,33); // two CSRCs, one extension word, two padding bytes
    assert(decode_calls==5 && received_length==2 && received_payload[0]==0x9c);
    media_handler((uint8_t)(phone.seid+1),p,33);assert(decode_calls==5);
    uint8_t configuration[18];sbc_event(configuration);
    assert(validator(sbc_endpoint,configuration,sizeof(configuration))==AVDTP_ERROR_CODE_SEP_IN_USE);
    output_ready=false;phone_sink_poll();assert(!phone_sink_active() && disconnected==1);
}
static void test_timeouts_and_stale_events(void) {
    fixture(true);signaling(0x40,phone_address);configure(false);opened();
    now=UINT32_MAX-999;stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);
    now=2000;phone_sink_poll();assert(rejected==1 && accepted==0);
    fixture(true);playing(false);
    uint8_t stale[6]={HCI_EVENT_A2DP_META,4,A2DP_SUBEVENT_STREAM_SUSPENDED,0x41,0,phone.seid};
    sink_handler(HCI_EVENT_PACKET,0,stale,sizeof(stale));assert(phone.started);
    stream_event(A2DP_SUBEVENT_STREAM_SUSPENDED,phone.seid);
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);phone_sink_poll();
    now=PHONE_START_TIMEOUT_MS;phone_sink_poll();assert(disconnected==1 && !phone_sink_active());
}
static void test_avrcp_ownership_controls_and_volume(void) {
    fixture(true);playing(false);
    uint8_t p[14]={HCI_EVENT_AVRCP_META,12,AVRCP_SUBEVENT_CONNECTION_ESTABLISHED,0,0x50,0};
    address_event(p,6,airpods_address);assert(!phone_sink_handle_avrcp(HCI_EVENT_PACKET,0,p,14));
    address_event(p,6,phone_address);assert(phone_sink_handle_avrcp(HCI_EVENT_PACKET,0,p,14));
    assert(phone.avrcp_cid==0x50);
    assert(phone_sink_media_control(AVRCP_OPERATION_ID_PAUSE));phone_sink_poll();
    assert(controls_sent==1 && last_control==AVRCP_OPERATION_ID_PAUSE);
    stream_event(A2DP_SUBEVENT_STREAM_SUSPENDED,phone.seid);
    assert(phone_sink_owns_controls() && phone_sink_media_control(AVRCP_OPERATION_ID_PLAY));phone_sink_poll();
    assert(controls_sent==2 && last_control==AVRCP_OPERATION_ID_PLAY);
    uint8_t paused_volume[7]={HCI_EVENT_AVRCP_META,5,AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED,0x50,0,0,66};
    assert(phone_sink_handle_target(HCI_EVENT_PACKET,0,paused_volume,7));assert(last_volume==66);
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);phone_sink_poll();
    stream_event(A2DP_SUBEVENT_STREAM_STARTED,phone.seid);
    control_busy=true;
    for(unsigned i=0;i<8;i++)assert(phone_sink_media_control(AVRCP_OPERATION_ID_FORWARD));
    assert(phone.control_count==PHONE_CONTROL_QUEUE);phone_sink_poll();assert(controls_sent==2);
    control_busy=false;phone_sink_poll();assert(controls_sent==3);
    uint8_t volume[7]={HCI_EVENT_AVRCP_META,5,AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED,0x50,0,0,88};
    assert(phone_sink_handle_target(HCI_EVENT_PACKET,0,volume,7));assert(last_volume==88);
    unsigned before=volume_updates;phone_sink_volume_changed(88);assert(volume_updates==before);
    volume[3]=0x51;assert(!phone_sink_handle_target(HCI_EVENT_PACKET,0,volume,7));
    uint8_t release[5]={HCI_EVENT_AVRCP_META,3,AVRCP_SUBEVENT_CONNECTION_RELEASED,0x50,0};
    assert(phone_sink_handle_avrcp(HCI_EVENT_PACKET,0,release,5));assert(phone.control_count==0 && phone.avrcp_cid==0);
}
int main(void) {
    test_roles_and_start_priming();test_configuration_and_security();test_aptx_raw_and_fallback();
    test_codec_selection();test_raw_boundaries_and_yield();test_worker_wait_and_release();
    test_rtp_bounds_sequence_and_faults();test_timeouts_and_stale_events();test_avrcp_ownership_controls_and_volume();
    puts("phone sink: role isolation, aptX/SBC, primed Start, packet bounds, loss, cancellation and AVRCP passed");
}
