// SPDX-License-Identifier: GPL-3.0-only
// Production worker/bridge/resampler/read/packet path, with synthetic stereo and
// measured CPU costs. Captured relative arrival times contain no audio.
#define PLAYBACK_REAL_PHONE
#define main usb_pipeline_main
#include "playback_pipeline_test.c"
#undef main

static unsigned worker_clock, decoder_cost;
#define PHONE_WORKER_TIME_US() worker_clock
#include "../src/btstack/phone_worker.c"
#include "phone_jitter_arrivals.h"

void phone_worker_wake(void) {}
bool phone_codec_configuration_valid(phone_codec_t codec,const phone_sbc_config_t *sbc) {
    (void)sbc;return codec==PHONE_CODEC_LHDCV5;
}
bool phone_codec_configure(phone_codec_t codec,const phone_sbc_config_t *sbc) {
    return phone_codec_configuration_valid(codec,sbc);
}
bool phone_codec_prepare(void) {return true;}
void phone_codec_close(void) {}
void phone_codec_reset(void) {}
bool phone_codec_decode(const uint8_t *payload,size_t length,uint8_t count,
                        phone_codec_pcm_callback callback,void *context) {
    assert(payload && length && count==3);worker_clock+=decoder_cost;
    int16_t pcm[720*2];
    for(unsigned i=0;i<720;++i) {pcm[2*i]=12345;pcm[2*i+1]=-23456;}
    callback(pcm,720,context);return true;
}
static bool bridge_phone_recovering(void) {return false;}
static void bridge_phone_drop_pcm(void) {assert(false);}
static void bridge_call_stop(void) {assert(false);}
#include "playback_phone_push.inc"

static void receive_pcm(const int16_t *pcm,uint16_t frames,void *context) {
    (void)context;bridge_phone_pcm_push(pcm,frames);
}
static void phone_pipeline_encoded(const int16_t *pcm,unsigned frames) {
    for(unsigned i=0;i<frames;++i)
        assert((pcm[2*i]==12345 && pcm[2*i+1]==-23456) || (!pcm[2*i]&&!pcm[2*i+1]));
}

#define STEADY_PACKETS 8000u // Settle the existing clock feedback for 120 seconds.
#define TRACE_PACKETS (sizeof(phone_jitter_arrivals_us)/sizeof(*phone_jitter_arrivals_us))
static uint64_t packet_arrival(unsigned packet) {
    // The measured -242 ppm source clock is within the existing correction.
    uint64_t steady=(uint64_t)(packet<STEADY_PACKETS?packet:STEADY_PACKETS)*15000u*1000000u/999758u;
    return packet<STEADY_PACKETS?steady:steady+phone_jitter_arrivals_us[packet-STEADY_PACKETS];
}
static void captured_phone_timing(unsigned phase) {
    fixture(0);is_usb_streaming=false;phone_active=phone_opened=true;phone_buffer_started=false;
    playback_trace.stats[TRACE_PCM_MIN]=UINT32_MAX;
    assert(playback_pcm_reset(&playback_pcm,PHONE_PCM_CAPACITY,false));playback_pcm.primed_on_wire=true;
    btstack_resample_init(&phone_resampler,2);phone_clock_init(&phone_clock,480);
    assert(phone_worker_configure(PHONE_CODEC_LHDCV5,NULL));
    assert(phone_worker_prepare()==PHONE_WORKER_READY);
    playback_game.target=150; // Isolate input jitter from receiver recovery.
    media_tracker.audio_timer.timeout=10+phase/1000u;
    unsigned packet=0,main_due=phase%1000u,worker_due=0,send_due=0,model_us=0;
    unsigned reads=0,decode_number=phase/1000u,min_pcm=PHONE_PCM_CAPACITY;
    bool decoding=false;worker_clock=0;
    uint64_t stop=packet_arrival(STEADY_PACKETS+TRACE_PACKETS-1u);
    for(;;) {
        now=model_us/1000u;
        if(decoding && model_us>=worker_due) {assert(phone_worker_step());decoding=false;}
        if(model_us>=main_due) {
            // The SDK drains pending HCI input before the main-loop PCM poll.
            while(packet<STEADY_PACKETS+TRACE_PACKETS && packet_arrival(packet)<=model_us) {
                const uint8_t payload[4]={1};
                assert(phone_worker_submit(payload,sizeof(payload),3));++packet;
            }
            assert(phone_worker_poll(receive_pcm,NULL));
            if((int32_t)(now-media_tracker.audio_timer.timeout)>=0)
                avdtp_audio_timeout_handler(&media_tracker.audio_timer);
            if(send_requested && model_us>=send_due) {send_requested=false;a2dp_demo_send_media_packet();}
            unsigned before=encode_calls;playback_audio_work(now);
            unsigned encodes=encode_calls-before;assert(encodes<=1);reads+=encodes;
            if(send_requested && send_due<model_us)send_due=model_us+8000u;
            unsigned cost=encodes?(reads%30u?1700u:6400u):0;
            main_due=model_us+(cost>1000u?cost:1000u);
            if(phone_buffer_started && playback_pcm.used<min_pcm)min_pcm=playback_pcm.used;
        }
        phone_worker_queue_snapshot_t queue=phone_worker_queue_snapshot();
        if(!decoding && queue.queued_packets && !queue.output_ready) {
            decoder_cost=decode_number++%10u?10215u:13800u;
            worker_due=model_us+decoder_cost;decoding=true;
        }
        if(model_us>=stop)break;
        unsigned next=main_due;if(decoding && worker_due<next)next=worker_due;
        assert(next>model_us);model_us=next;
    }
    printf("phone captured timing phase=%u: zeros=%u drops=%u PCM=%u..%u factor=%u\n",
           phase,playback_metrics.pcm_underflows,playback_metrics.pcm_drops,min_pcm,
           playback_trace.stats[TRACE_PCM_MAX],playback_trace.stats[TRACE_CLOCK_Q16]);
    assert(phone_buffer_started && !playback_metrics.pcm_underflows && !playback_metrics.pcm_drops);
    assert(playback_trace.stats[TRACE_PCM_MIN]==min_pcm);
    assert(playback_trace.stats[TRACE_PCM_MAX]>=phone_clock_prefill(480) &&
           playback_trace.stats[TRACE_PCM_MAX]<=PHONE_PCM_CAPACITY);
    assert(playback_trace.stats[TRACE_CLOCK_Q16]>=65536u-32u &&
           playback_trace.stats[TRACE_CLOCK_Q16]<=65536u+32u);
    assert(!playback_metrics.recoveries && media_tracker.streaming);
    assert(!phone_worker_close());assert(phone_worker_step());assert(phone_worker_close());
}
int main(void) {
    setbuf(stdout,NULL);
    captured_phone_timing(0);
    captured_phone_timing(3000); // Worst zero-fill phase with the former 80 ms reserve.
    captured_phone_timing(4000); // Longest decoded-delivery gap in the same replay.
    puts("phone worker, real resampler and production PCM captured-timing regression: passed");
    return 0;
}
