// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/btstack/playback_bitrate.h"
#include "../src/btstack/playback_options.h"
#include "../src/btstack/playback_trace.h"
#include "../src/btstack/playback_adjust.h"
#include "../src/btstack/playback_continuity.h"

enum { AACENC_BITRATE, AACENC_PEAK_BITRATE, AACENC_BITRATEMODE, AACENC_SAMPLERATE };
#define AACENC_OK 0
typedef struct { unsigned inputChannels, frameLength, confSize; uint8_t confBuf[64]; } AACENC_InfoStruct;
static AACENC_InfoStruct aacinf;
static void *handleAAC;
static playback_options_t playback_options;
static playback_bitrate_t playback_bitrate;
static playback_trace_t playback_trace;
static playback_game_t playback_game;
static playback_usb_verification_t usb_game_verification;
static playback_adjust_t playback_adjust;
static uint32_t aaceld_bitrate, bitrate_pressure, bitrate_discontinuities, bitrate_radio_epoch, now;
static uint8_t bitrate_input;
static bool bitrate_game_requested;
static uint16_t aaceld_frame_sequence;
static unsigned params[4], sets, initializes, fail_set, fail_init, bad_info, fail_rollback;
static bool a2dp_is_connected_flag, is_usb_streaming, phone_active, call_active, call_opened;
static bool profile_pending, a2dp_connect_pending, mic_prepare_open, mic_recovery_pending;
static bool phone_output_recovery, suspend_recovery_pending, playback_transport_stale;
static bool warm, owned, mic_requested, mic_active;
enum { PLAYBACK_RUNNING, PLAYBACK_SUSPENDING };
static unsigned playback_idle_phase;
typedef struct { bool streaming; uint16_t codec_storage_count,codec_num_frames;
    uint8_t codec_ready_to_send; uint32_t samples_ready,rtp_timestamp,time_audio_data_sent,acc_num_missed_samples;
    unsigned audio_timer; } a2dp_media_sending_context_t;
static a2dp_media_sending_context_t media_tracker;
static unsigned cur_codec;
typedef struct { uint32_t epoch, flush_events; } bt_hci_radio_diagnostics_t;
static bt_hci_radio_diagnostics_t radio_state;
static bool radio_known, radio_active;
static playback_continuity_t phone_continuity;
static bool is_streaming,recovery_reset_encoder;
static unsigned suspend_recovery_timer;
static bool playback_pcm_warm(void) {return warm;}
static bool aacp_audio_owned(void) {return owned;}
static bool aacp_mic_requested(void) {return mic_requested;}
static bool aacp_mic_active(void) {return mic_active;}
static void playback_usb_verify_cancel(void) {memset(&usb_game_verification,0,sizeof(usb_game_verification));}
static void btstack_run_loop_remove_timer(void *timer) {(void)timer;}
static void playback_idle_reset(void) {playback_idle_phase=PLAYBACK_RUNNING;}
static uint32_t btstack_run_loop_get_time_ms(void) {return now;}
static void bt_hci_radio_active(bool active) {radio_active=active;}
static bool bt_hci_radio_diagnostics(bt_hci_radio_diagnostics_t *out) {*out=radio_state;return radio_known;}
static void dongle_put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint32_t get32(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned aacEncoder_SetParam(void *handle,unsigned key,unsigned value) {
    assert(handle==handleAAC && key<4);++sets;
    if(fail_set && sets==fail_set)return 1;
    if(fail_rollback && value==256000)return 1;
    params[key]=value;return 0;
}
static unsigned aacEncoder_GetParam(void *handle,unsigned key) {assert(handle==handleAAC);return params[key];}
static unsigned aacEncEncode(void *handle,void *a,void *b,void *c,void *d) {
    assert(handle==handleAAC && !a && !b && !c && !d);++initializes;
    return fail_init && initializes==fail_init;
}
static unsigned aacEncInfo(void *handle,AACENC_InfoStruct *out) {
    assert(handle==handleAAC);*out=aacinf;
    if(bad_info && initializes==bad_info)out->confBuf[0]^=1;
    return 0;
}
#include "playback_bitrate.inc"

static void fixture(bool variable,uint32_t ceiling) {
    memset(&playback_trace,0,sizeof(playback_trace));
    memset(&playback_game,0,sizeof(playback_game));
    memset(&usb_game_verification,0,sizeof(usb_game_verification));
    memset(&playback_adjust,0,sizeof(playback_adjust));
    aacinf=(AACENC_InfoStruct){.inputChannels=2,.frameLength=480,.confSize=4,.confBuf={0xf8,0xe6,0x50,0}};
    handleAAC=(void *)1;playback_options=(playback_options_t){.codec=3,.rate_mode=variable};
    aaceld_bitrate=ceiling;playback_bitrate_reset(&playback_bitrate,ceiling,variable);
    params[AACENC_BITRATE]=params[AACENC_PEAK_BITRATE]=ceiling;
    params[AACENC_BITRATEMODE]=variable?5:0;params[AACENC_SAMPLERATE]=48000;
    sets=initializes=fail_set=fail_init=bad_info=fail_rollback=0;
    bitrate_pressure=bitrate_discontinuities=bitrate_radio_epoch=now=0;
    bitrate_input=0;bitrate_game_requested=false;
    a2dp_is_connected_flag=is_usb_streaming=warm=owned=true;
    phone_active=call_active=call_opened=profile_pending=a2dp_connect_pending=false;
    mic_prepare_open=mic_recovery_pending=mic_requested=mic_active=false;
    phone_output_recovery=suspend_recovery_pending=playback_transport_stale=false;
    playback_idle_phase=PLAYBACK_RUNNING;cur_codec=4;
    media_tracker=(a2dp_media_sending_context_t){.streaming=true,.samples_ready=960,.rtp_timestamp=123456};
    aaceld_frame_sequence=1999;
    radio_state=(bt_hci_radio_diagnostics_t){.epoch=7};radio_known=true;radio_active=false;
}
static void ready(void) {
    playback_bitrate_poll(now);++now;playback_bitrate_poll(now);
    now+=3000;playback_bitrate_poll(now);
    assert(playback_bitrate.active && !playback_bitrate.frozen);
}
static void window(uint32_t flushes,uint32_t pressure) {
    now+=1000;radio_state.flush_events+=flushes;bitrate_pressure+=pressure;playback_bitrate_poll(now);
}
static void test_live_updates_and_queued_old_units(void) {
    fixture(false,256000);ready();window(1,0);assert(playback_bitrate.target==256000);
    window(1,0);assert(playback_bitrate.target==220000 && playback_bitrate.current==256000);
    media_tracker.codec_storage_count=648;
    assert(playback_bitrate_apply(&media_tracker) && !sets); // Already queued AUs retain their original ceiling.
    media_tracker.codec_storage_count=0;
    assert(playback_bitrate_apply(&media_tracker));
    assert(playback_bitrate.current==220000 && playback_bitrate.changes==1 && initializes==1);
    assert(handleAAC==(void *)1 && params[AACENC_PEAK_BITRATE]==220000 && aacinf.frameLength==480);
    assert(aaceld_frame_sequence==1999 && media_tracker.rtp_timestamp==123456 && media_tracker.samples_ready==960);
    assert(playback_eld_au_ceiling(220000,false)==276 && playback_eld_au_ceiling(150400,false)==189);
    assert(playback_eld_au_ceiling(128000,true)==160);
    window(3,0);assert(playback_bitrate.target==192000);assert(playback_bitrate_apply(&media_tracker));
    window(3,0);assert(playback_bitrate.target==174000);assert(playback_bitrate_apply(&media_tracker));
    window(3,0);assert(playback_bitrate.target==128000);assert(playback_bitrate_apply(&media_tracker));
    for(unsigned i=0;i<10;++i)window(4,5);
    assert(playback_bitrate.current==128000 && playback_bitrate.target==128000 && playback_bitrate.changes==4);
    for(unsigned i=0;i<29;++i)window(0,0);
    assert(playback_bitrate.target==128000);
    window(0,0);assert(playback_bitrate.target==174000);assert(playback_bitrate_apply(&media_tracker));
    assert(playback_bitrate.current==174000);
}
static void test_vbr_limits_freezes_and_pressure_without_radio_queries(void) {
    fixture(true,173600);ready();window(3,0);assert(playback_bitrate.target==128000);
    mic_requested=true;assert(playback_bitrate_apply(&media_tracker) && !sets);
    playback_bitrate_poll(++now);assert(playback_bitrate.frozen && !radio_active && playback_bitrate.target==173600);
    mic_requested=false;ready(); // The live ACL remains known even if all optional RF queries are unsupported.
    window(0,2);window(0,2);assert(playback_bitrate.target==128000);
    assert(playback_bitrate_apply(&media_tracker) && playback_bitrate.current==128000);
    for(unsigned i=0;i<30;++i)window(0,0);
    assert(playback_bitrate.target==173600);assert(playback_bitrate_apply(&media_tracker));
    assert(playback_options.rate_mode==1 && playback_options.bitrate==0 && aaceld_bitrate==173600);
    playback_game.phase=GAME_PENDING;playback_bitrate_poll(++now);assert(playback_bitrate.frozen);
    playback_game.phase=GAME_CONFIRMED;ready();
    ++playback_trace.stats[TRACE_ACTIVE_ZERO];playback_bitrate_poll(++now);assert(playback_bitrate.frozen);
    ready();window(3,0);++playback_trace.stats[TRACE_ACTIVE_DROP];
    assert(playback_bitrate_apply(&media_tracker) && playback_bitrate.current==173600);
    playback_bitrate_poll(++now);assert(playback_bitrate.target==173600);
    owned=false;playback_bitrate_poll(++now);assert(!radio_active && !playback_bitrate.active);
}
static void test_errors_rollback_and_cap_validation(void) {
    fixture(false,256000);
    assert(playback_eld_rate_update(256000,256800)==1 && !sets);
    assert(playback_eld_rate_update(256000,127200)==1 && !sets);
    assert(playback_eld_rate_update(256000,256000)==0 && !sets);
    fail_set=2;assert(playback_eld_rate_update(256000,192000)==1);
    assert(params[AACENC_BITRATE]==256000 && params[AACENC_PEAK_BITRATE]==256000);
    fixture(false,256000);fail_init=1;assert(playback_eld_rate_update(256000,192000)==1);
    assert(params[AACENC_PEAK_BITRATE]==256000 && initializes==2);
    fixture(false,256000);bad_info=1;assert(playback_eld_rate_update(256000,192000)==1);
    assert(params[AACENC_PEAK_BITRATE]==256000 && aacinf.confBuf[0]==0xf8);
    fixture(false,256000);fail_init=1;fail_rollback=1;
    assert(playback_eld_rate_update(256000,192000)==-1);
    fixture(true,173600);assert(playback_eld_rate_update(173600,174000)==1 && !sets);
    assert(playback_eld_rate_update(173600,160001)==1 && !sets);
    fixture(false,256000);ready();window(3,0);fail_set=1;
    assert(playback_bitrate_apply(&media_tracker) && playback_bitrate.fault && playback_bitrate.errors==1);
    unsigned before=sets;for(unsigned i=0;i<20;++i){window(4,4);assert(playback_bitrate_apply(&media_tracker));}
    assert(sets==before && playback_bitrate.current==256000);
}
static void test_jitter_wrap_pause_reconnect_and_read_only_snapshot(void) {
    fixture(false,220000);now=UINT32_MAX-2000u;ready();
    for(unsigned i=0;i<60;++i){now+=17;playback_bitrate_poll(now);}
    assert(playback_bitrate.current==220000 && !playback_bitrate.frozen);
    radio_state.flush_events=UINT32_MAX;playback_bitrate.last_flushes=UINT32_MAX;
    window(1,0);window(1,0);assert(playback_bitrate.target==192000);
    is_usb_streaming=false;playback_bitrate_poll(++now);assert(!playback_bitrate.active && playback_bitrate.target==220000);
    is_usb_streaming=true;ready();window(3,0);assert(playback_bitrate_apply(&media_tracker));
    ++radio_state.epoch;playback_bitrate_poll(++now);assert(playback_bitrate.frozen && playback_bitrate.current==192000);
    uint8_t out[64];playback_bitrate_t before=playback_bitrate;
    assert(!dongle_playback_adaptive_snapshot(NULL,64) && !dongle_playback_adaptive_snapshot(out,63));
    assert(dongle_playback_adaptive_snapshot(out,64)==64 && out[0]==1 && get32(out+4)==192000 && get32(out+8)==220000);
    assert(!memcmp(&before,&playback_bitrate,sizeof(before)));
    cur_codec=1;assert(dongle_playback_adaptive_snapshot(out,64)==64 && get32(out+4)==0 && get32(out+8)==0 && !(out[1]&1));
    fixture(false,128000);ready();for(unsigned i=0;i<12;++i)window(5,5);
    assert(playback_bitrate.target==128000 && !sets);
    assert(sizeof(playback_bitrate_t)<=64);
}
static void test_repeated_radio_recovery_can_lower_but_pcm_starvation_cannot(void) {
    fixture(false,256000);ready();
    for(unsigned i=0;i<8;++i) {
        unsigned before=sets;
        now+=200;++radio_state.flush_events;
        playback_bitrate_transport_stalled(); // The sender/200-ms timeout captures pressure before stopping.
        assert(playback_bitrate.urgent);
        a2dp_demo_timer_stop(&media_tracker); // Execute the actual recovery stop, including streaming=false.
        ++playback_trace.stats[TRACE_RECOVERIES];
        playback_bitrate_poll(now);
        assert(playback_bitrate.frozen && playback_bitrate.urgent);
        assert(playback_bitrate_apply(&media_tracker) && sets==before);
        media_tracker.streaming=true;warm=false;
        assert(playback_bitrate_apply(&media_tracker) && sets==before);
        warm=true;media_tracker.codec_storage_count=20;
        assert(playback_bitrate_apply(&media_tracker) && sets==before);
        media_tracker.codec_storage_count=0;
        assert(playback_bitrate_apply(&media_tracker));
        assert(sets>before && !playback_bitrate.urgent);
        if(playback_bitrate.current==128000)break;
    }
    assert(playback_bitrate.current==128000 && playback_bitrate.changes==4);
    fixture(false,256000);ready();
    for(unsigned i=0;i<50;++i) {
        now+=200;++playback_trace.stats[TRACE_ACTIVE_ZERO];++playback_trace.stats[TRACE_RECOVERIES];
        bitrate_pressure+=2;playback_bitrate_poll(now);assert(playback_bitrate_apply(&media_tracker));
    }
    assert(playback_bitrate.current==256000 && !playback_bitrate.urgent && !sets);
    bool *blocked[]={&mic_prepare_open,&mic_recovery_pending,&playback_game.restore_failed};
    for(unsigned i=0;i<sizeof(blocked)/sizeof(blocked[0]);++i) {
        *blocked[i]=true;++radio_state.flush_events;++playback_trace.stats[TRACE_RECOVERIES];
        playback_bitrate_poll(++now);assert(!playback_bitrate.urgent);
        *blocked[i]=false;ready();
    }
    playback_bitrate_transport_stalled();playback_bitrate_poll(++now);assert(playback_bitrate.urgent);
    mic_requested=true;playback_bitrate_poll(++now);
    assert(!playback_bitrate.urgent && playback_bitrate_apply(&media_tracker) && !sets);
    mic_requested=false;ready();playback_bitrate_transport_stalled();assert(playback_bitrate.urgent);
    playback_game.requested=true;playback_game.phase=GAME_PENDING;playback_bitrate_poll(++now);
    assert(!playback_bitrate.urgent && playback_bitrate_apply(&media_tracker) && !sets);
    playback_game.phase=GAME_CONFIRMED;ready();playback_bitrate_transport_stalled();assert(playback_bitrate.urgent);
    is_usb_streaming=false;phone_active=true;playback_bitrate_poll(++now);assert(!playback_bitrate.urgent);
    ready();playback_bitrate_transport_stalled();assert(playback_bitrate.urgent);
    ++radio_state.epoch;playback_bitrate_poll(++now);
    assert(!playback_bitrate.urgent && playback_bitrate.target==256000);
    radio_known=false;playback_bitrate_transport_stalled();assert(!playback_bitrate.urgent);
}
int main(void) {
    test_live_updates_and_queued_old_units();
    test_vbr_limits_freezes_and_pressure_without_radio_queries();
    test_errors_rollback_and_cap_validation();
    test_jitter_wrap_pause_reconnect_and_read_only_snapshot();
    test_repeated_radio_recovery_can_lower_but_pcm_starvation_cannot();
    puts("live ELD bitrate: bounded pressure response, slow recovery, freezes, encoder rollback and read-only diagnostics passed");
}
