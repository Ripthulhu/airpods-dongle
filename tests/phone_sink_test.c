// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#define AVDTP_SI_OPEN 6 // Pinned BTstack signal identifier, absent from the minimal stub.
#include "../src/btstack/phone_sink.c"

static btstack_packet_handler_t sink_handler, raw_handler;
static void (*media_handler)(uint8_t,uint8_t*,uint16_t);
static uint8_t (*validator)(const avdtp_stream_endpoint_t*,const uint8_t*,uint16_t);
static avdtp_stream_endpoint_t endpoints[3];
static unsigned endpoint_count, finalized, framework_calls, accepted, rejected, disconnected;
static unsigned opens, claims, stops, pcm_frames, pcm_drops, codec_resets, decode_calls;
static unsigned prepares, closes, controls_sent, volume_updates;
static unsigned lhdc_selections, aptx_selections, sbc_selections, reconnects;
static uint8_t selected_remote;
static bool lhdc_select_ok, aptx_select_ok, lhdc_create_ok;
static uint8_t last_control, last_volume;
static uint32_t now;
static bool output_ready, output_recovering, open_allowed, claim_allowed, secure_peer;
static bool lhdc_available, aptx_available, prepare_ok, codec_ready, decode_ok, control_busy;
static bool prepare_waiting, close_waiting;
static bool async_worker, worker_pending, submit_ok;
static bool confirmed_volume_known, volume_request_ok, target_volume_busy, target_support_busy;
static uint8_t confirmed_volume, last_bridge_volume, target_volume;
static unsigned bridge_volume_attempts, bridge_volume_requests, target_volume_attempts, target_support_attempts;
static bool target_volume_initialized;
static unsigned volume_cancels;
static bool accept_ok;
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
    assert(media==AVDTP_AUDIO && configlen==caplen && config && caps && endpoint_count<3);
    if (codec==AVDTP_CODEC_NON_A2DP && caplen==11) {
        const uint8_t expected[]={0x3a,5,0,0,0x35,0x4c,0x10,0x92,0x11,0,0};
        assert(endpoint_count==0 && memcmp(caps,expected,sizeof(expected))==0);
        if (!lhdc_create_ok) return NULL;
    } else if (codec==AVDTP_CODEC_NON_A2DP) assert(caplen==7 && caps[6]==0x12);
    else assert(codec==AVDTP_CODEC_SBC && caplen==4 && caps[0]==0x13);
    endpoints[endpoint_count].seid=(uint8_t)(endpoint_count+4);
    return &endpoints[endpoint_count++];
}
void a2dp_sink_finalize_stream_endpoint(avdtp_stream_endpoint_t*endpoint) {assert(endpoint);finalized++;}
uint8_t avdtp_local_seid(avdtp_stream_endpoint_t*endpoint) {return endpoint->seid;}
void a2dp_sink_create_sdp_record(uint8_t*buffer,uint32_t handle,uint16_t features,const char*name,const char*provider) {(void)handle;(void)features;(void)name;(void)provider;buffer[0]=1;}
uint32_t sdp_create_service_record_handle(void) {return 0x10001;}
void sdp_register_service(const uint8_t*service) {assert(service[0]==1);}
uint8_t a2dp_sink_start_stream_accept(uint16_t cid,uint8_t seid) {assert(cid==phone.cid && seid==phone.seid);accepted++;return accept_ok ? 0 : 0x0c;}
uint8_t a2dp_sink_start_stream_reject(uint16_t cid,uint8_t seid) {assert(cid==phone.cid && seid==phone.seid);rejected++;return 0;}
void a2dp_sink_disconnect(uint16_t cid) {assert(cid);disconnected++;}
uint8_t a2dp_sink_establish_stream(bd_addr_t address,uint16_t *cid) {
    assert(memcmp(address,phone_address,6)==0);*cid=0x41;reconnects++;return 0;
}
uint8_t a2dp_sink_set_config_other(uint16_t cid,uint8_t local,uint8_t remote,const uint8_t *config,uint8_t length) {
    if (local==lhdc_seid) {
        assert(cid==phone.cid && local && remote && lhdc_configuration_valid(config,length));
        lhdc_selections++;selected_remote=remote;return lhdc_select_ok ? 0 : 0x11;
    }
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
bool phone_codec_available(phone_codec_t codec) {return codec==PHONE_CODEC_SBC ||
    (codec==PHONE_CODEC_APTX && aptx_available) || (codec==PHONE_CODEC_LHDCV5 && lhdc_available);}
bool phone_worker_configure(phone_codec_t codec,const phone_sbc_config_t*sbc) {desired=codec;codec_ready=false;if(codec==PHONE_CODEC_SBC)assert(sbc&&sbc->blocks==16&&sbc->subbands==8&&sbc->channel_mode==2);return true;}
phone_worker_status_t phone_worker_prepare(void) {prepares++;if(prepare_waiting)return PHONE_WORKER_WAITING;codec_ready=prepare_ok;return prepare_ok ? PHONE_WORKER_READY : PHONE_WORKER_FAILED;}
bool phone_worker_close(void) {desired=PHONE_CODEC_NONE;codec_ready=false;worker_pending=false;closes++;return !close_waiting;}
bool phone_worker_released(void) {return !codec_ready && !close_waiting;}
bool phone_worker_poll(phone_codec_pcm_callback callback,void*context) {
    if (!async_worker || !worker_pending || !codec_ready) return true;
    worker_pending=false;
    if (!decode_ok) return false;
    static const int16_t pcm[480]={1,2,3,4};
    callback(pcm,240,context);
    return true;
}
bool phone_worker_ready(void) {return codec_ready;}
uint32_t phone_worker_last_decode_us(void) {return codec_ready ? 1250u : 0u;}
uint32_t phone_worker_max_decode_us(void) {return codec_ready ? 2500u : 0u;}
phone_worker_queue_snapshot_t phone_worker_queue_snapshot(void) {
    return (phone_worker_queue_snapshot_t){async_worker && worker_pending ? 1u : 0u, false};
}
void phone_worker_reset(void) {codec_resets++;worker_pending=false;}
bool phone_worker_submit(const uint8_t*payload,size_t length,uint8_t count) {
    decode_calls++;received_length=length;received_frames=count;
    assert(length<=sizeof(received_payload));memcpy(received_payload,payload,length);
    if (async_worker) {
        if (!submit_ok || worker_pending) return false;
        worker_pending=true;
        return true;
    }
    static int16_t pcm[8]={1,2,3,4,5,6,7,8};
    if(decode_ok)decoded_pcm(pcm,4,NULL);
    return decode_ok;
}
bool bridge_phone_open(void) {opens++;return open_allowed;}
bool bridge_phone_ready(void) {return output_ready;}
bool bridge_phone_recovering(void) {return output_recovering;}
bool bridge_phone_start(void) {claims++;return claim_allowed;}
void bridge_phone_stop(void) {stops++;}
void bridge_phone_pcm_push(const int16_t*pcm,uint16_t frames) {assert(pcm);pcm_frames+=frames;}
void bridge_phone_drop_pcm(void) {pcm_frames=0;pcm_drops++;}
bool bridge_phone_volume(uint8_t volume) {
    ++bridge_volume_attempts;
    if (!volume_request_ok) return false;
    if (confirmed_volume_known && confirmed_volume==volume) return true;
    assert(phone.controls_owned && !phone.disconnecting);
    last_bridge_volume=last_volume=volume;bridge_volume_requests++;volume_updates++;
    confirmed_volume_known=false;
    return true;
}
bool bridge_phone_get_volume(uint8_t *volume) {
    if (!confirmed_volume_known || volume==NULL) return false;
    *volume=confirmed_volume;return true;
}
void bt_volume_cancel_phone(void) {volume_cancels++;}
static uint8_t send(uint16_t cid,uint8_t operation) {assert(cid==phone.avrcp_cid);if(control_busy)return 0x0c;controls_sent++;last_control=operation;return 0;}
uint8_t avrcp_controller_play(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_PLAY);}
uint8_t avrcp_controller_pause(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_PAUSE);}
uint8_t avrcp_controller_stop(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_STOP);}
uint8_t avrcp_controller_forward(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_FORWARD);}
uint8_t avrcp_controller_backward(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_BACKWARD);}
uint8_t avrcp_controller_volume_up(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_VOLUME_UP);}
uint8_t avrcp_controller_volume_down(uint16_t cid) {return send(cid,AVRCP_OPERATION_ID_VOLUME_DOWN);}
uint8_t avrcp_target_support_event(uint16_t cid,uint8_t event) {
    assert(cid==phone.avrcp_cid&&event==AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
    target_support_attempts++;return target_support_busy ? 0x0c : 0;
}
uint8_t avrcp_target_volume_changed(uint16_t cid,uint8_t volume) {
    assert(cid==phone.avrcp_cid&&volume<=127);target_volume_attempts++;
    if (target_volume_busy) return 0x0c;
    target_volume_initialized=true;
    if (target_volume!=volume) {target_volume=last_volume=volume;volume_updates++;}
    return 0;
}
void a2dp_config_process_avdtp_event_handler(avdtp_role_t role,uint8_t*packet,uint16_t size) {
    assert(role==AVDTP_ROLE_SINK);framework_calls++;
    if(packet[2]==AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED){
        packet[0]=HCI_EVENT_A2DP_META;packet[2]=A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED;
        sink_handler(HCI_EVENT_PACKET,0,packet,size);
        packet[0]=HCI_EVENT_AVDTP_META;packet[2]=AVDTP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED;
    }
}

static void fixture_codecs(bool lhdc,bool aptx,bool create_lhdc) {
    memset(&phone,0,sizeof(phone));memset(endpoints,0,sizeof(endpoints));
    initialized=lhdc_disabled=aptx_disabled=retry_codec=false;lhdc_endpoint=aptx_endpoint=sbc_endpoint=NULL;
    lhdc_seid=aptx_seid=sbc_seid=0;
    endpoint_count=finalized=framework_calls=accepted=rejected=disconnected=0;
    opens=claims=stops=pcm_frames=pcm_drops=codec_resets=decode_calls=prepares=closes=controls_sent=volume_updates=0;
    now=0;output_ready=output_recovering=false;open_allowed=claim_allowed=secure_peer=prepare_ok=decode_ok=true;
    codec_ready=control_busy=false;lhdc_available=lhdc;aptx_available=aptx;desired=PHONE_CODEC_NONE;
    prepare_waiting=close_waiting=false;
    async_worker=worker_pending=false;submit_ok=true;
    confirmed_volume_known=volume_request_ok=true;confirmed_volume=53;
    target_volume_busy=target_support_busy=target_volume_initialized=false;target_volume=0;
    bridge_volume_attempts=bridge_volume_requests=target_volume_attempts=target_support_attempts=0;
    volume_cancels=0;accept_ok=true;
    lhdc_selections=aptx_selections=sbc_selections=reconnects=0;selected_remote=0;lhdc_select_ok=aptx_select_ok=true;
    lhdc_create_ok=create_lhdc;
    assert(phone_sink_init());
}
static void fixture(bool aptx) {fixture_codecs(false,aptx,true);}
static void address_event(uint8_t*p,unsigned offset,const bd_addr_t address) {for(unsigned i=0;i<6;i++)p[offset+i]=address[5-i];}
static void phone_avrcp_connected(uint16_t cid) {
    uint8_t p[14]={HCI_EVENT_AVRCP_META,12,AVRCP_SUBEVENT_CONNECTION_ESTABLISHED,0,(uint8_t)cid,(uint8_t)(cid>>8)};
    address_event(p,6,phone_address);
    target_volume=0;target_volume_initialized=false;
    assert(phone_sink_handle_avrcp(HCI_EVENT_PACKET,0,p,sizeof(p)));
}
static bool phone_volume_set(uint16_t cid,uint8_t volume) {
    uint8_t p[7]={HCI_EVENT_AVRCP_META,5,AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED,
        (uint8_t)cid,(uint8_t)(cid>>8),0,volume};
    // The pinned target updates its own value before emitting the SET event.
    if (cid==phone.avrcp_cid && volume<=127) target_volume=volume;
    return phone_sink_handle_target(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void confirm_volume(uint8_t volume) {
    confirmed_volume_known=true;confirmed_volume=volume;phone_sink_volume_changed(volume);
}
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
static void lhdc_event(uint8_t*p) {
    uint8_t event[24]={HCI_EVENT_A2DP_META,22,A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION,
        (uint8_t)phone.cid,(uint8_t)(phone.cid>>8),lhdc_seid,1,0,AVDTP_AUDIO,0xff,0,11,0};
    memcpy(p,event,sizeof(event));memcpy(p+13,lhdc_capabilities,sizeof(lhdc_capabilities));
}
static void configure_lhdc(void) {
    uint8_t p[24];lhdc_event(p);assert(validator(lhdc_endpoint,p,sizeof(p))==0);
    sink_handler(HCI_EVENT_PACKET,0,p,sizeof(p));phone_sink_poll();
    assert(phone.configured && codec_ready && desired==PHONE_CODEC_LHDCV5);
}
static void test_precommit_remote_endpoint(void) {
    for(unsigned codec=0;codec<3;codec++) {
        fixture_codecs(true,true,true);signaling(0x40,phone_address);
        uint8_t p[24];unsigned size;
        avdtp_stream_endpoint_t *endpoint;
        if(codec==0) {lhdc_event(p);size=24;endpoint=lhdc_endpoint;}
        else if(codec==1) {
            uint8_t event[20]={HCI_EVENT_A2DP_META,18,A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION,
                (uint8_t)phone.cid,(uint8_t)(phone.cid>>8),aptx_seid,1,0,AVDTP_AUDIO,0xff,0,7,0};
            memcpy(p,event,sizeof(event));memcpy(p+13,aptx_capabilities,7);
            size=20;endpoint=aptx_endpoint;
        } else {sbc_event(p);size=18;endpoint=sbc_endpoint;}
        // Pinned BTstack validates before assigning the incoming remote SEP.
        p[6]=0;
        assert(validator(endpoint,p,size)==ERROR_CODE_SUCCESS);
        assert(!phone.configured && desired==PHONE_CODEC_NONE);
        sink_handler(HCI_EVENT_PACKET,0,p,size);
        assert(!phone.configured && desired==PHONE_CODEC_NONE); // Committed events still need a bound SEP.
        p[6]=0x3f;assert(validator(endpoint,p,size)==AVDTP_ERROR_CODE_UNSUPPORTED_CONFIGURATION);
        p[6]=1;assert(validator(endpoint,p,size)==ERROR_CODE_SUCCESS);
        sink_handler(HCI_EVENT_PACKET,0,p,size);assert(phone.configured);
    }
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
static void remote_lhdc(const uint8_t*caps) {
    uint8_t p[22]={HCI_EVENT_A2DP_META,20,A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CAPABILITY,
        (uint8_t)phone.cid,(uint8_t)(phone.cid>>8),13,AVDTP_AUDIO,0xff,0,11,0};
    memcpy(p+11,caps,sizeof(lhdc_capabilities));sink_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void capabilities_complete(void) {
    uint8_t p[6]={HCI_EVENT_A2DP_META,4,A2DP_SUBEVENT_SIGNALING_CAPABILITIES_COMPLETE,
        (uint8_t)phone.cid,(uint8_t)(phone.cid>>8),0xa5};
    sink_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
    // Configuration must wait until the framework finishes this callback.
    assert(lhdc_selections==0 && aptx_selections==0 && sbc_selections==0);
    phone_sink_poll();
}
static void open_result(uint16_t cid,uint8_t seid,uint8_t status,const bd_addr_t address) {
    uint8_t p[14]={HCI_EVENT_A2DP_META,12,A2DP_SUBEVENT_STREAM_ESTABLISHED,
        (uint8_t)cid,(uint8_t)(cid>>8)};
    address_event(p,5,address);p[11]=seid;p[12]=1;p[13]=status;
    sink_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void opened(void) {open_result(phone.cid,phone.seid,0,phone_address);}
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
    assert(disconnected==1 && aptx_disabled && retry_codec && phone.disconnecting);
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
static void discover_all(void) {
    lhdc_selections=aptx_selections=sbc_selections=0;
    remote_sbc(1);remote_aptx(0x12);remote_lhdc(lhdc_capabilities);capabilities_complete();
}
static void release_signaling(void) {
    uint8_t p[5]={HCI_EVENT_A2DP_META,3,A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED,
        (uint8_t)phone.cid,(uint8_t)(phone.cid>>8)};
    sink_handler(HCI_EVENT_PACKET,0,p,sizeof(p));phone_sink_poll();
}
static void reject_configuration(uint8_t seid) {
    uint8_t p[8]={HCI_EVENT_A2DP_META,6,A2DP_SUBEVENT_COMMAND_REJECTED,
        (uint8_t)phone.cid,(uint8_t)(phone.cid>>8),seid,1,AVDTP_SI_SET_CONFIGURATION};
    sink_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void test_lhdc_availability_and_configuration(void) {
    fixture(true);assert(lhdc_endpoint==NULL && lhdc_seid==0);
    signaling(0x40,phone_address);discover_all();
    assert(lhdc_selections==0 && aptx_selections==1);
    fixture_codecs(true,true,false);assert(lhdc_endpoint==NULL && endpoint_count==2);
    signaling(0x40,phone_address);discover_all();assert(aptx_selections==1);

    fixture_codecs(true,true,true);assert(endpoint_count==3 && lhdc_seid<aptx_seid && aptx_seid<sbc_seid);
    signaling(0x40,phone_address);uint8_t p[24];lhdc_event(p);
    assert(validator(lhdc_endpoint,p,sizeof(p))==0);
    for(unsigned n=0;n<sizeof(p);n++)assert(validator(lhdc_endpoint,p,n)!=0);
    const unsigned exact[]={3,5,9,11,13,14,17,18,19,21};
    for(unsigned i=0;i<sizeof(exact)/sizeof(*exact);i++){
        uint8_t saved=p[exact[i]];p[exact[i]]^=1;
        assert(validator(lhdc_endpoint,p,sizeof(p))!=0);p[exact[i]]=saved;
    }
    const uint8_t rates[]={0,0x20,4,1,0x35};
    for(unsigned i=0;i<sizeof(rates);i++){p[19]=rates[i];assert(validator(lhdc_endpoint,p,sizeof(p))!=0);}
    p[19]=0x10;
    const uint8_t formats[]={0x94,0x96,0x91,0x82,0xa2,0xb2,0x12,0x52};
    for(unsigned i=0;i<sizeof(formats);i++){p[20]=formats[i];assert(validator(lhdc_endpoint,p,sizeof(p))!=0);}
    p[20]=0xd2;assert(validator(lhdc_endpoint,p,sizeof(p))==0);p[20]=0x92;
    const uint8_t versions[]={0,1,0x10,0x12,0x31};
    for(unsigned i=0;i<sizeof(versions);i++){p[21]=versions[i];assert(validator(lhdc_endpoint,p,sizeof(p))!=0);}
    p[21]=0x11;
    for(unsigned at=22;at<24;at++)for(unsigned bit=0;bit<8;bit++){
        p[at]=(uint8_t)(1u<<bit);assert(validator(lhdc_endpoint,p,sizeof(p))!=0);p[at]=0;
    }
    lhdc_available=false;assert(validator(lhdc_endpoint,p,sizeof(p))!=0);lhdc_available=true;
    p[0]=HCI_EVENT_AVDTP_META;p[2]=AVDTP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION;
    unsigned before=framework_calls;
    for(unsigned n=3;n<sizeof(p);n++){p[1]=(uint8_t)(n-2);raw_handler(HCI_EVENT_PACKET,0,p,n);}
    assert(framework_calls==before);
    p[1]=22;raw_handler(HCI_EVENT_PACKET,0,p,sizeof(p));assert(framework_calls==before+1);
    p[11]=12;raw_handler(HCI_EVENT_PACKET,0,p,sizeof(p));assert(framework_calls==before+1);
    configure_lhdc();assert(phone.codec==PHONE_CODEC_LHDCV5);
}
static void test_lhdc_preference_and_classic_fallbacks(void) {
    fixture_codecs(true,true,true);signaling(0x40,phone_address);discover_all();
    assert(lhdc_selections==1 && aptx_selections==0 && sbc_selections==0 && selected_remote==13);
    assert(phone.requested_codec==PHONE_CODEC_LHDCV5);
    configure(true);assert(phone.codec==PHONE_CODEC_APTX); // inbound source choice is independent
    reject_configuration(lhdc_seid);assert(phone.configured && !phone.disconnecting && !lhdc_disabled);

    uint8_t caps[sizeof(lhdc_capabilities)];memcpy(caps,lhdc_capabilities,sizeof(caps));
    caps[6]=0x35;caps[7]=0xc7;caps[9]=caps[10]=0xff;
    fixture_codecs(true,true,true);signaling(0x40,phone_address);remote_lhdc(caps);capabilities_complete();
    assert(lhdc_selections==1 && phone.remote_lhdc_configuration[7]==0xd2);
    assert(phone.remote_lhdc_configuration[9]==0 && phone.remote_lhdc_configuration[10]==0);
    const unsigned required[]={0,4,6,7,8};
    for(unsigned i=0;i<sizeof(required)/sizeof(*required);i++){
        fixture_codecs(true,true,true);signaling(0x40,phone_address);
        memcpy(caps,lhdc_capabilities,sizeof(caps));caps[required[i]]=0;
        remote_lhdc(caps);remote_aptx(0x12);capabilities_complete();
        assert(lhdc_selections==0 && aptx_selections==1);
    }

    fixture_codecs(true,true,true);signaling(0x40,phone_address);lhdc_select_ok=false;discover_all();
    assert(lhdc_selections==1 && aptx_selections==1 && phone.requested_codec==PHONE_CODEC_APTX);
    fixture_codecs(true,true,true);signaling(0x40,phone_address);lhdc_select_ok=aptx_select_ok=false;discover_all();
    assert(lhdc_selections==1 && aptx_selections==1 && sbc_selections==1 && phone.requested_codec==PHONE_CODEC_SBC);
    fixture_codecs(true,false,true);signaling(0x40,phone_address);lhdc_select_ok=false;discover_all();
    assert(lhdc_selections==1 && aptx_selections==0 && sbc_selections==1);

    fixture_codecs(true,true,true);signaling(0x40,phone_address);discover_all();
    reject_configuration(lhdc_seid);
    assert(lhdc_disabled && !aptx_disabled && retry_codec && finalized==0 && phone.disconnecting);
    release_signaling();assert(lhdc_seid==0 && aptx_seid!=0 && finalized==1 && reconnects==1);
    signaling(0x41,phone_address);discover_all();assert(lhdc_selections==0 && aptx_selections==1);
    reject_configuration(aptx_seid);release_signaling();
    assert(aptx_seid==0 && finalized==2 && reconnects==2);
    signaling(0x42,phone_address);discover_all();assert(lhdc_selections==0 && aptx_selections==0 && sbc_selections==1);

    fixture_codecs(true,true,true);signaling(0x40,phone_address);configure_lhdc();
    codec_ready=false;prepare_ok=false;phone_sink_poll();
    assert(lhdc_disabled && !aptx_disabled && phone.disconnecting && finalized==0);
    release_signaling();assert(lhdc_seid==0 && aptx_seid!=0 && reconnects==1);
    prepare_ok=true;signaling(0x41,phone_address);discover_all();configure(true);
    assert(desired==PHONE_CODEC_APTX);

    fixture_codecs(true,true,true);signaling(0x40,phone_address);discover_all();
    reject_configuration(lhdc_seid);phone_sink_yield();release_signaling();
    assert(reconnects==0 && !retry_codec); // USB takeover also cancels queued fallback
}
static unsigned lhdc_rtp(uint8_t*p,uint16_t sequence,uint8_t codec_sequence,uint8_t frames,unsigned length) {
    rtp(p,sequence);p[12]=(uint8_t)(frames<<2);p[13]=codec_sequence;
    memset(p+14,0xa5,length);return 14+length;
}
static void reject_open(uint16_t cid,uint8_t seid,bool initiator) {
    uint8_t p[8]={HCI_EVENT_A2DP_META,6,A2DP_SUBEVENT_COMMAND_REJECTED,
        (uint8_t)cid,(uint8_t)(cid>>8),seid,initiator,AVDTP_SI_OPEN};
    sink_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void accept_open(uint16_t cid,uint8_t seid) {
    uint8_t p[8]={HCI_EVENT_AVDTP_META,6,AVDTP_SUBEVENT_SIGNALING_ACCEPT,
        (uint8_t)cid,(uint8_t)(cid>>8),seid,0,AVDTP_SI_OPEN};
    raw_handler(HCI_EVENT_PACKET,0,p,sizeof(p));
}
static void configure_lhdc_outgoing(void) {discover_all();configure_lhdc();assert(phone.opening);}
static void test_open_failure_fallbacks(void) {
    fixture_codecs(true,true,true);signaling(0x40,phone_address);configure_lhdc_outgoing();
    reject_open(0x41,phone.seid,true);reject_open(phone.cid,aptx_seid,true);
    reject_open(phone.cid,phone.seid,false);
    open_result(0x41,phone.seid,0x0c,phone_address);
    open_result(phone.cid,aptx_seid,0x0c,phone_address);
    open_result(phone.cid,phone.seid,0x0c,airpods_address);
    assert(!disconnected && phone.configured && !phone.opened && !lhdc_disabled);
    reject_open(phone.cid,phone.seid,true);
    assert(disconnected==1 && lhdc_disabled && !aptx_disabled && retry_codec && !phone.configured);
    opened();reject_open(phone.cid,lhdc_seid,true);phone_sink_poll();
    assert(disconnected==1 && !phone.opened && !codec_ready);
    release_signaling();assert(reconnects==1 && lhdc_seid==0 && aptx_seid!=0);
    signaling(0x41,phone_address);discover_all();configure(true);
    reject_open(0x40,phone.seid,true);assert(!phone.disconnecting);
    reject_open(phone.cid,phone.seid,true);release_signaling();
    assert(aptx_seid==0 && reconnects==2);
    signaling(0x42,phone_address);discover_all();configure(false);
    open_result(phone.cid,phone.seid,0x0c,phone_address);release_signaling();
    assert(reconnects==2 && sbc_seid!=0); // SBC still disconnects without another codec retry.

    fixture_codecs(true,true,true);signaling(0x40,phone_address);configure_lhdc();
    open_result(phone.cid,phone.seid,0x0c,phone_address);phone_sink_poll();
    assert(lhdc_disabled && !aptx_disabled && disconnected==1 && !codec_ready);
    release_signaling();assert(reconnects==1 && lhdc_seid==0);
}
static void test_open_deadline_and_cancellation(void) {
    fixture_codecs(true,true,true);signaling(0x40,phone_address);
    now=UINT32_MAX-999u;configure_lhdc_outgoing();
    now=1999;phone_sink_poll();assert(!disconnected && phone.configured);
    now=2000;phone_sink_poll();assert(disconnected==1 && lhdc_disabled && !codec_ready);
    phone_sink_poll();release_signaling();phone_sink_poll();assert(reconnects==1);

    fixture_codecs(true,true,true);signaling(0x40,phone_address);configure_lhdc();
    now=10000;phone_sink_poll();assert(!disconnected && !phone.opening && codec_ready);
    accept_open(0x41,phone.seid);accept_open(phone.cid,aptx_seid);
    assert(!phone.opening); // An incoming configured phone may legitimately wait to play.
    accept_open(phone.cid,phone.seid);assert(phone.opening);
    now=12999;phone_sink_poll();assert(!disconnected);
    now=13000;phone_sink_poll();assert(disconnected==1 && lhdc_disabled);

    fixture_codecs(true,true,true);signaling(0x40,phone_address);configure_lhdc_outgoing();
    now=2999;configure_lhdc(); // A replacement peer configuration cancels the old local Open deadline.
    now=10000;phone_sink_poll();assert(!disconnected && !phone.opening);
    accept_open(phone.cid,phone.seid);now=12999;phone_sink_poll();assert(!disconnected);
    now=13000;phone_sink_poll();assert(disconnected==1);

    fixture_codecs(true,true,true);signaling(0x40,phone_address);configure_lhdc_outgoing();
    now=2999;opened();now=6000;phone_sink_poll();
    reject_open(phone.cid,phone.seid,true);open_result(phone.cid,phone.seid,0x0c,phone_address);
    assert(!disconnected && phone.opened && !lhdc_disabled);
    uint8_t reconfigure[24];lhdc_event(reconfigure);reconfigure[7]=1;
    sink_handler(HCI_EVENT_PACKET,0,reconfigure,sizeof(reconfigure));
    now=10000;phone_sink_poll();assert(phone.opened && !disconnected);

    fixture_codecs(true,true,true);signaling(0x40,phone_address);configure_lhdc_outgoing();
    now=2999;phone_sink_yield();now=10000;phone_sink_poll();
    reject_open(phone.cid,phone.seid,true);opened();release_signaling();
    assert(!lhdc_disabled && !retry_codec && !reconnects && !phone.configured && !phone.opened);
    signaling(0x41,phone_address);configure_lhdc_outgoing();now=12999;phone_sink_poll();
    assert(!phone.disconnecting && !lhdc_disabled);
    opened();now=20000;phone_sink_poll();assert(!phone.disconnecting && phone.opened);
}

static void lhdc_playing_async(void) {
    fixture_codecs(true,true,true);signaling(0x40,phone_address);configure_lhdc();opened();
    output_ready=true;stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);phone_sink_poll();
    stream_event(A2DP_SUBEVENT_STREAM_STARTED,phone.seid);
    async_worker=true;
}
static void test_queue_congestion_preserves_pending_audio(void) {
    for(unsigned codec=0;codec<2;++codec) {
        if(codec==0) {fixture(true);playing(true);async_worker=true;}
        else lhdc_playing_async();
        uint8_t packet[32]={1,2,3,4};
        unsigned size=codec ? lhdc_rtp(packet,1,1,1,4) : 4;
        pcm_frames=960; // Previously decoded audio remains valid during a compressed-input burst.
        unsigned resets=phone.buffer_resets, worker_resets=codec_resets, drops=pcm_drops;
        media_handler(phone.seid,packet,size);assert(worker_pending && phone.packets==1);
        for(unsigned seq=2;seq<=4;++seq) {
            if(codec) size=lhdc_rtp(packet,(uint16_t)seq,(uint8_t)seq,1,4);
            media_handler(phone.seid,packet,size);
            assert(worker_pending && phone.packets==1 && phone.submit_failures==seq-1);
            assert(phone.malformed==0 && phone.buffer_resets==resets && codec_resets==worker_resets);
            assert(pcm_frames==960 && pcm_drops==drops);
        }
        phone_sink_poll();assert(!worker_pending && pcm_frames==1200);
        if(codec) size=lhdc_rtp(packet,5,5,1,4);
        media_handler(phone.seid,packet,size);assert(worker_pending && phone.packets==2);
        phone_sink_poll();assert(pcm_frames==1440 && phone.sequence_gaps==0);
        assert(phone.malformed==0 && phone.buffer_resets==resets && codec_resets==worker_resets);
        media_handler(phone.seid,packet,3); // Malformed data still cancels stale work and PCM.
        assert(phone.malformed==1 && phone.buffer_resets==resets+1 && codec_resets==worker_resets+1);
        assert(pcm_frames==0 && pcm_drops==drops+1 && !worker_pending);
    }
}
static void lhdc_decode_result(uint16_t sequence,bool success) {
    uint8_t p[32];unsigned length=lhdc_rtp(p,sequence,(uint8_t)sequence,1,4);
    decode_ok=success;media_handler(phone.seid,p,length);
    assert(worker_pending); // Submission itself is not a decoded success/failure.
    phone_sink_poll();assert(!worker_pending);
}
static void test_lhdc_runtime_failure_recovery(void) {
    lhdc_playing_async();lhdc_decode_result(0,false);
    assert(phone.decode_failures==1 && phone.started && !lhdc_disabled && !disconnected);
    for(unsigned i=0;i<20;i++)phone_sink_poll();
    assert(phone.decode_failures==1); // Empty successful polls cannot erase failed completions.
    lhdc_decode_result(1,true);assert(phone.decode_failures==0 && pcm_frames==240);
    lhdc_decode_result(2,false);lhdc_decode_result(3,false);
    assert(phone.decode_failures==2 && !disconnected);
    uint8_t p[32];unsigned size=lhdc_rtp(p,5,5,1,4);
    submit_ok=false;media_handler(phone.seid,p,size);phone_sink_poll();
    assert(phone.decode_failures==2 && !lhdc_disabled); // Backpressure and sequence gap do not count.
    media_handler(phone.seid,p,size);assert(phone.decode_failures==2); // Duplicate ignored.
    media_handler(phone.seid,p,3);assert(phone.decode_failures==2); // Malformed RTP does not count.
    submit_ok=true;lhdc_decode_result(6,true);assert(phone.decode_failures==0);
    lhdc_decode_result(7,false);lhdc_decode_result(8,false);lhdc_decode_result(9,false);
    assert(lhdc_disabled && !aptx_disabled && disconnected==1 && !phone.started && !codec_ready);
    phone_sink_poll();release_signaling();phone_sink_poll();assert(reconnects==1 && lhdc_seid==0);
    signaling(0x41,phone_address);discover_all();assert(phone.requested_codec==PHONE_CODEC_APTX);
    configure(true);open_result(phone.cid,phone.seid,0x0c,phone_address);release_signaling();
    assert(aptx_disabled && reconnects==2 && aptx_seid==0);
    signaling(0x42,phone_address);discover_all();configure(false);opened();
    output_ready=true;stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);phone_sink_poll();
    stream_event(A2DP_SUBEVENT_STREAM_STARTED,phone.seid);assert(phone.started && phone.codec==PHONE_CODEC_SBC);

    lhdc_playing_async();lhdc_decode_result(0,false);lhdc_decode_result(1,false);
    stream_event(A2DP_SUBEVENT_STREAM_SUSPENDED,phone.seid);
    assert(phone.decode_failures==0);
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);phone_sink_poll();
    stream_event(A2DP_SUBEVENT_STREAM_STARTED,phone.seid);lhdc_decode_result(0,false);
    assert(phone.decode_failures==1 && !lhdc_disabled);
    phone_sink_yield();phone_sink_poll();release_signaling();
    assert(!lhdc_disabled && !retry_codec && !reconnects && !worker_pending);
}
static void test_lhdc_media_bounds_sequence_and_cancellation(void) {
    fixture_codecs(true,true,true);signaling(0x40,phone_address);configure_lhdc();opened();
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);output_ready=true;phone_sink_poll();
    uint8_t p[PHONE_CODEC_MAX_PAYLOAD+32];unsigned size=lhdc_rtp(p,65535,255,1,4);
    media_handler(phone.seid,p,size);assert(decode_calls==0);
    stream_event(A2DP_SUBEVENT_STREAM_STARTED,phone.seid);
    media_handler(phone.seid,p,size);assert(decode_calls==1 && received_frames==1 && received_length==4 && received_payload[0]==0xa5);
    lhdc_rtp(p,0,0,8,PHONE_CODEC_MAX_PAYLOAD);media_handler(phone.seid,p,14+PHONE_CODEC_MAX_PAYLOAD);
    assert(decode_calls==2 && received_frames==8 && phone.sequence_gaps==0);
    unsigned before=codec_resets;
    size=lhdc_rtp(p,1,2,1,4);media_handler(phone.seid,p,size);
    assert(decode_calls==3 && phone.sequence_gaps==1 && codec_resets==before+1);
    media_handler(phone.seid,p,size);assert(decode_calls==3 && codec_resets==before+1);
    size=lhdc_rtp(p,0,1,1,4);media_handler(phone.seid,p,size);assert(decode_calls==3);
    size=lhdc_rtp(p,3,4,1,4);media_handler(phone.seid,p,size);
    assert(decode_calls==4 && phone.sequence_gaps==2 && codec_resets==before+2);
    size=lhdc_rtp(p,4,80,1,4);p[11]=2;media_handler(phone.seid,p,size);
    assert(decode_calls==5 && codec_resets==before+3); // new SSRC starts a new codec sequence

    for(unsigned n=0;n<=14;n++)media_handler(phone.seid,p,n);
    const uint8_t headers[]={0,1,2,3,5,6,7,9u<<2,0x80};
    for(unsigned i=0;i<sizeof(headers);i++){
        size=lhdc_rtp(p,5,81,1,4);p[12]=headers[i];media_handler(phone.seid,p,size);
    }
    size=lhdc_rtp(p,5,81,1,PHONE_CODEC_MAX_PAYLOAD+1);media_handler(phone.seid,p,size);
    assert(decode_calls==5);
    size=lhdc_rtp(p,5,81,1,4);decode_ok=false;async_worker=true;
    media_handler(phone.seid,p,size);assert(worker_pending);phone_sink_poll();
    assert(decode_calls==6 && pcm_frames==0 && phone.total_decode_failures==1);
    decode_ok=true;async_worker=false;
    memset(p,0,34);p[0]=0xb2;p[1]=96;p[3]=6;p[11]=1;p[23]=1;
    p[28]=4;p[29]=82;p[30]=0xa5;p[31]=0x5a;p[33]=2;
    media_handler(phone.seid,p,34);assert(decode_calls==7 && received_length==2 && received_frames==1);
    assert(received_payload[0]==0xa5 && received_payload[1]==0x5a);
    media_handler((uint8_t)(phone.seid+1),p,34);assert(decode_calls==7);
    uint8_t config[24];lhdc_event(config);assert(validator(lhdc_endpoint,config,sizeof(config))==AVDTP_ERROR_CODE_SEP_IN_USE);
    uint8_t stale[6]={HCI_EVENT_A2DP_META,4,A2DP_SUBEVENT_STREAM_SUSPENDED,0x41,0,phone.seid};
    sink_handler(HCI_EVENT_PACKET,0,stale,sizeof(stale));assert(phone.started);
    phone_sink_yield();media_handler(phone.seid,p,34);assert(decode_calls==7 && !phone.started);
    assert(validator(lhdc_endpoint,config,sizeof(config))!=0);
    sink_handler(HCI_EVENT_PACKET,0,config,sizeof(config));phone_sink_poll();assert(!phone.configured);
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
    rtp(p,3);decode_ok=false;async_worker=true;media_handler(phone.seid,p,size);
    assert(worker_pending);phone_sink_poll();
    assert(decode_calls==4 && pcm_frames==0 && phone.total_decode_failures==1);
    decode_ok=true;async_worker=false;
    memset(p,0,sizeof(p));p[0]=0xb2;p[1]=96;p[3]=4;p[11]=1;
    p[23]=1;p[28]=1;p[29]=0x9c;p[30]=0x77;p[32]=2;
    media_handler(phone.seid,p,33); // two CSRCs, one extension word, two padding bytes
    assert(decode_calls==5 && received_length==2 && received_payload[0]==0x9c);
    media_handler((uint8_t)(phone.seid+1),p,33);assert(decode_calls==5);
    uint8_t configuration[18];sbc_event(configuration);
    assert(validator(sbc_endpoint,configuration,sizeof(configuration))==AVDTP_ERROR_CODE_SEP_IN_USE);
    output_ready=false;phone_sink_poll();assert(!phone_sink_active() && disconnected==1);
}

static uint32_t phone_diagnostic_field(const uint8_t *data, unsigned field) {
    const uint8_t *p = data + 4u + field * 4u;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void test_read_only_phone_diagnostics(void) {
    fixture(true);playing(true);async_worker=true;
    uint8_t data[53], packet[64], raw[4]={1,2,3,4};
    memset(data,0xa5,sizeof(data));
    assert(phone_sink_diagnostics(NULL,52)==0 && phone_sink_diagnostics(data,51)==0);
    for(unsigned i=0;i<sizeof(data);++i)assert(data[i]==0xa5);
    now=100;media_handler(phone.seid,raw,sizeof(raw));
    assert(phone_sink_diagnostics(data,sizeof(data))==52);
    assert(data[0]==1 && data[1]==PHONE_CODEC_APTX && data[2]==0xcf && data[3]==12 && data[52]==0xa5);
    assert(phone_diagnostic_field(data,0)==1 && phone_diagnostic_field(data,8)==1);
    assert(phone_diagnostic_field(data,9)==0 && phone_diagnostic_field(data,10)==1250);
    assert(phone_diagnostic_field(data,11)==2500 && worker_pending); // Reading never consumes work.
    unsigned resets=phone.buffer_resets;
    now=101;media_handler(phone.seid,raw,sizeof(raw)); // Stub queue is still occupied.
    assert(phone.submit_failures==1 && phone.malformed==0 && phone.buffer_resets==resets && worker_pending);
    now=110;
    decode_ok=false;phone_sink_poll();
    assert(phone.total_decode_failures==1 && phone.malformed==1 && phone.buffer_resets==resets+1);
    now=120;decode_ok=true;media_handler(phone.seid,raw,sizeof(raw));phone_sink_poll();
    assert(phone.decoded_frames==240);
    now=130;media_handler(phone.seid,raw,3);
    assert(phone_sink_diagnostics(data,sizeof(data))==52);
    assert(phone_diagnostic_field(data,0)==2 && phone_diagnostic_field(data,1)==2);
    assert(phone_diagnostic_field(data,3)==1 && phone_diagnostic_field(data,4)==1);
    assert(phone_diagnostic_field(data,5)==resets+2 && phone_diagnostic_field(data,6)==240);
    assert(phone_diagnostic_field(data,7)==130 && phone_diagnostic_field(data,8)==0);
    phone.packets=0x12345678u;phone_sink_diagnostics(data,sizeof(data));
    assert(data[4]==0x78 && data[5]==0x56 && data[6]==0x34 && data[7]==0x12);

    fixture(true);playing(false);resets=phone.buffer_resets;
    unsigned size=rtp(packet,1);media_handler(phone.seid,packet,size);
    rtp(packet,3);media_handler(phone.seid,packet,size);
    rtp(packet,4);packet[11]=2;media_handler(phone.seid,packet,size); // SSRC change also clears PCM.
    assert(phone.sequence_gaps==1 && phone.buffer_resets==resets+2);
    stream_event(A2DP_SUBEVENT_STREAM_SUSPENDED,phone.seid);
    assert(phone.buffer_resets==resets+3);
    phone_sink_diagnostics(data,sizeof(data));
    assert(phone_diagnostic_field(data,2)==1 && phone_diagnostic_field(data,5)==resets+3);
    assert((data[2]&8)==0 && phone_diagnostic_field(data,6)==12);
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
    assert(phone_volume_set(0x50,66));assert(last_volume==66);
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);phone_sink_poll();
    stream_event(A2DP_SUBEVENT_STREAM_STARTED,phone.seid);
    control_busy=true;
    for(unsigned i=0;i<8;i++)assert(phone_sink_media_control(AVRCP_OPERATION_ID_FORWARD));
    assert(phone.control_count==PHONE_CONTROL_QUEUE);phone_sink_poll();assert(controls_sent==2);
    control_busy=false;phone_sink_poll();assert(controls_sent==3);
    assert(phone_volume_set(0x50,88));assert(last_volume==88);
    unsigned before=volume_updates;phone_sink_volume_changed(88);assert(volume_updates==before);
    assert(!phone_volume_set(0x51,40));
    uint8_t release[5]={HCI_EVENT_AVRCP_META,3,AVRCP_SUBEVENT_CONNECTION_RELEASED,0x50,0};
    assert(phone_sink_handle_avrcp(HCI_EVENT_PACKET,0,release,5));assert(phone.control_count==0 && phone.avrcp_cid==0);
}
static void test_unknown_phone_volume_is_not_intent(void) {
    fixture(true);confirmed_volume_known=false;
    phone_avrcp_connected(0x50);
    assert(!phone.volume_valid && !phone.requested_volume_valid && volume_updates==0);
    assert(target_volume_attempts==0 && target_support_attempts==1 && phone.volume_event_supported);
    assert(!target_volume_initialized); // Capability advertisement must not seed a guessed value.
    assert(opens==0 && claims==0 && bridge_volume_attempts==0);
    for(unsigned i=0;i<5;i++)phone_sink_poll();
    assert(target_volume_attempts==0 && bridge_volume_attempts==0);
    confirm_volume(42);
    assert(phone.volume_valid && phone.volume==42 && !phone.requested_volume_valid);
    assert(target_volume_initialized && target_volume==42 && target_support_attempts==1 && bridge_volume_attempts==0);
    playing(false);
    assert(bridge_volume_attempts==0); // A confirmed report is not a future Start request.
    stream_event(A2DP_SUBEVENT_STREAM_SUSPENDED,phone.seid);
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);phone_sink_poll();
    assert(bridge_volume_attempts==0);

    fixture(true);phone_avrcp_connected(0x50);
    assert(phone.volume_valid && phone.volume==53 && bridge_volume_attempts==0);
    assert(opens==0 && claims==0); // A controls-only connection never changes AirPods volume/audio.
    fixture(true);confirmed_volume=0;phone_avrcp_connected(0x50);
    assert(phone.volume_valid && phone.volume==0 && target_support_attempts==1);
}
static void test_explicit_phone_volume_survives_readback_until_start(void) {
    fixture(true);confirmed_volume_known=false;phone_avrcp_connected(0x50);
    assert(phone_volume_set(0x50,41) && phone.requested_volume_valid);
    assert(!phone.volume_valid && bridge_volume_attempts==0);
    confirm_volume(80); // PC confirmation cannot overwrite the separate phone intent.
    assert(phone.volume==80 && phone.requested_volume==41 && phone.requested_volume_valid);
    playing(false);
    assert(bridge_volume_requests==1 && last_bridge_volume==41 && !phone.requested_volume_valid);
    assert(!phone.volume_valid); // Queued intent is not confirmed readback.
    confirm_volume(41);
    unsigned attempts=bridge_volume_attempts;
    stream_event(A2DP_SUBEVENT_STREAM_SUSPENDED,phone.seid);
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);phone_sink_poll();
    assert(bridge_volume_attempts==attempts); // No replay on resume.

    fixture(true);phone_avrcp_connected(0x50);assert(phone_volume_set(0x50,0));
    playing(false);assert(bridge_volume_requests==1 && last_bridge_volume==0);

    fixture(true);phone_avrcp_connected(0x50);assert(phone_volume_set(0x50,53));
    playing(false);assert(!phone.requested_volume_valid && phone.volume_valid);
    assert(bridge_volume_requests==0); // A confirmed no-op stays known.
}
static void test_phone_notification_retry_and_invalidation(void) {
    fixture(true);confirmed_volume_known=false;phone_avrcp_connected(0x50);
    target_volume_busy=true;confirm_volume(44);
    assert(phone.volume_notification_pending && target_volume_attempts==1 && target_support_attempts==1);
    phone_sink_poll();assert(phone.volume_notification_pending && target_volume_attempts==2);
    confirm_volume(55);assert(phone.volume_notification_pending);
    target_volume_busy=false;phone_sink_poll();
    assert(!phone.volume_notification_pending && target_volume==55 && target_support_attempts==1);
    unsigned before=volume_updates;confirm_volume(55);assert(volume_updates==before);
    assert(bridge_volume_requests==0);

    fixture(true);confirmed_volume_known=false;target_support_busy=true;phone_avrcp_connected(0x50);
    assert(!phone.volume_event_supported && target_support_attempts==1 && target_volume_attempts==0);
    phone_sink_poll();assert(target_support_attempts==2 && target_volume_attempts==0);
    confirm_volume(40);
    assert(phone.volume_notification_pending && target_volume_attempts==0 && !phone.volume_event_supported);
    confirm_volume(45);target_support_busy=false;phone_sink_poll();
    assert(!phone.volume_notification_pending && phone.volume_event_supported && target_volume==45);

    assert(phone_volume_set(0x50,90) && phone.requested_volume_valid);
    target_volume_busy=true;confirm_volume(60);before=volume_updates;
    confirmed_volume_known=false;phone_sink_volume_invalidated();
    assert(!phone.volume_valid && !phone.volume_notification_pending && !phone.requested_volume_valid);
    phone_sink_poll();assert(volume_updates==before && bridge_volume_requests==0);
    playing(false);assert(bridge_volume_requests==0); // Pair/reset invalidation drops obsolete intent.
}
static void test_phone_volume_paused_retry_and_ownership_loss(void) {
    fixture(true);playing(false);phone_avrcp_connected(0x50);
    unsigned cancels=volume_cancels;
    stream_event(A2DP_SUBEVENT_STREAM_SUSPENDED,phone.seid);
    assert(phone.controls_owned && volume_cancels==cancels);
    volume_request_ok=false;assert(phone_volume_set(0x50,77));
    assert(phone.requested_volume_valid && bridge_volume_requests==0);
    phone_sink_poll();assert(phone.requested_volume_valid && bridge_volume_requests==0);
    volume_request_ok=true;phone_sink_poll();
    assert(!phone.requested_volume_valid && bridge_volume_requests==1 && last_bridge_volume==77);
    confirm_volume(77);
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);phone_sink_poll();
    assert(bridge_volume_requests==1 && volume_cancels==cancels);
    stream_event(A2DP_SUBEVENT_STREAM_STARTED,phone.seid);
    volume_request_ok=false;assert(phone_volume_set(0x50,88) && phone.requested_volume_valid);
    phone_sink_yield();
    assert(!phone.requested_volume_valid && volume_cancels>cancels && !phone.controls_owned);
    assert(phone_volume_set(0x50,89) && !phone.requested_volume_valid); // Ignore events during takeover.
    volume_request_ok=true;phone_sink_poll();assert(bridge_volume_requests==1);

    fixture(true);phone_avrcp_connected(0x50);assert(phone_volume_set(0x50,67));
    signaling(0x40,phone_address);configure(false);opened();
    accept_ok=false;output_ready=true;
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid);phone_sink_poll();
    assert(accepted==1 && !phone.start_accepted);
    assert(!phone.controls_owned && !phone.requested_volume_valid && bridge_volume_requests==0);

    fixture(true);playing(false);phone_avrcp_connected(0x50);
    stream_event(A2DP_SUBEVENT_STREAM_SUSPENDED,phone.seid);
    volume_request_ok=false;assert(phone_volume_set(0x50,68) && phone.requested_volume_valid);
    configure(false);
    assert(!phone.controls_owned && !phone.requested_volume_valid && bridge_volume_requests==0);
}
static void test_phone_volume_disconnect_and_stale_cid(void) {
    fixture(true);phone_avrcp_connected(0x50);
    unsigned before=volume_updates;
    assert(!phone_volume_set(0x51,90) && !phone.requested_volume_valid && volume_updates==before);
    assert(phone_volume_set(0x50,60) && phone.requested_volume_valid);
    uint8_t release[5]={HCI_EVENT_AVRCP_META,3,AVRCP_SUBEVENT_CONNECTION_RELEASED,0x51,0};
    assert(!phone_sink_handle_avrcp(HCI_EVENT_PACKET,0,release,sizeof(release)) && phone.requested_volume_valid);
    release[3]=0x50;
    unsigned cancels=volume_cancels;
    assert(phone_sink_handle_avrcp(HCI_EVENT_PACKET,0,release,sizeof(release)));
    assert(!phone.volume_valid && !phone.requested_volume_valid && !phone.volume_notification_pending);
    assert(phone.avrcp_cid==0 && volume_cancels==cancels+1);
    confirmed_volume_known=false;phone_avrcp_connected(0x51);
    assert(!phone.volume_valid && !phone.requested_volume_valid);
    assert(!phone_volume_set(0x50,70) && !phone.requested_volume_valid);
}
static void test_output_recovery_keeps_phone_stream(void) {
    lhdc_playing_async();
    unsigned resets_before=codec_resets, controls_before=controls_sent;
    output_ready=false; output_recovering=true;
    uint8_t packet[64];
    for(unsigned seq=1;seq<=4;++seq) {
        unsigned size=lhdc_rtp(packet,(uint16_t)seq,(uint8_t)seq,1,4);
        media_handler(phone.seid,packet,size); phone_sink_poll();
        assert(phone.started && !disconnected && pcm_frames==seq*240u);
    }
    assert(!phone.sequence_gaps && codec_resets==resets_before && controls_sent==controls_before);
    output_ready=true; output_recovering=false;
    unsigned size=lhdc_rtp(packet,5,5,1,4);
    media_handler(phone.seid,packet,size); phone_sink_poll();
    assert(phone.started && !disconnected && pcm_frames==1200);
    assert(codec_resets==resets_before && !phone.sequence_gaps);
    output_ready=false; phone_sink_poll();
    assert(disconnected==1 && !phone.started); // A real output loss still yields ownership.

    fixture(true); signaling(0x40,phone_address); configure(true); opened();
    output_recovering=true;
    stream_event(A2DP_SUBEVENT_START_STREAM_REQUESTED,phone.seid); phone_sink_poll();
    assert(!accepted && !phone.started); // Recovery cannot authorize an initial Start.
}

int main(void) {
    test_output_recovery_keeps_phone_stream();
    test_queue_congestion_preserves_pending_audio();
    test_read_only_phone_diagnostics();
    test_precommit_remote_endpoint();
    test_unknown_phone_volume_is_not_intent();
    test_explicit_phone_volume_survives_readback_until_start();
    test_phone_notification_retry_and_invalidation();
    test_phone_volume_paused_retry_and_ownership_loss();
    test_phone_volume_disconnect_and_stale_cid();
    test_roles_and_start_priming();test_configuration_and_security();test_aptx_raw_and_fallback();
    test_codec_selection();test_raw_boundaries_and_yield();test_worker_wait_and_release();
    test_rtp_bounds_sequence_and_faults();test_timeouts_and_stale_events();test_avrcp_ownership_controls_and_volume();
    test_lhdc_availability_and_configuration();test_lhdc_preference_and_classic_fallbacks();
    test_lhdc_media_bounds_sequence_and_cancellation();
    test_open_failure_fallbacks();test_open_deadline_and_cancellation();test_lhdc_runtime_failure_recovery();
    puts("phone sink: role isolation, LHDCv5/aptX/SBC, primed Start, packet bounds, loss, cancellation and AVRCP passed");
}
