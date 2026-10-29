// Real Start/PCM priming/send receipt and the production Game policy block.
#define USB_GAME_VERIFICATION_TEST
#define main existing_idle_suite_main
#include "playback_idle_test.c"
#undef main

static uint32_t control_session,delay_revision,buffer_serial,request_count;
static uint32_t requests_60,requests_150;
static uint32_t mic_prepare_serial;
static uint16_t reported_delay,buffer_last,queued_target;
static bool defer_send,control_ready,admission_busy;
static unsigned admission_attempts;
static bool low_latency_flush;
static void bt_hci_airpods_flush_timeout(bool enabled) {low_latency_flush=enabled;}
static void verification_test_buffer_cancel(void) {queued_target=0;}
#define DONGLE_ACCEPTED 0
static bool aacp_control_ready(void) {return control_ready;}
static bool aacp_mic_uplink_active(void) {return false;}
static uint32_t aacp_control_session(void) {return control_session;}
static uint32_t aacp_receive_buffer_serial(void) {return buffer_serial;}
static uint16_t aacp_receive_buffer_last(void) {return buffer_last;}
static void actual_send(void) {
    assert(queued_target==60 || queued_target==150);
    buffer_last=queued_target;++buffer_serial;queued_target=0;
    playback_game_observe_send(&playback_game,now,buffer_serial,buffer_last,delay_revision);
    playback_usb_verify_sent();
}
static unsigned aacp_receive_buffer(uint16_t target) {
    assert(target==60 || target==150);
    ++admission_attempts;if(admission_busy)return 1;
    ++request_count;if(target==60)++requests_60;else ++requests_150;
    queued_target=target;if(!defer_send)actual_send();return DONGLE_ACCEPTED;
}
static void playback_adjust_cancel_stream(a2dp_media_sending_context_t *context,uint32_t time,uint8_t interval) {
    assert(context==&media_tracker);(void)time;(void)interval;
    playback_adjust_cancel(&playback_adjust);
}
#include "usb_game_verification.inc"

static void reset_test(void) {
    fixture();playback_usb_verify_cancel();
    control_session=1;delay_revision=buffer_serial=request_count=requests_60=requests_150=0;
    mic_prepare_serial=0;reported_delay=UINT16_MAX;buffer_last=queued_target=0;
    defer_send=admission_busy=false;control_ready=true;game_eligible=true;admission_attempts=0;
}
static void fresh_report(uint16_t delay) {
    reported_delay=delay;++delay_revision;++now;production_game_poll();
}
static void cold_prepared_before_start(void) {
    reset_test();a2dp_demo_timer_stop(&media_tracker);
    assert(playback_pcm_reset(&playback_pcm,7680,false));
    set_usb_streaming(true);
    playback_game=(playback_game_t){.requested=true,.phase=GAME_PENDING};
    playback_idle_phase=PLAYBACK_SUSPENDED;
    now=0;production_game_poll();assert(!request_count && !starts);
    now=500;production_game_poll();assert(requests_60==1 && playback_game.waiting_report && !starts);
    fresh_report(920);
    assert(playback_game.phase==GAME_CONFIRMED && !playback_adjust_busy(&playback_adjust));
    assert(low_latency_flush);
    assert(starts==1 && !media_tracker.streaming);
    accept(AVDTP_SI_START,5);
    assert(media_tracker.streaming && playback_pcm.priming==4800 && !playback_pcm_warm());
}
static void prime_with_real_packet_receipts(void) {
    int16_t pcm[960];const uint32_t before=request_count;
    for(unsigned block=0;block<10;++block) {
        now+=10;assert(playback_pcm_read(&playback_pcm,pcm,480)==0);
        if(block%2==1)playback_media_sent();
        production_game_poll();
        assert(playback_pcm_warm()==(block==9));
        if(block<9)assert(request_count==before);
    }
}
static void post_prime_verification_uses_fresh_send_and_report(void) {
    cold_prepared_before_start();defer_send=true;
    prime_with_real_packet_receipts();
    assert(requests_60==2 && !requests_150 && playback_game.waiting_tx);
    assert(low_latency_flush);
    assert(!playback_adjust_busy(&playback_adjust));
    fresh_report(600); // This report predates the deferred command's real send.
    assert(playback_game.waiting_tx && !playback_adjust_busy(&playback_adjust));
    actual_send();production_game_poll();
    assert(playback_game.waiting_report && !playback_adjust_busy(&playback_adjust));
    fresh_report(920);
    assert(playback_adjust.pending && playback_adjust.interval==11 && playback_adjust.goal_frames==22u*48u);
    assert(!playback_game.waiting_tx && !playback_game.waiting_report);
    for(unsigned i=0;i<100;++i){now+=10;production_game_poll();}
    assert(requests_60==2); // One verification, never a periodic re-tune loop.
    // The separate timing suite drives the real packet worker. Here finish the
    // production adjustment accounting so the token reaches confirmed, not canceled.
    playback_adjust_start(&playback_adjust,now);now+=1000;
    playback_adjust_sent(&playback_adjust,43200,now);
    assert(!playback_adjust_busy(&playback_adjust));
    production_game_poll();production_game_poll();
    set_usb_streaming(false);production_game_poll();set_usb_streaming(true);production_game_poll();
    assert(requests_60==2); // A completed token is not rearmed by a warm USB pause.
}
static void manual_live_on_already_uses_live_adjustment(void) {
    reset_test();set_usb_streaming(true);
    playback_game_request(&playback_game,true);
    production_game_poll();now+=500;production_game_poll();assert(requests_60==1);
    fresh_report(920);assert(playback_adjust.pending);
    for(unsigned i=0;i<100;++i){now+=10;production_game_poll();}
    assert(requests_60==1 && !requests_150);
}
static void start_ack_before_pre_live_report_still_verifies(void) {
    reset_test();a2dp_demo_timer_stop(&media_tracker);
    assert(playback_pcm_reset(&playback_pcm,7680,false));set_usb_streaming(true);
    playback_idle_phase=PLAYBACK_SUSPENDED;production_game_poll();
    assert(!starts && requests_150==1 && !requests_60 && playback_game.waiting_report);
    fresh_report(1500);
    assert(starts==1 && playback_idle_phase==PLAYBACK_STARTING);
    // Default OFF admitted Start first; the user explicitly enables ON before its ACK.
    playback_game_request(&playback_game,true);now=500;production_game_poll();
    assert(requests_60==1 && playback_game.waiting_report && !media_tracker.streaming);
    accept(AVDTP_SI_START,5);assert(playback_pcm.priming==4800);
    fresh_report(920);assert(!playback_adjust_busy(&playback_adjust));
    prime_with_real_packet_receipts();assert(requests_60==2 && playback_game.waiting_report);
    fresh_report(920);assert(playback_adjust.pending);
}
static void warm_phone_to_usb_gets_one_verification(void) {
    reset_test();phone_opened=phone_active=true;
    playback_game=(playback_game_t){.requested=true,.changed=true,.target=60,.phase=GAME_CONFIRMED};
    assert(a2dp_demo_timer_start(&media_tracker));
    assert(usb_game_verification.unverified && !usb_game_verification.needed);
    prime_with_real_packet_receipts();assert(!request_count);
    phone_opened=phone_active=false;set_usb_streaming(true);production_game_poll();
    assert(requests_60==1 && playback_game.waiting_report && !starts);
    fresh_report(920);assert(playback_adjust.pending);
    for(unsigned i=0;i<100;++i){now+=10;production_game_poll();}
    assert(requests_60==1);
}
static void interrupted_verification_never_uses_old_proof(void) {
    cold_prepared_before_start();set_usb_streaming(false);production_game_poll();
    set_usb_streaming(true);production_game_poll();prime_with_real_packet_receipts();
    assert(requests_60==2 && playback_game.waiting_report); // Incomplete need survives, bound to new USB activity.
    fresh_report(920);assert(playback_adjust.pending);

    cold_prepared_before_start();defer_send=true;prime_with_real_packet_receipts();
    assert(requests_60==2 && playback_game.waiting_tx);
    set_usb_streaming(false);production_game_poll();assert(!queued_target);
    set_usb_streaming(true);production_game_poll();
    assert(requests_60==3 && playback_game.waiting_tx);
    fresh_report(600);assert(playback_game.waiting_tx && !playback_adjust_busy(&playback_adjust));
    actual_send();fresh_report(920);
    assert(playback_adjust.pending);

    cold_prepared_before_start();audio_owned=false;dongle_playback_ownership_changed(false);
    for(unsigned i=0;i<20;++i){now+=100;production_game_poll();}
    assert(requests_60==1 && !playback_adjust_busy(&playback_adjust));
    cold_prepared_before_start();++control_session;prime_with_real_packet_receipts();
    assert(requests_60==1); // The old AACP session cannot authorize a retry.
    cold_prepared_before_start();selected_peer[0]=9;prime_with_real_packet_receipts();
    assert(requests_60==1); // Neither can another selected peer.
    cold_prepared_before_start();media_tracker.avdtp_cid=11;prime_with_real_packet_receipts();assert(requests_60==1);
    cold_prepared_before_start();media_tracker.local_seid=6;prime_with_real_packet_receipts();assert(requests_60==1);
}
static void cancellation_and_missing_receipts_are_bounded(void) {
    cold_prepared_before_start();playback_game_request(&playback_game,false);production_game_poll();
    prime_with_real_packet_receipts();assert(requests_60==1 && requests_150==1);
    cold_prepared_before_start();defer_send=true;prime_with_real_packet_receipts();
    now+=2000;production_game_poll();assert(playback_game.failed && requests_60==2);
    now+=2000;production_game_poll();now+=5000;production_game_poll();assert(requests_60==2 && requests_150<=1);
    cold_prepared_before_start();admission_busy=true;prime_with_real_packet_receipts();
    const unsigned initial_attempts=admission_attempts;
    for(unsigned i=0;i<30;++i){now+=100;production_game_poll();}
    assert(admission_attempts>initial_attempts && requests_60==1 && !usb_game_verification.needed);
    const unsigned attempts=admission_attempts;admission_busy=false;
    now+=5000;production_game_poll();assert(admission_attempts==attempts && requests_60==1);
    cold_prepared_before_start();prime_with_real_packet_receipts();
    now+=2000;production_game_poll();assert(playback_game.failed && requests_60==2);
    now+=2000;production_game_poll();now+=5000;production_game_poll();assert(requests_60==2 && requests_150<=1);
}
int main(void) {
    post_prime_verification_uses_fresh_send_and_report();
    manual_live_on_already_uses_live_adjustment();
    start_ack_before_pre_live_report_still_verifies();
    warm_phone_to_usb_gets_one_verification();
    interrupted_verification_never_uses_old_proof();
    cancellation_and_missing_receipts_are_bounded();
    puts("USB Game verification: real Start/4800-frame priming/send receipts, fresh reports, once-only live adjustment and epoch cancellation passed");
    return 0;
}
