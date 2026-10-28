// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/btstack/playback_pcm.h"
#include "../src/btstack/playback_eld.h"
#include "../src/btstack/playback_options.h"
#include "../src/btstack/playback_adjust.h"
#include "../src/btstack/playback_usb_clock.h"
#include "../src/btstack/phone_clock.h"
static playback_usb_clock_t usb_clock;
static uint32_t usb_recovery_drops;
static uint32_t media_last_sent_us,packet_oldest_us;
static bool playback_transport_stale;
static playback_adjust_t playback_adjust;

#define ERROR_CODE_SUCCESS 0
#define PLAYBACK_IDLE_MS 1000u
#define PLAYBACK_TRANSITION_MS 3000u
#define SUSPEND_RECOVERY_TIMEOUT_MS 3000u
#define AVDTP_SI_START 1
#define AVDTP_SI_SUSPEND 2
enum { PLAYBACK_RUNNING, PLAYBACK_SUSPENDING, PLAYBACK_SUSPENDED, PLAYBACK_STARTING };
static uint8_t playback_idle_phase, cur_codec = 4;
static bool playback_idle_counting, phone_opened, phone_active, phone_buffer_started;
static bool call_active, call_opened, call_capture;
static bool phone_output_recovery;
static uint16_t phone_output_recovery_cid;
static uint32_t phone_output_recovery_since;
static uint8_t phone_output_recovery_peer[6], cur_active_device[6], selected_peer[6];
static phone_clock_t phone_clock;
static unsigned phone_resampler, acc_num_simples=480;
static void btstack_resample_init(void *resampler,unsigned channels) {(void)resampler;assert(channels==2);}
static const uint8_t *get_device_addr(void) {return selected_peer;}
static bool peer_address_valid(const uint8_t *peer) {return peer && peer[0];}
void bridge_phone_stop(void);
void bridge_call_stop(void);
static unsigned phone_yields;
static void phone_sink_yield(void) {++phone_yields;bridge_phone_stop();}
static void phone_sink_pause_for_call(void) {phone_sink_yield();}
static void avdtp_source_establish_stream(void) {}
static bool is_usb_streaming, is_streaming, a2dp_is_connected_flag, finish_scan_avdtp_codec;
static bool profile_pending, a2dp_connect_pending;
static bool recovery_reset_encoder, suspend_recovery_pending, mic_requested, mic_active;
static bool mic_recovery_pending, mic_recovery_started;
static uint16_t mic_recovery_cid;
static uint32_t mic_recovery_revision;
static uint32_t playback_idle_since, playback_idle_transition_since, now;
static unsigned suspends, starts, closes, resets;
static uint8_t suspend_error, start_error;
static bool encoder_ready = true;
static bool game_eligible;
static playback_game_t playback_game;
static bool playback_game_eligible(void) { return game_eligible; }
static struct { unsigned recoveries; } playback_metrics;
static void *handleAAC = (void *)1;
static uint32_t aaceld_bitrate = 256000;
static uint16_t pcm_frame_samples = 480, active_slot_count = 16;
static unsigned audio_timer_interval = 10;
static playback_pcm_t playback_pcm;
static uint32_t audio_slot_capacity(void) {return pcm_frame_samples*active_slot_count;}
static unsigned playback_pcm_lock;
typedef struct timer { bool active; void *context; } btstack_timer_source_t;
static btstack_timer_source_t suspend_recovery_timer;
typedef struct {
    uint16_t avdtp_cid;
    uint8_t local_seid;
    int max_media_payload_size;
    unsigned codec_num_frames, codec_storage_count;
    uint32_t time_audio_data_sent, samples_ready, acc_num_missed_samples;
    bool codec_ready_to_send, streaming;
    btstack_timer_source_t audio_timer;
} a2dp_media_sending_context_t;
static a2dp_media_sending_context_t media_tracker;
static void critical_section_enter_blocking(void *p) { (void)p; }
static void critical_section_exit(void *p) { (void)p; }
static uint32_t btstack_run_loop_get_time_ms(void) {return now;}
static void btstack_run_loop_remove_timer(btstack_timer_source_t *t) { t->active = false; }
static void btstack_run_loop_set_timer(btstack_timer_source_t *t, unsigned delay) { (void)t; (void)delay; }
static void btstack_run_loop_add_timer(btstack_timer_source_t *t) { t->active = true; }
static void btstack_run_loop_set_timer_context(btstack_timer_source_t *t, void *context) { t->context = context; }
static void btstack_run_loop_set_timer_handler(btstack_timer_source_t *t, void (*handler)(btstack_timer_source_t *)) { (void)t; (void)handler; }
static void avdtp_audio_timeout_handler(btstack_timer_source_t *t) { (void)t; }
static int avdtp_max_media_payload_size(unsigned cid, unsigned seid) { assert(cid == 10 && seid == 5); return 986; }
static void aacp_disconnect(void) { }
static unsigned a2dp_source_disconnect(unsigned cid) { assert(cid == 10); ++closes; return 0; }
static unsigned avdtp_source_suspend(unsigned cid, unsigned seid) { assert(cid == 10 && seid == 5); ++suspends; return suspend_error; }
static unsigned avdtp_source_start_stream(unsigned cid, unsigned seid) { assert(cid == 10 && seid == 5); ++starts; return start_error; }
static bool aaceld_encoder_init(void) { ++resets; return encoder_ready; }
static bool usb_mic_requested(void) { return mic_requested; }
static bool aacp_mic_requested(void) { return mic_requested || call_capture; }
static bool aacp_mic_active(void) { return mic_active; }
static uint8_t avdtp_subevent_signaling_accept_get_local_seid(const uint8_t *p) { return *p; }
static void set_led_mode_on(void) { }
bool bridge_phone_recovering(void);
#include "playback_idle.inc"

static void accept(unsigned signal, uint8_t seid) {
    const uint8_t *packet = &seid;
    switch (signal) {
#include "playback_accept.inc"
        default: assert(false);
    }
}
static void fixture(void) {
    playback_idle_reset();
    memset(&media_tracker, 0, sizeof(media_tracker));
    media_tracker.avdtp_cid = 10; media_tracker.local_seid = 5;
    media_tracker.streaming = is_streaming = a2dp_is_connected_flag = finish_scan_avdtp_codec = true;
    media_tracker.audio_timer.active = true;
    assert(playback_pcm_reset(&playback_pcm, 7680, false));
    playback_pcm.primed_on_wire = true;
    now = suspends = starts = closes = resets = 0;
    suspend_error = start_error = 0; encoder_ready = true;
    recovery_reset_encoder = suspend_recovery_pending = false;
    profile_pending = a2dp_connect_pending = false;
    is_usb_streaming = phone_opened = phone_active = mic_requested = mic_active = false;
    call_active = call_opened = call_capture = false;
    phone_yields=0;
    phone_output_recovery=false;
    for(unsigned i=0;i<6;++i)selected_peer[i]=cur_active_device[i]=(uint8_t)(i+1);
    cur_codec = 4;
    game_eligible=false; playback_game=(playback_game_t){0};
}
static void suspend_idle(void) {
    playback_idle_poll(now);
    now += 999; playback_idle_poll(now); assert(!suspends);
    ++now; playback_idle_poll(now); assert(suspends == 1);
    assert(is_streaming && media_tracker.streaming && playback_idle_phase == PLAYBACK_SUSPENDING);
    accept(AVDTP_SI_SUSPEND, 6); assert(media_tracker.streaming);
    accept(AVDTP_SI_SUSPEND, 5);
    assert(!is_streaming && !media_tracker.streaming && !media_tracker.audio_timer.active);
    assert(playback_idle_phase == PLAYBACK_SUSPENDED);
}
static void usb_onset_survives_restart(void) {
    fixture(); suspend_idle();
    for(unsigned i=0;i<PLAYBACK_FILTER_TAPS*2;++i)playback_pcm.history[i]=0.5f;
    int16_t onset[960], output[960];
    for (unsigned i = 0; i < 960; ++i) onset[i] = (int16_t)(i + 1);
    set_usb_streaming(true); playback_pcm_push(&playback_pcm, onset, 480);
    playback_idle_poll(++now);
    assert(starts == 1 && resets == 1 && !media_tracker.streaming);
    assert(playback_idle_phase == PLAYBACK_STARTING);
    accept(AVDTP_SI_START, 6); assert(!media_tracker.streaming);
    accept(AVDTP_SI_START, 5);
    assert(media_tracker.streaming && is_streaming && playback_idle_phase == PLAYBACK_RUNNING);
    assert(playback_pcm.priming == 4800 && playback_pcm.used == 480);
    for(unsigned i=0;i<PLAYBACK_FILTER_TAPS*2;++i)assert(playback_pcm.history[i]==0.0f);
    for (unsigned i = 0; i < 10; ++i) {
        assert(playback_pcm_read(&playback_pcm, output, 480) == 0);
        for (unsigned j = 0; j < 960; ++j) assert(output[j] == 0);
    }
    assert(playback_pcm_read(&playback_pcm, output, 480) == 480);
    assert(memcmp(onset, output, sizeof(onset)) == 0);
}
static void phone_recovery_start(void) {
    fixture(); phone_opened=phone_active=phone_buffer_started=true;
    playback_transport_stale=true;
    assert(!playback_phone_recovery_poll());
    assert(suspends==1 && !media_tracker.streaming && bridge_phone_recovering());
    playback_transport_recover(&media_tracker);
    assert(suspends==1); // One confirmed boundary at a time.
}

static void phone_output_recovers_without_losing_input(void) {
    phone_recovery_start();
    assert(!playback_phone_recovery_poll());
    accept(AVDTP_SI_SUSPEND,6); assert(!starts && bridge_phone_recovering());
    now=10; accept(AVDTP_SI_SUSPEND,5);
    assert(starts==1 && resets==1 && bridge_phone_recovering());
    playback_pcm.used=777;
    now=20; accept(AVDTP_SI_START,5);
    assert(media_tracker.streaming && phone_active && bridge_phone_recovering());
    assert(playback_pcm.capacity==PHONE_PCM_CAPACITY && !playback_pcm.used && !phone_buffer_started);
    assert(playback_pcm.priming==4800 && !bridge_phone_ready());
    int16_t output[960];
    for(unsigned i=0;i<10;++i) {
        assert(playback_pcm_read(&playback_pcm,output,480)==0);
        playback_pcm_sent(&playback_pcm);
    }
    assert(bridge_phone_ready() && !playback_phone_recovery_poll() && !phone_output_recovery);
    assert(phone_active && !closes);

    phone_recovery_start(); now=2999;
    assert(bridge_phone_recovering() && !playback_phone_recovery_poll());
    now=3000; assert(!bridge_phone_recovering());
    assert(playback_phone_recovery_poll() && closes==1 && !phone_output_recovery);

    for(unsigned reason=0;reason<8;++reason) {
        phone_recovery_start();
        switch(reason) {
            case 0: is_usb_streaming=true;break;
            case 1: mic_requested=true;break;
            case 2: mic_active=true;break;
            case 3: profile_pending=true;break;
            case 4: a2dp_connect_pending=true;break;
            case 5: ++media_tracker.avdtp_cid;break;
            case 6: ++selected_peer[0];break;
            case 7: ++cur_active_device[0];break;
        }
        assert(!bridge_phone_recovering());
        assert(!playback_phone_recovery_poll() && !phone_output_recovery && !closes);
        if (reason>=5) {
            assert(!suspend_recovery_pending && !suspend_recovery_timer.active);
            suspend_recovery_handler(&suspend_recovery_timer);
            assert(!closes); // A stale timer cannot release a replacement output.
        } else assert(suspend_recovery_pending && suspend_recovery_timer.active);
        if (reason==0) {
            accept(AVDTP_SI_SUSPEND,5); accept(AVDTP_SI_START,5);
            assert(media_tracker.streaming && starts==1 && !closes && !phone_output_recovery);
        }
    }
    phone_recovery_start(); phone_output_recovery_since=UINT32_MAX-1000u;
    now=1998; assert(bridge_phone_recovering());
    now=1999; assert(!bridge_phone_recovering());

    phone_recovery_start(); now=10;accept(AVDTP_SI_SUSPEND,5);accept(AVDTP_SI_START,5);
    now=3000;assert(playback_phone_recovery_poll() && closes==1); // Warm-up is bounded too.
    phone_recovery_start();bridge_phone_stop();assert(!phone_output_recovery);
}

static void call_output_ownership(void) {
    fixture();phone_opened=phone_active=true;
    assert(bridge_call_open() && bridge_call_ready() && phone_yields==1);
    assert(call_active && !phone_active && !phone_opened && !phone_buffer_started);
    assert(phone_clock.target_q8==1080*256 && playback_pcm.capacity==8192);
    assert(bridge_call_open() && phone_yields==1);
    playback_pcm.used=1200;phone_buffer_started=true;
    bridge_phone_stop();bridge_phone_drop_pcm();
    assert(bridge_call_ready() && playback_pcm.used==1200 && phone_buffer_started);
    suspend_recovery_pending=recovery_reset_encoder=true;
    accept(AVDTP_SI_SUSPEND,5);assert(call_opened && call_active && !bridge_call_ready());
    accept(AVDTP_SI_START,5);
    assert(call_active && !playback_pcm.used && playback_pcm.priming==4800 && !phone_buffer_started);
    int16_t pcm[960];
    for(unsigned i=0;i<10;++i) {playback_pcm_read(&playback_pcm,pcm,480);playback_pcm_sent(&playback_pcm);}
    assert(bridge_call_ready());
    set_usb_streaming(true);
    assert(!call_active && !call_opened && !bridge_call_ready() && !bridge_call_open());
    fixture();mic_requested=true;assert(!bridge_call_open());
    fixture();assert(bridge_call_open());bridge_call_stop();assert(!call_opened && !call_active);
}

int main(void) {
    call_output_ownership();
    phone_output_recovers_without_losing_input();
    usb_onset_survives_restart();
    fixture(); suspend_recovery_pending=recovery_reset_encoder=true; is_usb_streaming=true;
    mic_recovery_pending=mic_recovery_started=true; mic_recovery_cid=10; mic_recovery_revision=4;
    accept(AVDTP_SI_SUSPEND,5);
    assert(mic_recovery_pending && mic_recovery_revision==4 && playback_idle_phase==PLAYBACK_STARTING);
    accept(AVDTP_SI_START,6); assert(mic_recovery_pending && mic_recovery_revision==4);
    accept(AVDTP_SI_START,5); assert(!mic_recovery_pending && mic_recovery_revision==5);
    accept(AVDTP_SI_START,5); assert(mic_recovery_revision==5);
    fixture(); suspend_recovery_pending=recovery_reset_encoder=true; is_usb_streaming=true;
    mic_recovery_pending=mic_recovery_started=true; mic_recovery_cid=11;
    accept(AVDTP_SI_SUSPEND,5); accept(AVDTP_SI_START,5);
    assert(mic_recovery_pending && mic_recovery_revision==5); // Wrong-CID repair cannot complete.
    mic_recovery_pending=mic_recovery_started=false;
    fixture();suspend_recovery_pending=recovery_reset_encoder=true;is_usb_streaming=true;
    accept(AVDTP_SI_SUSPEND,5);
    assert(resets==1 && starts==1 && playback_idle_phase==PLAYBACK_STARTING && !media_tracker.streaming);
    playback_idle_poll(3000);
    assert(closes==1 && !finish_scan_avdtp_codec); // Missing recovery Start cannot strand audio.
    fixture();suspend_recovery_pending=recovery_reset_encoder=true;is_usb_streaming=true;
    accept(AVDTP_SI_SUSPEND,5);accept(AVDTP_SI_START,5);
    assert(resets==1 && media_tracker.streaming && playback_idle_phase==PLAYBACK_RUNNING);
    fixture(); suspend_idle(); is_usb_streaming=game_eligible=true;
    playback_game_request(&playback_game,true);
    playback_idle_poll(++now); assert(starts==0);
    playback_game.phase=GAME_SENT;
    playback_idle_poll(++now); assert(starts==0);
    playback_game.phase=GAME_CONFIRMED;
    playback_idle_poll(++now); assert(starts==1);
    fixture(); suspend_idle(); is_usb_streaming=game_eligible=true;
    playback_game_request(&playback_game,true); playback_game.failed=true;
    playback_idle_poll(++now); assert(starts==1); // A failed preparation cannot strand audio.
    fixture(); suspend_idle(); is_usb_streaming=game_eligible=mic_requested=true;
    playback_game_request(&playback_game,true);
    playback_idle_poll(++now); assert(starts==1);
    for (unsigned codec = 1; codec <= 2; ++codec) {
        fixture(); cur_codec = (uint8_t)codec; suspend_idle();
        is_usb_streaming = true; playback_idle_poll(++now);
        accept(AVDTP_SI_START, 5);
        assert(is_streaming && !playback_pcm.priming && !resets);
    }
    for (unsigned keep = 0; keep < 5; ++keep) {
        fixture();
        if (keep == 0) is_usb_streaming = true;
        if (keep == 1) phone_opened = true;
        if (keep == 2) mic_requested = true;
        if (keep == 3) mic_active = true;
        if (keep == 4) playback_pcm.primed_on_wire = false;
        playback_idle_poll(0); playback_idle_poll(10000); assert(!suspends);
    }
    fixture(); playback_idle_poll(0); playback_idle_poll(1000);
    phone_opened = true;
    assert(!bridge_phone_ready()); // A pending Suspend cannot authorize phone Start.
    accept(AVDTP_SI_SUSPEND, 5);
    playback_idle_poll(1001); assert(starts == 1 && !bridge_phone_ready());
    accept(AVDTP_SI_START, 5); assert(!bridge_phone_ready());
    playback_pcm.priming = 0; playback_pcm_sent(&playback_pcm); assert(bridge_phone_ready());
    bridge_phone_stop(); playback_idle_poll(1002); playback_idle_poll(2002); assert(suspends == 2);
    fixture(); suspend_idle();
    now += 60000; playback_idle_poll(now); assert(!starts && !closes);
    for (unsigned start = 0; start < 2; ++start) {
        fixture(); playback_idle_poll(0); playback_idle_poll(1000);
        if (start) { accept(AVDTP_SI_SUSPEND, 5); is_usb_streaming = true; playback_idle_poll(1001); }
        playback_idle_poll(start ? 4001 : 4000);
        assert(closes == 1 && !finish_scan_avdtp_codec);
        accept(AVDTP_SI_START, 5); assert(!media_tracker.streaming);
    }
    fixture(); suspend_error = 1; playback_idle_poll(0); playback_idle_poll(1000); assert(closes == 1);
    fixture(); suspend_idle(); start_error = 1; is_usb_streaming = true; playback_idle_poll(++now); assert(closes == 1);
    fixture(); suspend_idle(); encoder_ready = false; is_usb_streaming = true; playback_idle_poll(++now); assert(closes == 1 && !starts);
    fixture(); accept(AVDTP_SI_SUSPEND, 5); is_usb_streaming = true;
    playback_idle_poll(10000); assert(!starts); // An unsolicited peer stop cannot be reclaimed.
    fixture(); suspend_idle(); is_usb_streaming = profile_pending = true;
    playback_idle_poll(++now); assert(!starts);
    fixture(); now = UINT32_MAX - 500; suspend_idle(); assert(!closes);
    puts("idle playback: confirmed suspend/start, onset priming, preparation/mic fences, timeouts and stale accepts passed");
}
