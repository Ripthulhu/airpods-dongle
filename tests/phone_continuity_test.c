// Actual production encoding preparation/send/recovery, with synthetic codec bytes.
#define main baseline_timing_main
#include "playback_timing_test.c"
#undef main

static void fixture_phone(void) {
    initialize(4,480,324,10,986);
    playback_game=(playback_game_t){0};
    playback_idle_phase=PLAYBACK_RUNNING;
    is_usb_streaming=false;
    phone_opened=phone_active=phone_buffer_started=true;
}
static void packet(unsigned elapsed_ms) {
    now+=elapsed_ms;
    media_tracker.samples_ready+=960;
    // This fixture injects the credited packet directly instead of running the source timer.
    media_tracker.time_audio_data_sent=now;
    drain();
}
static void cumulative_stalls_recover(void) {
    fixture_phone();packet(20);
    for(unsigned i=0;i<7;++i) {packet(40);assert(!playback_transport_stale);}
    packet(40);
    assert(phone_continuity.deficit_us==160000 && playback_transport_stale);
    // Every gap is below 150 ms; the previous single-gap guard missed this.
    allow_recovery=true;
    playback_transport_recover(&media_tracker);
    assert(playback_metrics.recoveries==1 && suspend_recovery_pending && phone_output_recovery);
    assert(!media_tracker.streaming && !phone_continuity.armed);
    playback_transport_recover(&media_tracker);
    assert(playback_metrics.recoveries==1);

    fixture_phone();playback_game.changed=true;playback_game.target=60;packet(20);
    for(unsigned i=0;i<3;++i) {packet(40);assert(!playback_transport_stale);}
    packet(40);assert(playback_transport_stale);
}
static void catchup_repays_shortage(void) {
    fixture_phone();packet(20);packet(100);
    assert(phone_continuity.deficit_us==80000 && !playback_transport_stale);
    for(unsigned i=0;i<4;++i)packet(0);
    assert(!phone_continuity.deficit_us && !playback_transport_stale);
    for(unsigned i=0;i<1000;++i)packet(20);
    assert(!phone_continuity.deficit_us && !playback_transport_stale);
}
static void aggregation_and_wrap_stay_continuous(void) {
    fixture_phone();
    // Source packets switch between 720/15 ms and 960/20 ms. AAC still emits
    // two 480-frame units per packet, independent of the incoming boundary.
    for(unsigned elapsed=0;elapsed<600000;) {
        unsigned frames=(elapsed/10000u)%2u?960u:720u;
        now+=frames/48u;elapsed+=frames/48u;
        media_tracker.samples_ready+=frames;
        media_tracker.time_audio_data_sent=now;
        drain();
        assert(!playback_transport_stale);
    }
    fixture_phone();
    now=UINT32_MAX/1000u-30u;
    for(unsigned i=0;i<10;++i)packet(20);
    assert(!phone_continuity.deficit_us && !playback_transport_stale);
}
static void inactive_epochs_do_not_accumulate(void) {
    for(unsigned reason=0;reason<6;++reason) {
        fixture_phone();packet(20);packet(100);
        switch(reason) {
            case 0:phone_buffer_started=false;break;
            case 1:audio_owned=false;break;
            case 2:is_usb_streaming=true;break;
            case 3:call_active=true;break;
            case 4:playback_game.waiting_report=true;break;
            case 5:
                playback_adjust_request(&playback_adjust,1500,60);
                playback_adjust_start(&playback_adjust,now);
                assert(playback_adjust.running && playback_adjust.interval==11);
                break;
        }
        packet(20);assert(!phone_continuity.armed);
        if(reason==5)assert(playback_adjust.running);
        phone_buffer_started=audio_owned=true;is_usb_streaming=call_active=false;
        playback_game.waiting_report=false;
        playback_adjust_cancel_stream(&media_tracker,now,playback_adjust_interval(&playback_adjust));
        packet(20);assert(phone_continuity.armed && !phone_continuity.deficit_us);
        assert(!playback_transport_stale);
    }
}
int main(void) {
    cumulative_stalls_recover();
    catchup_repays_shortage();
    aggregation_and_wrap_stay_continuous();
    inactive_epochs_do_not_accumulate();
    puts("phone continuity: cumulative short stalls recover; catch-up, aggregation, wrap and inactive epochs pass");
    return 0;
}
