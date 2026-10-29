// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/btstack/phone_call.c"

static const bd_addr_t phone_address = {1,2,3,4,5,6};
static hci_connection_t phone_connection = {0x20};
static uint32_t now, configured_features;
static bool authorized, acl_connected, usb_playing, usb_recording, output_ready, mic_active, can_send, reserved;
static bd_addr_t saved_phone_address;
static unsigned connects, slc_releases, sco_releases, answers, hangs, opens, stops, polls;
static unsigned media_resume_polls;
static unsigned uplinks, downlinks, requests, sends, call_changes;
static uint32_t microphone_samples;
static uint8_t init_result, send_result, command_result, speaker, microphone;
static unsigned sdp_length, sample_rate, last_downlink_frames;
static int16_t last_downlink[120];
static bool last_bad;
static uint8_t outgoing[123], last_sent[123];
static int sco_length;
static btstack_packet_handler_t hfp_handler, sco_handler;

uint32_t btstack_run_loop_get_time_ms(void) { return now; }
void gap_set_security_level(int level) { assert(level==LEVEL_2); }
bool bt_hci_phone_address(uint8_t out[6]) { memcpy(out,saved_phone_address,6);return true; }
bool bt_hci_phone_peer_connected(const uint8_t address[6]) {return authorized && !memcmp(address,phone_address,6);}
hci_connection_t *hci_connection_for_bd_addr_and_type(const uint8_t *address,int type) {
    assert(type==BD_ADDR_TYPE_ACL);return acl_connected&&!memcmp(address,phone_address,6)?&phone_connection:NULL;
}
bool get_usb_streaming(void) {return usb_playing;}
bool usb_mic_requested(void) {return usb_recording;}
bool aacp_mic_active(void) {return mic_active;}
void phone_sink_resume_after_call(void) {
    if (!phone_call_active() && !get_usb_streaming() && !usb_mic_requested() && !aacp_mic_active())
        ++media_resume_polls;
}
void aacp_mic_call_changed(void) {++call_changes;}
bool bridge_call_open(void) {++opens;return true;}
bool bridge_call_ready(void) {return output_ready;}
void bridge_call_stop(void) {++stops;}
bool phone_call_audio_reset(uint32_t rate) {sample_rate=rate;return rate==0||rate==8000||rate==16000;}
void phone_call_audio_set_gain(uint8_t a,uint8_t b) {assert(a<=15&&b<=15);speaker=a;microphone=b;}
void phone_call_audio_poll(void) {++polls;}
uint32_t phone_call_audio_mic_samples(void) {return microphone_samples;}
bool phone_call_audio_downlink(const int16_t *pcm,uint16_t frames,bool bad) {
    assert(frames<=120);++downlinks;last_downlink_frames=frames;last_bad=bad;
    memcpy(last_downlink,pcm,frames*sizeof(*pcm));return true;
}
void phone_call_audio_uplink(int16_t *pcm,uint16_t frames) {++uplinks;for(unsigned i=0;i<frames;++i)pcm[i]=(int16_t)(0x1200+i);}
uint8_t hfp_hf_init(uint8_t channel) {assert(channel==8);return init_result;}
void hfp_hf_init_supported_features(uint32_t features) {configured_features=features;}
void hfp_hf_init_codecs(uint8_t count,const uint8_t *codecs) {assert(count==1&&codecs[0]==1);}
uint8_t hfp_hf_set_default_speaker_gain(uint8_t gain) {assert(gain==15);return 0;}
uint8_t hfp_hf_set_default_microphone_gain(uint8_t gain) {assert(gain==15);return 0;}
void hfp_hf_register_packet_handler(btstack_packet_handler_t handler) {hfp_handler=handler;}
void hfp_hf_create_sdp_record_with_codecs(uint8_t *out,uint32_t handle,int channel,const char *name,uint16_t features,uint8_t count,const uint8_t *codecs) {
    assert(out&&handle&&channel==8&&name&&features==configured_features&&count==1&&codecs[0]==1);
}
unsigned de_get_len(const uint8_t *record) {(void)record;return sdp_length;}
uint32_t sdp_create_service_record_handle(void) {return 0x10009;}
uint8_t sdp_register_service(uint8_t *record) {(void)record;return 0;}
void hci_set_sco_voice_setting(uint16_t setting) {assert(setting==0x60);}
void hci_register_sco_packet_handler(btstack_packet_handler_t handler) {sco_handler=handler;}
uint8_t hfp_hf_establish_service_level_connection(uint8_t *address) {assert(!memcmp(address,saved_phone_address,6));++connects;return 0;}
uint8_t hfp_hf_release_service_level_connection(uint16_t handle) {assert(handle==0x20||handle==0x21);++slc_releases;return 0;}
uint8_t hfp_hf_release_audio_connection(uint16_t handle) {assert(handle==0x20);++sco_releases;return 0;}
uint8_t hfp_hf_answer_incoming_call(uint16_t handle) {assert(handle==0x20);++answers;return command_result;}
uint8_t hfp_hf_terminate_call(uint16_t handle) {assert(handle==0x20);++hangs;return command_result;}
bool hci_can_send_sco_packet_now_for_con_handle(uint16_t handle) {assert(handle==0x30);return can_send;}
int hci_get_sco_packet_length_for_connection(uint16_t handle) {assert(handle==0x30);return sco_length;}
void hci_reserve_packet_buffer(void) {assert(!reserved);reserved=true;}
uint8_t *hci_get_outgoing_packet_buffer(void) {assert(reserved);return outgoing;}
uint8_t hci_send_sco_packet_buffer(int length) {assert(reserved&&length==sco_length);reserved=false;++sends;memcpy(last_sent,outgoing,(unsigned)length);return send_result;}
void hci_request_sco_can_send_now_event_for_con_handle(uint16_t handle) {assert(handle==0x30);++requests;}
static void reset(void) {
    memset(&call,0,sizeof(call));call.acl=call.sco=call.retry_acl=HCI_CON_HANDLE_INVALID;
    now=0;authorized=acl_connected=can_send=output_ready=true;usb_playing=usb_recording=mic_active=reserved=false;
    memcpy(saved_phone_address,phone_address,6);
    phone_connection.con_handle=0x20;
    connects=slc_releases=sco_releases=answers=hangs=opens=stops=polls=media_resume_polls=0;
    uplinks=downlinks=requests=sends=call_changes=0;microphone_samples=0;
    init_result=send_result=command_result=0;sdp_length=140;sco_length=63;
    assert(phone_call_init());
    assert(configured_features==((1u<<9)|(1u<<4)));
}
static void slc(uint16_t handle,const uint8_t *peer) {
    uint8_t event[12]={HCI_EVENT_HFP_META,10,HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED};
    little_endian_store_16(event,3,handle);reverse_bytes(peer,event+6,6);hfp_handler(4,0,event,sizeof(event));
}
static void meta(uint8_t kind,uint16_t handle) {
    uint8_t event[5]={HCI_EVENT_HFP_META,3,kind};little_endian_store_16(event,3,handle);hfp_handler(4,0,event,5);
}
static void audio(uint8_t codec) {
    uint8_t event[21]={HCI_EVENT_HFP_META,19,HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED};
    little_endian_store_16(event,3,0x20);little_endian_store_16(event,6,0x30);
    reverse_bytes(phone_address,event+8,6);event[14]=codec;hfp_handler(4,0,event,sizeof(event));
}
static void send_now(uint16_t handle) {
    uint8_t event[4]={HCI_EVENT_SCO_CAN_SEND_NOW,2};little_endian_store_16(event,2,handle);sco_handler(4,0,event,4);
}
static void sco(uint16_t handle,uint8_t status,uint8_t length) {
    uint8_t packet[123]={0};little_endian_store_16(packet,0,(uint16_t)(handle|status<<12));packet[2]=length;
    for(unsigned i=0;i<length;++i)packet[3+i]=(uint8_t)i;
    sco_handler(3,0,packet,(uint16_t)(length+3));
}
static void indicator(const char *name,uint8_t value) {
    uint8_t event[40]={HCI_EVENT_HFP_META,0,HFP_SUBEVENT_AG_INDICATOR_STATUS_CHANGED};
    little_endian_store_16(event,3,0x20);event[6]=value;strcpy((char*)event+12,name);
    unsigned length=13+(unsigned)strlen(name);event[1]=(uint8_t)(length-2);hfp_handler(4,0,event,(uint16_t)length);
}

int main(void) {
    reset();phone_call_poll();assert(connects==1);
    bd_addr_t stranger={9,9,9,9,9,9};slc(0x21,stranger);assert(slc_releases==1&&!phone_call_sco_allowed(stranger));
    slc(0x21,phone_address);assert(slc_releases==2&&!phone_call_sco_allowed(phone_address));
    authorized=false;slc(0x20,phone_address);assert(slc_releases==3);authorized=true;
    slc(0x20,phone_address);assert(phone_call_sco_allowed(phone_address));
    assert(!phone_call_active()&&!phone_call_media_control(AVRCP_OPERATION_ID_PLAY)&&answers==0);
    uint8_t gain[6]={HCI_EVENT_HFP_META,4,HFP_SUBEVENT_SPEAKER_VOLUME,0x20,0,6};
    hfp_handler(4,0,gain,6);assert(speaker==6&&microphone==15);
    gain[2]=HFP_SUBEVENT_MICROPHONE_VOLUME;gain[5]=0;hfp_handler(4,0,gain,6);
    assert(speaker==6&&microphone==0);
    gain[5]=16;hfp_handler(4,0,gain,6);assert(microphone==0);
    gain[3]=0x21;gain[5]=15;hfp_handler(4,0,gain,6);assert(microphone==0);
    meta(HFP_SUBEVENT_RING,0x21);assert(!phone_call_active());
    meta(HFP_SUBEVENT_RING,0x20);assert(phone_call_active()&&answers==0);
    assert(phone_call_media_control(AVRCP_OPERATION_ID_PLAY)&&answers==1);
    assert(phone_call_media_control(AVRCP_OPERATION_ID_PLAY)&&answers==1);
    now+=2000;phone_call_poll();indicator("call",1);indicator("callsetup",0);
    assert(phone_call_media_control(AVRCP_OPERATION_ID_PAUSE)&&hangs==1);
    assert(!phone_call_media_control(AVRCP_OPERATION_ID_FORWARD));
    indicator("call",0);assert(!phone_call_active());

    reset();slc(0x20,phone_address);audio(HFP_CODEC_CVSD);
    assert(sample_rate==8000&&phone_call_audio_requested()&&phone_call_mic_requested());
    phone_call_poll();assert(opens==1&&requests==1&&polls==1&&sends==0);
    phone_call_poll();assert(requests==1&&opens==2);
    sco(0x31,0,60);sco(0x30,0,59);assert(downlinks==0);
    sco(0x30,0,60);assert(downlinks==1&&last_downlink_frames==30&&!last_bad&&last_downlink[0]==0x100);
    sco(0x30,2,60);assert(downlinks==2&&last_bad);
    assert(call.received==2&&call.bad_frames==1);
    uint8_t malformed[5]={0x30,0,60,0,0};sco_handler(3,0,malformed,5);assert(downlinks==2);
    send_now(0x31);assert(sends==0);can_send=false;send_now(0x30);assert(sends==0&&uplinks==0);
    can_send=true;phone_call_poll();assert(requests==2);send_result=1;send_now(0x30);
    assert(sends==1&&uplinks==1&&call.pending_length==60&&last_sent[3]==0&&last_sent[4]==0x12);
    uint8_t saved[63];memcpy(saved,last_sent,63);send_result=0;phone_call_poll();send_now(0x30);
    assert(sends==2&&uplinks==1&&call.pending_length==0&&!memcmp(saved,last_sent,63));
    usb_recording=true;phone_call_poll();assert(!phone_call_audio_requested()&&sco_releases==1&&stops==1&&sample_rate==0);
    assert(!phone_call_sco_allowed(phone_address));
    usb_recording=false;audio(HFP_CODEC_CVSD);phone_call_poll();mic_active=true;
    uint8_t released[7]={HCI_EVENT_HFP_META,5,HFP_SUBEVENT_AUDIO_CONNECTION_RELEASED,0x20,0,0x30,0};
    hfp_handler(4,0,released,7);assert(phone_call_active()&&!phone_call_audio_requested());
    unsigned resumes_before=media_resume_polls;phone_call_poll();assert(media_resume_polls==resumes_before);
    mic_active=false;phone_call_poll();assert(!phone_call_active()&&media_resume_polls==resumes_before+1);
    meta(HFP_SUBEVENT_RING,0x20);phone_call_poll();assert(media_resume_polls==resumes_before+1);
    meta(HFP_SUBEVENT_STOP_RINGING,0x20);usb_playing=true;phone_call_poll();assert(media_resume_polls==resumes_before+1);

    reset();slc(0x20,phone_address);audio(HFP_CODEC_CVSD);assert(sample_rate==8000&&call_changes==1);
    audio(HFP_CODEC_CVSD);assert(sample_rate==8000&&call_changes==1);
    authorized=false;phone_call_poll();assert(!phone_call_audio_requested()&&slc_releases==1&&sco_releases==1);
    reset();slc(0x20,phone_address);audio(HFP_CODEC_MSBC);assert(!phone_call_audio_requested()&&sco_releases==1);
    reset();slc(0x20,phone_address);audio(HFP_CODEC_CVSD);output_ready=false;phone_call_poll();
    now=8000;phone_call_poll();assert(!phone_call_audio_requested()&&sco_releases==1);
    reset();slc(0x20,phone_address);audio(HFP_CODEC_CVSD);phone_call_poll();
    now=60000;microphone_samples+=480;sco(0x30,0,60);phone_call_poll();assert(phone_call_audio_requested());
    output_ready=false;now+=100;sco(0x30,0,60);phone_call_poll();assert(phone_call_audio_requested());
    output_ready=true;now+=100;sco(0x30,0,60);phone_call_poll();assert(phone_call_audio_requested());
    output_ready=false;now+=8000;sco(0x30,0,60);phone_call_poll();assert(!phone_call_audio_requested()&&sco_releases==1);
    reset();slc(0x20,phone_address);audio(HFP_CODEC_CVSD);phone_call_poll();
    now=7900;sco(0x30,0,60);phone_call_poll();assert(phone_call_audio_requested());
    microphone_samples=UINT32_MAX;phone_call_poll();now+=7000;microphone_samples=0;
    sco(0x30,0,60);phone_call_poll();assert(phone_call_audio_requested());
    now+=8000;sco(0x30,0,60);phone_call_poll();assert(!phone_call_audio_requested()&&sco_releases==1);
    reset();slc(0x20,phone_address);audio(HFP_CODEC_CVSD);phone_call_poll();send_result=1;
    for(unsigned i=0;i<3;++i){send_now(0x30);phone_call_poll();}
    assert(sco_releases==1&&!phone_call_audio_requested()&&uplinks==1);
    reset();now=UINT32_MAX-5000u;phone_call_poll();assert(connects==1);
    now+=10000;phone_call_poll();assert(slc_releases==1&&connects==1);
    for(unsigned i=0;i<2;++i){now+=30000;phone_call_poll();now+=10000;phone_call_poll();}
    now+=30000;phone_call_poll();assert(connects==3);
    // A normal SLC release retains the reconnect backoff for the same live ACL.
    reset();slc(0x20,phone_address);
    meta(HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED,0x20);
    phone_call_poll();assert(connects==0);
    now=29999;phone_call_poll();assert(connects==0);
    now=30000;phone_call_poll();assert(connects==1);
    // A saved phone can be away at boot. Its later authenticated ACL starts a
    // fresh bounded retry epoch, even if the controller reused its handle.
    reset();authorized=acl_connected=false;
    for(unsigned i=0;i<3;++i){phone_call_poll();now+=10000;phone_call_poll();now+=30000;}
    phone_call_poll();assert(connects==3);
    authorized=acl_connected=true;phone_call_poll();assert(connects==4);
    now+=10000;phone_call_poll();
    for(unsigned i=0;i<2;++i){now+=30000;phone_call_poll();now+=10000;phone_call_poll();}
    now+=30000;phone_call_poll();assert(connects==6);
    authorized=acl_connected=false;phone_call_poll();
    authorized=acl_connected=true;phone_call_poll();assert(connects==7);
    // USB ownership prevents dialing, but must not hide the ACL epoch change.
    reset();authorized=acl_connected=false;
    for(unsigned i=0;i<3;++i){phone_call_poll();now+=10000;phone_call_poll();now+=30000;}
    usb_playing=authorized=acl_connected=true;phone_call_poll();assert(connects==3);
    usb_playing=false;phone_call_poll();assert(connects==4);
    // A different authenticated handle also renews retries without requiring an
    // observed disconnected pass; repeated polls cannot keep renewing it.
    reset();
    for(unsigned i=0;i<3;++i){phone_call_poll();now+=10000;phone_call_poll();now+=30000;}
    phone_connection.con_handle=0x21;phone_call_poll();assert(connects==4);
    phone_call_poll();assert(connects==4);
    // Selecting another saved phone also renews the budget; it cannot inherit
    // the previous phone's permanent lockout or reconnect backoff.
    reset();authorized=acl_connected=false;
    for(unsigned i=0;i<3;++i){phone_call_poll();now+=10000;phone_call_poll();now+=30000;}
    phone_call_poll();assert(connects==3);
    saved_phone_address[0]=9;phone_call_poll();assert(connects==4);
    reset();slc(0x20,phone_address);
    assert(!phone_call_usb_exclusion_requested());
    meta(HFP_SUBEVENT_RING,0x20);indicator("call",1);
    assert(!phone_call_usb_exclusion_requested()); // Indicators also arrive for calls routed over USB.
    audio(HFP_CODEC_CVSD);
    assert(phone_call_usb_exclusion_requested());
    usb_recording=true;
    assert(!phone_call_usb_exclusion_requested());
    usb_recording=false;authorized=false;
    assert(!phone_call_usb_exclusion_requested());
    authorized=true;mic_active=true;audio_stop(false);
    assert(phone_call_usb_exclusion_requested());
    mic_active=false;phone_call_poll();
    assert(!phone_call_usb_exclusion_requested());
    puts("phone calls: authenticated HFP/CVSD, explicit stems, bounded SCO, microphone progress, USB priority and cleanup passed");
}
