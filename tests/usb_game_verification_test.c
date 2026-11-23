// Real Start/PCM priming/send receipt and the production Game policy block.
#define USB_GAME_VERIFICATION_TEST
#define main existing_idle_suite_main
#include "playback_idle_test.c"
#undef main

static uint32_t delay_revision,buffer_serial,request_count;
static uint32_t requests_60,requests_150;
static uint32_t mic_prepare_serial;
static uint16_t reported_delay,buffer_last,queued_target;
static bool defer_send,admission_busy;
static unsigned admission_attempts;
static bool low_latency_flush;
static bool playback_release_pending;
// Releasing the transport is tested by the separate lifecycle suite.
static void playback_release_close(void) {assert(false);}
static void bt_hci_airpods_flush_timeout(bool enabled) {low_latency_flush=enabled;}
static void verification_test_buffer_cancel(void) {queued_target=0;}
#define DONGLE_ACCEPTED 0
static bool aacp_mic_uplink_active(void) {return false;}
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
static void claim_game_fixture(bool requested) {
    reset_test();a2dp_demo_timer_stop(&media_tracker);
    audio_owned=false;claim_known=claim_pending=true;claim_stage=3;
    set_usb_streaming(true);
    playback_game=(playback_game_t){.requested=requested,.phase=requested?GAME_PENDING:GAME_OFF};
    production_game_poll();
    assert_control_only(1);
    assert(playback_claim.active && playback_idle_phase==PLAYBACK_STARTING && !request_count);
}
static void claimed_start_proves_buffer_only_after_media(void) {
    for(unsigned game=0;game<2;++game)for(unsigned owner_before_ack=0;owner_before_ack<2;++owner_before_ack) {
        claim_game_fixture(game!=0);
        if(owner_before_ack) {
            audio_owned=true;claim_pending=false;dongle_playback_ownership_changed(true);
        }
        // Ownership can arrive before the Start ACK. Neither ordering authorizes
        // receiver-buffer commands while this control-only attempt is pending.
        now+=600;production_game_poll();assert_control_only(1);assert(!request_count);
        accept(AVDTP_SI_START,5);
        assert(playback_idle_phase==PLAYBACK_CLAIMING);
        if(!owner_before_ack) {
            ++now;production_game_poll();assert_control_only(1);assert(!request_count);
            audio_owned=true;claim_pending=false;dongle_playback_ownership_changed(true);
        }
        ++now;production_game_poll();
        assert(starts==1 && resets==1 && media_tracker.streaming && !playback_claim.active);
        assert(!request_count && !playback_game.waiting_report && !playback_game.failed);
        assert(playback_pcm.priming==4800 && !playback_pcm_warm());
        production_game_poll();assert(!request_count); // A timer alone is not RTP progress.
        prime_with_real_packet_receipts();
        if(game) {
            assert(!request_count);now+=500;production_game_poll();
            assert(requests_60==1 && !requests_150);
        } else assert(requests_150==1 && !requests_60);
        assert(playback_game.waiting_report && !playback_game.failed);
        uint32_t sent=playback_game.sent_since;
        fresh_report(0); // The pre-media zero report never confirms or renews a command.
        assert(playback_game.waiting_report && playback_game.sent_since==sent);
        fresh_report(game?1380:1500);
        assert(!playback_game.waiting_report && !playback_game.failed && !playback_game.restore_failed);
        if(game) {
            assert(playback_adjust.pending);
            playback_adjust_start(&playback_adjust,now);now+=1000;
            playback_adjust_sent(&playback_adjust,43200,now,true);
            production_game_poll();
            assert(playback_game.phase==GAME_CONFIRMED);
        } else assert(playback_game.phase==GAME_OFF && !playback_game.changed);
        assert(request_count==1 && starts==1 && !suspends && !closes);
    }
}
static void claiming_defers_old_restoration_without_faking_receipt(void) {
    for(unsigned target=60;target<=150;target+=90) {
        claim_game_fixture(false);
        // Real ownership loss clears this debt. Seed it explicitly to ensure
        // the existing urgent restoration path cannot bypass control-Start.
        playback_game=(playback_game_t){.changed=true,.target=(uint16_t)target,.phase=GAME_RESTORING};
        audio_owned=true;claim_pending=false;dongle_playback_ownership_changed(true);
        now+=600;production_game_poll();assert_control_only(1);assert(!request_count);
        accept(AVDTP_SI_START,5);production_game_poll();
        assert(media_tracker.streaming && !playback_claim.active && starts==1 && !request_count);
        assert(playback_game.changed && !playback_game.waiting_report);
        // Once the normal pipeline is started, ordinary urgent restoration is
        // unchanged; it still requires a real fresh report to clear the debt.
        production_game_poll();assert(requests_150==1 && playback_game.waiting_report);
        fresh_report(0);assert(playback_game.changed && playback_game.waiting_report);
        fresh_report(1500);assert(!playback_game.changed && playback_game.phase==GAME_OFF);
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
    playback_adjust_sent(&playback_adjust,43200,now,true);
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
static void failed_off_retries_once_on_fresh_owned_usb_playback(void) {
    reset_test();set_usb_streaming(true);playback_media_sent();
    production_game_poll();assert(requests_150==1);
    fresh_report(780);assert(playback_adjust.pending && playback_game.phase==GAME_RESTORING);
    playback_adjust_cancel(&playback_adjust);playback_adjust.failed=true;
    production_game_poll();assert(playback_game.phase==GAME_FAILED);
    for(unsigned i=0;i<20;++i){now+=10;playback_media_sent();production_game_poll();}
    assert(requests_150==1 && playback_game.phase==GAME_FAILED);

    set_usb_streaming(false);set_usb_streaming(true);
    production_game_poll();assert(requests_150==1); // A host Start alone cannot issue a retry.
    audio_owned=false;playback_media_sent();production_game_poll();
    assert(requests_150==1 && playback_game.phase==GAME_FAILED);
    audio_owned=true;defer_send=true;playback_media_sent();production_game_poll();
    assert(requests_150==2 && playback_game.waiting_tx);
    fresh_report(1500);assert(playback_game.waiting_tx);
    actual_send();production_game_poll();
    fresh_report(0);assert(playback_game.waiting_report);
    fresh_report(1100);assert(playback_adjust.pending && playback_game.phase==GAME_RESTORING);
    playback_adjust_start(&playback_adjust,now);now+=400;
    playback_adjust_sent(&playback_adjust,21200,now,true);production_game_poll();
    assert(!playback_adjust_busy(&playback_adjust) && playback_game.phase==GAME_OFF);
    for(unsigned i=0;i<20;++i){now+=10;playback_media_sent();production_game_poll();}
    assert(requests_150==2 && !requests_60);
}
int main(void) {
    failed_off_retries_once_on_fresh_owned_usb_playback();
    claimed_start_proves_buffer_only_after_media();
    claiming_defers_old_restoration_without_faking_receipt();
    post_prime_verification_uses_fresh_send_and_report();
    manual_live_on_already_uses_live_adjustment();
    start_ack_before_pre_live_report_still_verifies();
    warm_phone_to_usb_gets_one_verification();
    interrupted_verification_never_uses_old_proof();
    cancellation_and_missing_receipts_are_bounded();
    puts("USB Game verification: real Start/4800-frame priming/send receipts, fresh reports, once-only live adjustment and epoch cancellation passed");
    return 0;
}
