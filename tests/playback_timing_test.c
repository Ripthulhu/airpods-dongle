// SPDX-License-Identifier: GPL-3.0-only
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../src/btstack/playback_eld.h"
#include "../src/btstack/playback_continuity.h"
#include "../src/btstack/playback_adjust.h"
#include "../src/btstack/playback_options.h"
#include "playback_test_hooks.h"
#include "../src/runtime_watch.h"
#include "../src/btstack/playback_inspect.h"
#include "../src/btstack/playback_pcm.h"
static playback_inspect_t playback_inspect;
static bool inspect_enabled;
static playback_adjust_t playback_adjust;
static playback_game_t playback_game;
static playback_continuity_t phone_continuity;

#define HAVE_AAC_FDK
#define AVDTP_CODEC_SBC 1
#define AVDTP_CODEC_MPEG_1_2_AUDIO 2
#define AVDTP_CODEC_ATRAC_FAMILY 3
#define AVDTP_CODEC_NON_A2DP 4
#define AVDTP_CODEC_MPEG_2_4_AAC 5
#define A2DP_CODEC_VENDOR_ID_APPLE 76
#define A2DP_APPLE_CODEC_AAC_ELD 32769
#define ERROR_CODE_SUCCESS 0
#define SUSPEND_RECOVERY_TIMEOUT_MS 3000
#define AUDIO_SLOT_MAX_SAMPLES 1024
#define btstack_assert assert
#define IN_AUDIO_DATA 0
#define OUT_BITSTREAM_DATA 1
#define AACENC_OK 0
typedef int AACENC_ERROR;
typedef void *HANDLE_AACENCODER;
typedef struct { int numBufs; void **bufs; int *bufferIdentifiers, *bufSizes, *bufElSizes; } AACENC_BufDesc;
typedef struct { unsigned numInSamples; } AACENC_InArgs;
typedef struct { int numOutBytes, numInSamples; } AACENC_OutArgs;
static void *handleAAC = (void *)1;
static struct { unsigned inputChannels; } aacinf = {2};
static struct { bool rate_mode; } playback_options;
static bool bt_locked = true, encode_in_progress;
static uint32_t control_session = 1, usb_playback_revision = 1;
static uint32_t aacp_control_session(void) {return control_session;}
static void (*encode_hook)(void);
static void (*pcm_read_hook)(void), (*pcm_lock_hook)(void);
static unsigned encoder_closes, encoder_resets, configuration_callbacks;
static unsigned encoder_errors;
static bool allow_encoder_failure, encode_zero;
static int encode_error, encode_consumed_delta;
static uint8_t last_configuration;
#include "playback_encoder_state.inc"
static uint16_t aaceld_frame_sequence;
static uint32_t now, consumed, emitted, frame_size, sample_frames, send_failures;
static unsigned encode_calls, eld_packets, eld_units;
static uint16_t eld_sequence;
static bool send_requested, phone_opened, recovery_reset_encoder, suspend_recovery_pending;
static bool phone_active, phone_output_recovery, phone_buffer_started;
static bool audio_owned = true;
static bool aacp_audio_owned(void) {return audio_owned;}
static bool call_active, call_opened;
static uint16_t phone_output_recovery_cid;
static uint32_t phone_output_recovery_since;
static uint8_t phone_output_recovery_peer[6], cur_active_device[6];
static bool bridge_phone_ready(void) {return phone_active;}
static bool profile_pending, a2dp_connect_pending;
static bool aacp_mic_active(void) {return false;}
static bool allow_recovery, synchronous_request;
static unsigned request_depth;
static unsigned scheduled_interval, source_consumed;
static unsigned audio_timer_interval, acc_num_simples, cur_codec, aaceld_bitrate = 256000;
enum { PLAYBACK_RUNNING, PLAYBACK_SUSPENDING, PLAYBACK_SUSPENDED, PLAYBACK_STARTING };
static unsigned playback_idle_phase;
typedef struct { void *context; uint32_t timeout; } btstack_timer_source_t;
typedef struct { uint8_t *media_codec_information; } adtvp_media_codec_capabilities_t;
typedef unsigned avdtp_media_codec_type_t;
typedef struct {
    unsigned avdtp_cid, local_seid;
    uint32_t time_audio_data_sent, acc_num_missed_samples, samples_ready;
    btstack_timer_source_t audio_timer;
    bool streaming;
    int max_media_payload_size;
    uint32_t send_wait_start, rtp_timestamp;
    uint8_t codec_storage[986];
    uint16_t codec_storage_count, codec_num_frames;
    uint8_t codec_ready_to_send;
} a2dp_media_sending_context_t;
static a2dp_media_sending_context_t media_tracker;
static void playback_adjust_cancel_stream(a2dp_media_sending_context_t *,uint32_t,uint8_t);
static bool a2dp_is_connected_flag = true;
#ifndef PLAYBACK_PIPELINE
static bool is_usb_streaming;
static unsigned playback_pcm_lock;
static void critical_section_enter_blocking(void *p) {
    (void)p;assert(bt_locked);if(pcm_lock_hook)pcm_lock_hook();
}
static void critical_section_exit(void *p) {(void)p;assert(bt_locked);}
#endif
static struct { unsigned encoded_units, recoveries, send_failures;
    uint32_t pcm_peak,pcm_drops,pcm_underflows; } playback_metrics;
static playback_encoder_pcm_t encoder_pcm;
static struct endpoint {
    struct { struct { unsigned media_codec_type; } media_codec; } remote_configuration;
    struct { struct { adtvp_media_codec_capabilities_t media_codec; } capabilities; } sep;
} endpoint;
static struct { struct endpoint *local_stream_endpoint; } sc = {&endpoint};
static btstack_timer_source_t suspend_recovery_timer;
static void btstack_run_loop_set_timer(void *a, unsigned b) {
    ((btstack_timer_source_t *)a)->timeout=now+b+1;
    scheduled_interval=b+1;
}
static void btstack_run_loop_add_timer(void *a) { (void)a; }
static void btstack_run_loop_remove_timer(void *a) { (void)a; }
static void *btstack_run_loop_get_timer_context(btstack_timer_source_t *t) { return t->context; }
static uint32_t btstack_run_loop_get_time_ms(void) { return now; }
static unsigned a2dp_sample_rate(void) { return 48000; }
static void a2dp_demo_timer_stop(void *a) {
    playback_encoder_invalidate();
    playback_continuity_reset(&phone_continuity);
    (void)a; assert(allow_recovery); media_tracker.streaming=false;
    media_tracker.samples_ready=media_tracker.acc_num_missed_samples=0;
    media_tracker.codec_ready_to_send=media_tracker.codec_storage_count=media_tracker.codec_num_frames=0;
}
static unsigned avdtp_source_suspend(unsigned a, unsigned b) { (void)a; (void)b; assert(allow_recovery); return 0; }
static void aacp_disconnect(void) { assert(false); }
static void a2dp_source_disconnect(unsigned a) { (void)a; assert(false); }
static void suspend_recovery_handler(btstack_timer_source_t *a) { (void)a; }
static void btstack_run_loop_set_timer_handler(void *a, void (*b)(btstack_timer_source_t *)) { (void)a; (void)b; }
static unsigned btstack_sbc_encoder_num_audio_frames(void) { return sample_frames; }
static int btstack_sbc_encoder_sbc_buffer_length(void) { return (int)frame_size; }
static uint8_t *btstack_sbc_encoder_sbc_buffer(void) { static uint8_t bytes[128]; return bytes; }
static void btstack_sbc_encoder_process_data(void *a) { (void)a; }
static unsigned time_us_32(void) { return now*1000u; }
static void playback_encode_measured(unsigned a) { (void)a; }
#ifndef PLAYBACK_PIPELINE
static void audio_pcm_read(unsigned frames) {
    consumed += frames;
    source_consumed += frames * playback_adjust_interval(&playback_adjust) / 10;
    if(pcm_read_hook)pcm_read_hook();
}
#else
#include "../src/btstack/playback_pcm.h"
static playback_pcm_t playback_pcm;
static playback_resample_t encoder_resample;
static playback_usb_clock_t usb_clock;
static playback_trace_t playback_trace;
#undef PB_ADD
#undef PB_MAX
#undef PB_EVENT
#define PB_ADD(stat,value) (playback_trace.stats[stat]+=(value))
#define PB_MAX(stat,value) do {uint32_t v=(value);if(v>playback_trace.stats[stat])playback_trace.stats[stat]=v;}while(0)
#define PB_EVENT(kind,a,b) playback_trace_record(&playback_trace,time_us_32(),kind,a,b)
static unsigned playback_pcm_lock,usb_recovery_drops,pcm_frame_samples=480;
static bool is_usb_streaming=true;
#ifdef PLAYBACK_REAL_PHONE
#include "btstack_resample.h"
#include "../src/btstack/phone_clock.h"
static phone_clock_t phone_clock;
static btstack_resample_t phone_resampler;
#else
static struct { unsigned unused; } phone_clock;
static unsigned phone_resampler;
static unsigned phone_clock_prefill(unsigned frames) {(void)frames;return 1440;}
static unsigned phone_clock_update(void *clock,unsigned remaining,unsigned frames) {
    (void)clock;(void)remaining;(void)frames;return 65536;
}
static void btstack_resample_set_factor(void *resampler,unsigned rate) {(void)resampler;(void)rate;}
#endif
static bool pcm_locked;
static void phone_sink_record_loss(bool overflow,uint32_t frames,uint32_t fill,uint32_t debt) {
    (void)overflow;(void)frames;(void)fill;(void)debt;
    assert(phone_active && !call_active && !is_usb_streaming);
}
static void pipeline_filter_cost(void);
static void critical_section_enter_blocking(void *p) {(void)p;assert(!pcm_locked);pcm_locked=true;}
static void critical_section_exit(void *p) {(void)p;assert(pcm_locked);pcm_locked=false;}
static void pipeline_render(const playback_resample_t *work,int16_t *dst) {
    assert(!pcm_locked);playback_pcm_render(work,dst);
    pipeline_filter_cost();
}
static void pipeline_render_q31(const playback_resample_t *work,int32_t *dst) {
    assert(!pcm_locked);playback_pcm_render_q31(work,dst);
    pipeline_filter_cost();
}
#define playback_pcm_render pipeline_render
#define playback_pcm_render_q31 pipeline_render_q31
static void playback_prepare_packet(a2dp_media_sending_context_t *,uint32_t);
static void playback_transport_recover(a2dp_media_sending_context_t *);
#include "playback_pipeline.inc"
static void pipeline_encode(AACENC_BufDesc *,AACENC_InArgs *);
static void pipeline_received(unsigned frames);
static void pipeline_request(void);
#endif
static void a2dp_demo_send_media_packet(void);
static void a2dp_source_stream_endpoint_request_can_send_now(unsigned a, unsigned b) {
    (void)a; (void)b;
    if (!synchronous_request) {
        send_requested=true;
#ifdef PLAYBACK_PIPELINE
        pipeline_request();
#endif
        return;
    }
    assert(++request_depth == 1); // A failed send must not recursively request another callback.
    a2dp_demo_send_media_packet();
    --request_depth;
}
static unsigned get_vendor_id(void *a) { (void)a; return A2DP_CODEC_VENDOR_ID_APPLE; }
static unsigned get_codec_id(void *a) { (void)a; return A2DP_APPLE_CODEC_AAC_ELD; }
static void playback_media_sent(void) {
#ifdef PLAYBACK_PIPELINE
    playback_pcm_sent(&playback_pcm);
#endif
}
static unsigned a2dp_source_stream_send_media_payload_rtp(unsigned a, unsigned b,
    unsigned c, unsigned timestamp, uint8_t *packet, unsigned length) {
    (void)a; (void)b; (void)c;
    assert(timestamp == emitted);
    assert(length <= (unsigned)media_tracker.max_media_payload_size);
    if (send_failures) { --send_failures; return 1; }
    if (cur_codec == 1) {
        assert(packet[0] >= 1 && packet[0] <= 15);
        assert(length == 1 + packet[0] * frame_size);
        emitted += packet[0] * sample_frames;
    } else {
        if (cur_codec == 4) {
            unsigned offset=0,units=0;
            while(offset<length) {
                assert(length-offset>=4);
                assert(packet[offset]==(uint8_t)(0xb0|(eld_sequence>>8)));
                assert(packet[offset+1]==(uint8_t)eld_sequence);
                assert((packet[offset+2]&0xf8)==0x10);
                unsigned bytes=((packet[offset+2]&7u)<<8)|packet[offset+3];
                assert(bytes && bytes<=length-offset-4);
                for(unsigned i=0;i<bytes;++i)assert(packet[offset+4+i]==(uint8_t)eld_units);
                offset+=4+bytes;++units;++eld_units;eld_sequence=(eld_sequence+1)&0xfff;
            }
            assert(units==media_tracker.codec_num_frames);++eld_packets;
        }
        emitted += media_tracker.codec_num_frames * sample_frames;
    }
#ifdef PLAYBACK_PIPELINE
    pipeline_received(media_tracker.codec_num_frames*sample_frames);
#endif
    return 0;
}
static int playback_encoder_failed(a2dp_media_sending_context_t *context) {
    assert(allow_encoder_failure);++encoder_errors;a2dp_demo_timer_stop(context);return -1;
}
static void aacEncClose(void **handle) {
    assert(bt_locked && !encode_in_progress);
    ++encoder_closes; *handle = NULL;
}
#include "playback_encoder_reset_guard.inc"
static bool aaceld_encoder_init(void) {
    if(production_encoder_reset_guard())return true;
    assert(bt_locked && !encode_in_progress);
    ++encoder_resets; handleAAC = (void *)1; return true;
}
#define HCI_EVENT_PACKET 4
static uint16_t little_endian_read_16(const uint8_t *data, unsigned offset) {
    return data[offset] | (uint16_t)data[offset + 1] << 8;
}
static void packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)type; (void)channel;
    assert(bt_locked && !encode_in_progress); ++configuration_callbacks;
    last_configuration=size>5?packet[5]:0;
}
static AACENC_ERROR aacEncEncode(void *handle, AACENC_BufDesc *input, AACENC_BufDesc *output,
    AACENC_InArgs *args, AACENC_OutArgs *result) {
    (void)handle; (void)input;
    assert(!bt_locked && !encode_in_progress);
    encode_in_progress = true;
    if (encode_hook) encode_hook();
#ifdef PLAYBACK_PIPELINE
    pipeline_encode(input,args);
#endif
    assert(args->numInSamples == sample_frames * 2);
    unsigned bytes = frame_size - (cur_codec == 4 ? 4 : 0);
    assert(output->bufSizes[0] >= (int)bytes);
    // The byte tag checks committed packet order; canceled jobs never go on air.
    uint8_t tag=cur_codec==4?(uint8_t)(emitted/sample_frames+media_tracker.codec_num_frames):(uint8_t)encode_calls;
    ++encode_calls;memset(output->bufs[0],tag,bytes);
    result->numOutBytes = encode_zero?0:(int)bytes;
    result->numInSamples = (int)args->numInSamples+encode_consumed_delta;
    encode_in_progress = false;
    return encode_error;
}
#include "playback_timing.inc"
#include "playback_encoder_job.inc"

static void complete_test_encode(void) {
    assert(bt_locked);
    bt_locked = false; dongle_playback_encode_run(); bt_locked = true;
    dongle_playback_encode_complete();
}
static void prepare_test_packet(a2dp_media_sending_context_t *context, uint32_t time) {
    playback_prepare_packet(context, time);
    complete_test_encode();
    playback_prepare_packet(context, time);
}
#define playback_prepare_packet prepare_test_packet

static void drain(void) {
    unsigned limit=sample_frames*4;
    if(limit<1920)limit=1920;
    if(media_tracker.samples_ready>limit)media_tracker.samples_ready=limit;
    unsigned budget = 100;
    while (send_requested || playback_work_pending || playback_encoder_busy() ||
           (!media_tracker.codec_ready_to_send && media_tracker.samples_ready>=sample_frames)) {
        assert(budget--);
        if(playback_work_pending || playback_encoder_busy() ||
           (!media_tracker.codec_ready_to_send && media_tracker.samples_ready>=sample_frames)) {
            playback_work_pending=false;
            playback_prepare_packet(&media_tracker,now);
        }
        if(!send_requested)continue;
        send_requested = false;
        a2dp_demo_send_media_packet();
        if (media_tracker.codec_ready_to_send == 2) {
            assert(!send_requested); // Failed sends must return before the main loop retries.
            playback_prepare_packet(&media_tracker, now);
        }
    }
}
static void initialize(unsigned codec, unsigned samples, unsigned bytes, unsigned interval, unsigned mtu) {
    memset(&encoder_job, 0, sizeof(encoder_job));
    atomic_init(&encoder_job.epoch, 1);
    atomic_init(&encoder_job.state, ENCODE_IDLE);
    handleAAC = (void *)1; bt_locked = true; encode_in_progress = false; encode_hook = NULL;
    encoder_closes = encoder_resets = configuration_callbacks = 0;
    encoder_errors = 0;allow_encoder_failure = encode_zero = false;
    encode_error = encode_consumed_delta = 0;last_configuration = 0;
    pcm_read_hook = pcm_lock_hook = NULL;
    control_session = usb_playback_revision = 1;
    playback_continuity_reset(&phone_continuity);
    phone_buffer_started=false;audio_owned=true;
    playback_adjust_cancel(&playback_adjust);
    playback_work_pending=playback_transport_stale=false;packet_oldest_us=media_last_sent_us=0;
    memset(&media_tracker, 0, sizeof(media_tracker));
    memset(&playback_metrics, 0, sizeof(playback_metrics));
    now = consumed = emitted = send_failures = 0;
    encode_calls=eld_packets=eld_units=0;aaceld_frame_sequence=eld_sequence=4095;
    send_requested = false; phone_opened = phone_active = phone_output_recovery = call_active = call_opened = false;
    allow_recovery = synchronous_request = false;
    suspend_recovery_pending=recovery_reset_encoder=false;
    request_depth = 0;
    cur_codec = codec; sample_frames = acc_num_simples = samples;
    scheduled_interval=interval; source_consumed=0;
    frame_size = bytes; audio_timer_interval = interval;
    endpoint.remote_configuration.media_codec.media_codec_type =
        codec == 1 ? AVDTP_CODEC_SBC : codec == 2 ? AVDTP_CODEC_MPEG_2_4_AAC : AVDTP_CODEC_NON_A2DP;
    media_tracker.streaming = true;
    media_tracker.max_media_payload_size = (int)mtu;
}
static void timing(unsigned codec, unsigned samples, unsigned bytes, unsigned interval, unsigned mtu, bool phone) {
    initialize(codec, samples, bytes, interval, mtu);
    phone_opened = phone;
    btstack_timer_source_t timer = {.context=&media_tracker};
    for (unsigned i = 0; i < 1000; ++i) {
        now += interval;
        avdtp_audio_timeout_handler(&timer);
        drain();
    }
    unsigned expected = now * 48;
    assert(consumed <= expected && expected - consumed < samples);
    assert(emitted + media_tracker.codec_num_frames * samples == consumed);
    // A delayed callback bounds stale work; subsequent packets remain valid.
    unsigned before = consumed;
    now += 1000;
    avdtp_audio_timeout_handler(&timer);
    drain();
    assert(consumed - before <= (samples * 4 > 1920 ? samples * 4 : 1920));
    assert(playback_metrics.recoveries == 0);
}
static void phone_packet_groups(void) {
    // A nominal 320-byte CBR AU can need one extra byte from FDK's reservoir alignment.
    initialize(4,480,324,10,324);media_tracker.samples_ready=480;
    assert(fill_aac_audio_buffer(&media_tracker)==0 && !encode_calls && !consumed);
    const unsigned mtus[]={325,648,649,986};
    for(unsigned target=60;target<=150;target+=90)for(unsigned m=0;m<4;++m) {
        initialize(4,480,324,10,mtus[m]);phone_opened=true;
        playback_game.changed=true;playback_game.target=(uint16_t)target;
        btstack_timer_source_t timer={.context=&media_tracker};
        unsigned group=mtus[m]<649?1:2;
        for(unsigned unit=0;unit<600;++unit) {
            if(unit==301)playback_game.target=(uint16_t)(210-target); // Change modes with one AU queued.
            now+=10;avdtp_audio_timeout_handler(&timer);
            unsigned before=encode_calls;playback_prepare_packet(&media_tracker,now);
            assert(encode_calls==before+1); // Each main-loop pass yields after one AU.
            if((unit+1)%group)assert(!send_requested && media_tracker.codec_num_frames==1);
            else {
                assert(send_requested && media_tracker.codec_num_frames==group);
                send_requested=false;a2dp_demo_send_media_packet();
            }
        }
        assert(eld_packets==600/group && eld_units==600 && consumed==288000);
        assert(emitted==consumed && media_tracker.rtp_timestamp==consumed);
    }
    initialize(4,480,324,10,986);phone_opened=true;
    btstack_timer_source_t timer={.context=&media_tracker};
    now=20;avdtp_audio_timeout_handler(&timer);
    playback_prepare_packet(&media_tracker,now);
    assert(encode_calls==1 && !send_requested && media_tracker.codec_num_frames==1);
    playback_prepare_packet(&media_tracker,now);
    assert(send_requested && media_tracker.codec_num_frames==2);
    uint8_t saved[648];memcpy(saved,media_tracker.codec_storage,sizeof(saved));
    send_requested=false;send_failures=1;a2dp_demo_send_media_packet();
    assert(!emitted && media_tracker.codec_ready_to_send==2 && !media_tracker.rtp_timestamp);
    assert(!memcmp(saved,media_tracker.codec_storage,sizeof(saved)));
    playback_prepare_packet(&media_tracker,now);send_requested=false;a2dp_demo_send_media_packet();
    assert(emitted==960 && eld_packets==1 && eld_units==2 && encode_calls==2);
    initialize(4,480,324,10,986);phone_opened=synchronous_request=true;
    now=100;avdtp_audio_timeout_handler(&timer);
    playback_prepare_packet(&media_tracker,now);assert(encode_calls==1 && !emitted);
    playback_prepare_packet(&media_tracker,now);
    assert(encode_calls==2 && emitted==960 && request_depth==0); // Inline send still yields to main.
}
static void live_clock(uint16_t delay, uint16_t target) {
    initialize(4,480,324,10,986);
    playback_adjust_request(&playback_adjust,delay,target);
    btstack_timer_source_t timer={.context=&media_tracker};
    now=10; avdtp_audio_timeout_handler(&timer); drain();
    assert(playback_adjust.running && !playback_adjust.pending);
    for(unsigned i=0;playback_adjust_busy(&playback_adjust) && i<1000;++i) {
        now+=scheduled_interval;
        avdtp_audio_timeout_handler(&timer); drain();
    }
    assert(!playback_adjust_busy(&playback_adjust) && !playback_adjust.failed);
    assert(media_tracker.streaming && !playback_metrics.recoveries);
    assert(source_consumed<=now*48 && now*48-source_consumed<480);
    assert(emitted+media_tracker.codec_num_frames*sample_frames==consumed && scheduled_interval==10);
}
static void fixed_deadlines(void) {
    initialize(4,480,324,10,986);
    btstack_timer_source_t timer={.context=&media_tracker};
    media_tracker.audio_timer.timeout=10;
    now=10;avdtp_audio_timeout_handler(&timer);drain();
    assert(media_tracker.audio_timer.timeout==20);
    now=21;avdtp_audio_timeout_handler(&timer);drain();
    assert(media_tracker.audio_timer.timeout==30); // A late callback does not shift the next tick.
    now=1000;avdtp_audio_timeout_handler(&timer);drain();
    assert(media_tracker.audio_timer.timeout==1010); // No timer spin after a long stall.
}
static void rate_boundaries_preserve_source_time(void) {
    for(unsigned old=9;old<=11;++old)for(unsigned next=9;next<=11;++next) {
        initialize(4,480,324,10,986);
        now=UINT32_MAX-3u;
        media_tracker.time_audio_data_sent=now-7u;
        media_tracker.samples_ready=137;media_tracker.acc_num_missed_samples=14300;
        uint64_t before=((uint64_t)137*99000u+14300u)*old+7u*48u*990000u;
        playback_clock_switch(&media_tracker,now,(uint8_t)old,(uint8_t)next);
        uint64_t after=((uint64_t)media_tracker.samples_ready*99000u+
                        media_tracker.acc_num_missed_samples)*next;
        assert(after<=before && before-after<next);
        assert(media_tracker.time_audio_data_sent==now);
    }
}
#if !defined(PLAYBACK_PIPELINE) && !defined(PLAYBACK_ENCODER_JOB)
int main(void) {
    phone_packet_groups();
    rate_boundaries_preserve_source_time();
    fixed_deadlines();
    live_clock(1590,60); live_clock(900,150);
    timing(1, 128, 118, 10, 656, false); // Ordinary stereo SBC, bitpool 53.
    timing(1, 128, 38, 10, 656, false);  // Valid bitpool 13 must respect the 15-frame header.
    timing(1, 16, 9, 10, 656, false);    // Small blocks need multiple sends per timer tick.
    timing(1, 128, 118, 10, 120, false); // A valid small MTU cannot reduce the playback rate.
    timing(2, 1024, 587, 12, 656, false);
    timing(4, 480, 324, 10, 986, false);
    timing(4, 480, 324, 10, 986, true);
    initialize(1, 128, 118, 10, 656);
    btstack_timer_source_t timer = {.context=&media_tracker};
    now = 10;
    avdtp_audio_timeout_handler(&timer);
    now = 20;
    avdtp_audio_timeout_handler(&timer);
    send_failures = 2;
    drain();
    assert(emitted && playback_metrics.send_failures == 2);
    initialize(1, 128, 118, 10, 656);
    now = 10; avdtp_audio_timeout_handler(&timer);playback_prepare_packet(&media_tracker,now);
    now = 20; avdtp_audio_timeout_handler(&timer);playback_prepare_packet(&media_tracker,now);
    send_requested = false; send_failures = 1000;
    a2dp_demo_send_media_packet();
    assert(!send_requested && media_tracker.codec_ready_to_send == 2);
    // A repeated failure returns to the poller without resetting its original deadline.
    unsigned started = media_tracker.send_wait_start;
    synchronous_request = true;
    playback_prepare_packet(&media_tracker, now);
    assert(request_depth == 0);
    assert(!send_requested && media_tracker.codec_ready_to_send == 2);
    assert(media_tracker.send_wait_start == started);
    now = started + 200; allow_recovery = true;
    avdtp_audio_timeout_handler(&timer);
    assert(playback_metrics.recoveries == 1 && suspend_recovery_pending && !media_tracker.streaming);
    puts("playback timing: SBC/AAC/ELD pacing, bounded debt, 15-frame headers, MTU and retries passed");
    return 0;
}
#endif
