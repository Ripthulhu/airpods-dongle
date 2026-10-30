// Production timer/PCM/encoding/send and recovery poll, with bounded synthetic CPU/radio costs.
#define main baseline_pipeline_main
#include "playback_pipeline_test.c"
#undef main
static const uint8_t *get_device_addr(void) {return cur_active_device;}
static bool aacp_mic_requested(void) {return false;}
#define PLAYBACK_TRANSITION_MS 3000u
#include "playback_phone_recovery.inc"

static void relay_fixture(unsigned wait) {
    fixture(0);playback_game=(playback_game_t){0};
    is_usb_streaming=false;phone_active=phone_opened=phone_buffer_started=true;
    assert(playback_pcm_reset(&playback_pcm,8192,false));playback_pcm.primed_on_wire=true;
    generated=encoded=0;phone_arrivals=phone_queued=phone_decode_due=phone_decoded=0;
    phone_decoding=phone_output_ready=false;phone_ingress(4800);
    encode_cost=6;loop_cost=1;request_wait=wait;next_wait=500;
}
static void relay_step(void) {
    step();
    assert(!playback_phone_recovery_poll());
}
static void recoverable_radio_waits_keep_source_and_receiver_time(void) {
    const unsigned waits[]={65,80};
    for(unsigned i=0;i<sizeof(waits)/sizeof(*waits);++i) {
        relay_fixture(waits[i]);allow_recovery=true;
        while(now<15000 && media_tracker.streaming)relay_step();
        assert(media_tracker.streaming && !suspend_recovery_pending && !playback_metrics.recoveries);
        assert(!playback_metrics.pcm_drops && !playback_metrics.pcm_underflows && !playback_transport_stale);
        assert(generated==encoded+playback_pcm.used && playback_pcm.capacity==8192);
        assert(phone_continuity.deficit_us<150000u);
        request_wait=0;blocked_until=0;
        unsigned stop=now+500;
        while(now<stop)relay_step();
        assert(!phone_continuity.deficit_us && playback_pcm.used<5760 && media_tracker.samples_ready<480);
    }
}
static void actual_overflow_and_missing_data_still_recover(void) {
    relay_fixture(100);allow_recovery=true;
    while(now<15000 && media_tracker.streaming)relay_step();
    assert(playback_metrics.pcm_drops && !media_tracker.streaming && suspend_recovery_pending);
    assert(playback_metrics.recoveries==1 && phone_output_recovery);

    relay_fixture(0);allow_recovery=true;
    playback_pcm_clear(&playback_pcm,8192);
    media_tracker.codec_ready_to_send=1;media_tracker.send_wait_start=0;
    now=200;avdtp_audio_timeout_handler(&media_tracker.audio_timer);
    assert(!media_tracker.streaming && suspend_recovery_pending && playback_metrics.recoveries==1);
    assert(!encode_calls); // No decoded data can justify unbounded catch-up work.
}
static void wrapped_backlog_and_other_input_limits(void) {
    relay_fixture(0);
    media_tracker.codec_ready_to_send=1;
    media_tracker.time_audio_data_sent=UINT32_MAX-59u;
    now=20;playback_clock_accrue(&media_tracker,now);
    assert(media_tracker.samples_ready==3840);
    playback_audio_work(now);
    assert(media_tracker.samples_ready==3840 && playback_pcm.used==4800 && !playback_metrics.pcm_drops);

    relay_fixture(0);phone_active=phone_opened=false;call_active=call_opened=true;
    media_tracker.codec_ready_to_send=1;media_tracker.samples_ready=3200;
    playback_audio_work(now);
    assert(media_tracker.samples_ready==2880 && playback_metrics.pcm_drops==320);

    relay_fixture(0);phone_active=phone_opened=false;is_usb_streaming=true;allow_recovery=true;
    media_tracker.codec_ready_to_send=1;media_tracker.samples_ready=3200;
    playback_audio_work(now);
    assert(!media_tracker.streaming && suspend_recovery_pending && playback_pcm.used<=1440);
}
int main(void) {
    recoverable_radio_waits_keep_source_and_receiver_time();
    actual_overflow_and_missing_data_still_recover();
    wrapped_backlog_and_other_input_limits();
    puts("phone catch-up: retained PCM/deadlines, live recovery poll, overflow, missing data, wrap and USB/call limits passed");
    return 0;
}
