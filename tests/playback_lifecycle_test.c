// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/btstack/playback_options.h"
#include "../src/btstack/playback_adjust.h"
static playback_adjust_t playback_adjust;
static playback_usb_verification_t usb_game_verification;
static void playback_usb_verify_cancel(void) {memset(&usb_game_verification,0,sizeof(usb_game_verification));}
static bool playback_usb_verify_poll(uint32_t time) {(void)time;return false;}
static void playback_usb_verify_sent(void) {}
#include "../src/tinyusb/dongle_protocol.h"

#define ERROR_CODE_SUCCESS 0
static bool is_usb_streaming, phone_active, phone_opened, profile_pending;
static bool call_active, call_capture;
static bool a2dp_connect_pending, a2dp_is_connected_flag, profile_reconnect;
static bool allow_switch_slot, finish_scan_avdtp_codec;
static bool have_aaceld_codec_capabilities, finish_setup_aac;
static playback_options_t playback_options, profile_previous;
#include "playback_defaults.inc"
static uint8_t address[6], profile_peer[6], game_peer[6], connected_address[6];
static uint8_t cur_codec, cur_capability;
static uint32_t now, profile_since, game_session, delay_revision, actual_bitrate;
static uint32_t reconnect_since, reconnect_delay = 1000;
static uint16_t reported_delay;
enum { PLAYBACK_RUNNING, PLAYBACK_SUSPENDING, PLAYBACK_SUSPENDED, PLAYBACK_STARTING };
static unsigned playback_idle_phase;
static bool mic_prepare_open, mic_recovery_pending, mic_recovery_started;
static uint32_t mic_prepare_serial, mic_prepare_session, mic_recovery_since, mic_recovery_revision;
static uint16_t mic_recovery_cid;
static unsigned playback_recoveries;

static void *handleAAC;
typedef struct { uint32_t timeout; } btstack_timer_source_t;
typedef struct {
    uint16_t avdtp_cid, avrcp_cid, local_seid;
    bool streaming;
    uint8_t codec_ready_to_send;
    uint32_t time_audio_data_sent, samples_ready, acc_num_missed_samples;
    btstack_timer_source_t audio_timer;
} a2dp_media_sending_context_t;
static a2dp_media_sending_context_t media_tracker;
static void playback_transport_recover(void *context) {(void)context;++playback_recoveries;media_tracker.streaming=false;}
static uint32_t audio_timer_interval=10, scheduled_interval, timer_adds, timer_removes;
static unsigned a2dp_sample_rate(void) { return 48000; }
static void btstack_run_loop_set_timer(btstack_timer_source_t *timer, uint32_t interval) {
    timer->timeout=now+interval+1u; scheduled_interval=interval+1u;
}
static void btstack_run_loop_remove_timer(btstack_timer_source_t *timer) { (void)timer; ++timer_removes; }
static void btstack_run_loop_add_timer(btstack_timer_source_t *timer) { (void)timer; ++timer_adds; }
static unsigned connects, disconnects, scans, keys_dropped;
static bool slot_write_ok, inquiry_ok, slot_cleared;
static bool game_ready, game_eligible, send_synchronously;
static bool aacp_audio_owned(void) {return true;}
static bool model_known=true;
static uint32_t control_session=1;
static unsigned game_feedback_count;
static bool game_feedback_enabled;
static bool inspect_enabled;
static uint32_t inspect_deadline;
static void set_led_game_feedback(bool enabled) {++game_feedback_count;game_feedback_enabled=enabled;}
static bool pcm_warm=true;
static bool playback_pcm_warm(void) {return pcm_warm;}
static uint32_t buffer_serial;
static uint16_t buffer_target;
static uint8_t buffer_result;
void dongle_playback_receive_buffer_sent(uint32_t now);

static bool mic_requested, mic_active;
static bool aacp_mic_requested(void) { return mic_requested || call_capture; }
static bool aacp_mic_active(void) { return mic_active; }
static bool aacp_mic_uplink_active(void) {return mic_active;}
static bool peer_address_valid(const uint8_t *p) { return p[0] != 0; }
static bool playback_profile_preflight(playback_options_t p, const uint8_t *peer) {
    (void)p; (void)peer; return true;
}
static void *get_device_addr(void) { return address; }
static uint32_t btstack_run_loop_get_time_ms(void) { return now; }
static int a2dp_source_disconnect(uint16_t cid) { (void)cid; ++disconnects; return 0; }
static int avdtp_source_connect(uint8_t *peer, uint16_t *cid) {
    ++connects; memcpy(connected_address, peer, 6); *cid = 20; return 0;
}
static void a2dp_demo_timer_stop(void *p) { (void)p; media_tracker.streaming = false; }
static void aacp_disconnect(void) {game_ready=false;}
static void aacp_mic_recovery_failed(void) {}
static uint32_t aacp_control_session(void) { return control_session; }
static uint32_t aacp_receive_buffer_serial(void) { return buffer_serial; }
static uint16_t aacp_receive_buffer_last(void) { return buffer_target; }
static bool aacp_control_ready(void) { return game_ready; }
static bool aacp_model_known(void) {return model_known;}
static bool playback_game_eligible(void) { return game_eligible; }
static uint8_t aacp_receive_buffer(uint16_t target) {
    if (buffer_result) return buffer_result;
    buffer_target=target;
    if (send_synchronously) {
        ++buffer_serial;
        dongle_playback_receive_buffer_sent(now);
    }
    return DONGLE_ACCEPTED;
}
static void playback_idle_poll(uint32_t time) { (void)time; }
static void playback_prepare_packet(void *context, uint32_t time) { (void)context; (void)time; }
static void playback_audio_work(uint32_t time) {
    if(media_tracker.codec_ready_to_send==2)playback_prepare_packet(&media_tracker,time);
}
static bool playback_phone_recovery_poll(void) {return false;}
static void l2cap_set_flushable_local_cid(uint16_t cid) { (void)cid; }
static void avrcp_disconnect(uint16_t cid) { (void)cid; }
static void audio_slot_queue_init(void) {}
static uint8_t read_uint8_last_flash(void) { return 1; }
static bool write_slot1_mac(const uint8_t *p) {
    static const uint8_t empty[6];
    assert(!memcmp(p, empty, 6));
    slot_cleared = slot_write_ok; return slot_write_ok;
}
static bool write_slot2_mac(const uint8_t *p) { return write_slot1_mac(p); }
static void set_led_mode_off(void) {}
static void gap_drop_link_key_for_bd_addr(uint8_t *p) {
    assert(!memcmp(p, address, 6)); ++keys_dropped;
}
static bool gap_start_scanning(void) { ++scans; return inquiry_ok; }
static uint16_t avdtp_subevent_signaling_connection_released_get_avdtp_cid(const uint16_t *p) {
    return *p;
}
static void aacEncClose(void **handle) { *handle = NULL; }
void avdtp_source_establish_stream(void);

// The runner extracts these functions and this callback case from the firmware.
#include "playback_cancel.inc"
#include "playback_lifecycle.inc"
static void signaling_released(uint16_t cid) {
    const uint16_t *packet = &cid;
    switch (0) {
        case 0:
#include "playback_release.inc"
        default: break;
    }
}

static void profile_then_pair(bool storage_succeeds, bool inquiry_succeeds) {
    static const uint8_t old_peer[6] = {1, 2, 3, 4, 5, 6};
    playback_options = (playback_options_t){0};
    playback_game = (playback_game_t){0};
    media_tracker.avdtp_cid = 10;
    a2dp_is_connected_flag = a2dp_accept_incoming = true;
    a2dp_connect_pending = profile_pending = profile_reconnect = false;
    memcpy(address, old_peer, 6);
    connects = disconnects = scans = keys_dropped = 0;
    slot_cleared = false;
    slot_write_ok = storage_succeeds; inquiry_ok = inquiry_succeeds; now = 100;

    assert(dongle_playback_configure(3, 0, 128000) == DONGLE_ACCEPTED);
    assert(profile_pending && profile_reconnect && disconnects == 1);
    // Pair supersedes the profile while the asynchronous disconnect is outstanding.
    assert(avdtp_disconnect_and_scan() == (storage_succeeds && inquiry_succeeds));
    assert(a2dp_is_connected_flag && media_tracker.avdtp_cid == 10);
    assert(!profile_pending && !profile_reconnect && !a2dp_accept_incoming);
    assert(slot_cleared == storage_succeeds);
    assert(scans == (unsigned)storage_succeeds && keys_dropped == (unsigned)storage_succeeds);
    assert(playback_options.codec == 3 && playback_options.bitrate == 128000);
    dongle_playback_poll();
    assert(connects == 0 && !a2dp_connect_pending);

    signaling_released(10);
    assert(!a2dp_is_connected_flag && media_tracker.avdtp_cid == 0);
    dongle_playback_poll();
    now += 16000;
    dongle_playback_poll();
    assert(connects == 0 && !a2dp_connect_pending && !a2dp_accept_incoming);
    assert(!profile_pending && !profile_reconnect && disconnects == 2);

    // A later legitimate profile request still reconnects the newly selected peer.
    address[0] = 7;
    a2dp_is_connected_flag = true; media_tracker.avdtp_cid = 11;
    assert(dongle_playback_configure(2, 0, 174000) == DONGLE_ACCEPTED);
    assert(profile_pending && profile_reconnect && disconnects == 3);
    signaling_released(11);
    dongle_playback_poll();
    assert(connects == 1 && a2dp_connect_pending && a2dp_accept_incoming);
    assert(!memcmp(connected_address, address, 6) && memcmp(connected_address, old_peer, 6));
    assert(!profile_reconnect && profile_pending);
}

static void game_receipts_follow_actual_send(void) {
    for (unsigned synchronous=0; synchronous<2; ++synchronous) {
        playback_game=(playback_game_t){0};
        profile_pending=profile_reconnect=a2dp_connect_pending=false;
        a2dp_is_connected_flag=true;
        game_ready=game_eligible=is_usb_streaming=true; media_tracker.streaming=false;
        memcpy(game_peer,address,6); game_session=1;
        buffer_serial=delay_revision=0; reported_delay=1500; buffer_result=0;
        send_synchronously=synchronous!=0; now=100;
        playback_game_request(&playback_game,true);
        dongle_playback_poll(); now+=500; dongle_playback_poll();
        assert(playback_game.changed);
        if (!synchronous) {
            assert(playback_game.waiting_tx && playback_game.phase==GAME_PENDING);
            // A report before the deferred send must not confirm that send.
            ++delay_revision; ++buffer_serial;
            dongle_playback_receive_buffer_sent(++now);
        }
        assert(!playback_game.waiting_tx && playback_game.waiting_report);
        assert(playback_game.phase==GAME_SENT);
        dongle_playback_poll(); assert(playback_game.phase==GAME_SENT);
        ++delay_revision; reported_delay=600; ++now; dongle_playback_poll();
        assert(playback_game.phase==GAME_CONFIRMED);
        media_tracker.streaming=true; dongle_playback_poll();
        assert(playback_game.phase==GAME_CONFIRMED && playback_game.changed);
        media_tracker.streaming=false; dongle_playback_poll();
        assert(playback_game.phase==GAME_CONFIRMED && playback_game.changed);
        playback_game_request(&playback_game,false);
        send_synchronously=true; dongle_playback_poll();
        assert(playback_game.phase==GAME_RESTORING && playback_game.waiting_report);
        ++delay_revision; reported_delay=1500; ++now; dongle_playback_poll();
        assert(!playback_game.changed && playback_game.phase==GAME_OFF);
    }
    playback_game=(playback_game_t){0}; playback_game_request(&playback_game,true);
    now=100; dongle_playback_poll(); now+=500;
    buffer_result=DONGLE_BUSY; dongle_playback_poll();
    assert(!playback_game.changed && !playback_game.waiting_tx);
    buffer_result=0; dongle_playback_poll();
    assert(playback_game.phase==GAME_SENT);
    // A fresh live report starts a bounded rate adjustment, without a reconnect.
    playback_game=(playback_game_t){0}; media_tracker.streaming=true;
    playback_game_request(&playback_game,true);
    now+=10000; dongle_playback_poll(); now+=500; dongle_playback_poll();
    assert(playback_game.phase==GAME_SENT && playback_game.changed);
    ++delay_revision; reported_delay=1590; ++now; dongle_playback_poll();
    assert(playback_adjust.pending && playback_adjust.interval==11);
    assert(playback_game.phase==GAME_PENDING);
    playback_adjust_start(&playback_adjust,now);
    now+=990; playback_adjust_sent(&playback_adjust,43200,now);
    dongle_playback_poll();
    assert(!playback_adjust_busy(&playback_adjust) && playback_game.phase==GAME_CONFIRMED);
    assert(media_tracker.streaming);
}

static void saved_pair_reconnects_with_backoff(void) {
    playback_game=(playback_game_t){0}; game_ready=game_eligible=false;
    a2dp_is_connected_flag=a2dp_connect_pending=profile_pending=profile_reconnect=false;
    a2dp_accept_incoming=true; address[0]=1;
    reconnect_since=connects=0; reconnect_delay=1000;
    now=999; dongle_playback_poll(); assert(connects==0);
    now=1000; dongle_playback_poll(); assert(connects==1 && a2dp_connect_pending);
    now=1001; dongle_playback_poll(); assert(connects==1);
    a2dp_connect_pending=false;
    now=3000; dongle_playback_poll(); assert(connects==1);
    now=3001; dongle_playback_poll(); assert(connects==2 && a2dp_connect_pending);
    // A successful connection resets the wait before the next case return.
    a2dp_connect_pending=false; a2dp_is_connected_flag=true;
    now=5000; dongle_playback_poll(); assert(reconnect_delay==1000);
    a2dp_is_connected_flag=false;
    now=5999; dongle_playback_poll(); assert(connects==2);
    now=6000; dongle_playback_poll(); assert(connects==3);
    // Pairing cancellation, profile negotiation and invalid identity inhibit retries.
    a2dp_connect_pending=false; a2dp_accept_incoming=false;
    now+=100000; dongle_playback_poll(); assert(connects==3);
    a2dp_accept_incoming=true; profile_pending=true; profile_since=now;
    memcpy(profile_peer,address,6);
    ++now; dongle_playback_poll(); assert(connects==3 && profile_pending);
    profile_pending=false; address[0]=0;
    now+=100000; dongle_playback_poll(); assert(connects==3);
    address[0]=1; reconnect_since=UINT32_MAX-500; reconnect_delay=1000;
    now=498; dongle_playback_poll(); assert(connects==3);
    now=499; dongle_playback_poll(); assert(connects==4);
    for (unsigned attempt=0; attempt<6; ++attempt) {
        a2dp_connect_pending=false; now+=reconnect_delay; dongle_playback_poll();
        assert(connects==5+attempt && reconnect_delay<=8000);
    }
}

static void standalone_boot_connects_in_normal_mode(void) {
    assert(a2dp_accept_incoming && !playback_game.requested && playback_game.phase==GAME_OFF);
    address[0]=1; now=1000;
    dongle_playback_poll();
    assert(connects==1 && a2dp_connect_pending);
    a2dp_is_connected_flag=game_ready=game_eligible=send_synchronously=true;
    a2dp_connect_pending=false; now+=1000;
    dongle_playback_poll();
    assert(!playback_game.requested && playback_game.waiting_report && buffer_target==150);
    ++delay_revision;reported_delay=1500;++now;dongle_playback_poll();
    assert(!playback_game.changed && !playback_game.waiting_report && playback_game.phase==GAME_OFF);
    assert(dongle_playback_game_mode(true)==DONGLE_ACCEPTED);
    now+=500;dongle_playback_poll();
    assert(playback_game.waiting_report && buffer_target==60);
    ++delay_revision; reported_delay=600; ++now; dongle_playback_poll();
    assert(playback_game.phase==GAME_CONFIRMED);
}
static void game_waits_for_encoder_warmup(void) {
    playback_game=(playback_game_t){0};playback_game_request(&playback_game,true);
    media_tracker.streaming=is_usb_streaming=game_ready=game_eligible=true;
    pcm_warm=false;now+=10000;
    dongle_playback_poll();now+=1000;dongle_playback_poll();
    assert(!playback_game.changed);
    pcm_warm=true;++now;dongle_playback_poll();
    now+=499;dongle_playback_poll();assert(!playback_game.changed);
    ++now;dongle_playback_poll();assert(playback_game.changed && playback_game.waiting_report);
}


static uint64_t prepare_live_adjustment(unsigned interval) {
    playback_game=(playback_game_t){.requested=true,.changed=true,.target=60,
                                    .phase=GAME_CONFIRMED,.active=true};
    profile_pending=profile_reconnect=a2dp_connect_pending=false;
    game_ready=game_eligible=is_usb_streaming=media_tracker.streaming=pcm_warm=true;
    phone_active=phone_opened=mic_requested=mic_active=false;
    memcpy(game_peer,address,6); game_session=1;
    send_synchronously=true; buffer_result=0;
    now+=20000;
    playback_adjust=(playback_adjust_t){.running=true,.interval=(uint8_t)interval,
                                       .started=now-20,.target=60};
    media_tracker.time_audio_data_sent=now-5;
    media_tracker.samples_ready=960; media_tracker.acc_num_missed_samples=12345;
    timer_adds=timer_removes=scheduled_interval=0;
    // Independent source-time calculation, including the fractional frame debt.
    uint64_t debt=960u*99000u+12345u+(uint64_t)5u*48000u*(interval==9 ? 110u : 90u);
    return debt*interval/10u;
}
static void assert_adjustment_normalized(uint64_t expected) {
    assert(!playback_adjust_busy(&playback_adjust));
    assert((uint64_t)media_tracker.samples_ready*99000u+
           media_tracker.acc_num_missed_samples==expected);
    assert(media_tracker.time_audio_data_sent==now);
    assert(scheduled_interval==10 && media_tracker.audio_timer.timeout==now+10);
    assert(timer_adds==1 && timer_removes==1 && media_tracker.streaming);
}
static void microphone_and_toggle_cancel_live_clock(void) {
    for (unsigned interval=9;interval<=11;interval+=2) {
        for (unsigned remote=0;remote<2;++remote) {
            uint64_t expected=prepare_live_adjustment(interval);
            mic_requested=!remote; mic_active=remote!=0;
            dongle_playback_poll();
            assert_adjustment_normalized(expected);
            assert(buffer_target==150); // Capture restores the normal receive buffer.
            // Repeated polling must not accrue or convert that debt a second time.
            dongle_playback_poll();
            assert_adjustment_normalized(expected);
        }
        uint64_t expected=prepare_live_adjustment(interval);
        assert(dongle_playback_game_mode(false)==DONGLE_ACCEPTED);
        assert_adjustment_normalized(expected);
        assert(!playback_game.requested);
        // A timeout clears running inside the policy; retain its prior clock domain.
        expected=prepare_live_adjustment(interval);
        playback_adjust.started=now-10000;
        dongle_playback_poll();
        assert_adjustment_normalized(expected);
        assert(playback_game.failed);
    }
    uint64_t unused=prepare_live_adjustment(11); (void)unused;
    media_tracker.streaming=false;
    playback_adjust_cancel_stream(&media_tracker,now,11);
    assert(!playback_adjust_busy(&playback_adjust));
    assert(media_tracker.samples_ready==960 && media_tracker.acc_num_missed_samples==12345);
    assert(!timer_adds && !timer_removes); // A stopped stream must never be restarted.
    mic_requested=mic_active=false;
}


static void microphone_prepares_normal_buffer_and_binds_repair(bool call) {
    prepare_live_adjustment(11);
    mic_requested=call_capture=false;
    bool *demand=call?&call_capture:&mic_requested;
    a2dp_is_connected_flag=finish_scan_avdtp_codec=true;
    media_tracker.local_seid=5; media_tracker.avdtp_cid=10;
    playback_idle_phase=PLAYBACK_RUNNING;
    mic_prepare_open=mic_recovery_pending=mic_recovery_started=false;
    *demand=true; buffer_serial=20; delay_revision=10;
    assert(!dongle_playback_mic_ready());
    // A fresh Pico boot may meet AirPods retaining 60 ms even before local metadata arrives.
    playback_adjust_cancel(&playback_adjust);
    playback_game.changed=false; game_eligible=false; model_known=false;
    dongle_playback_poll(); assert(!dongle_playback_mic_ready());
    model_known=game_eligible=true;
    dongle_playback_poll(); assert(buffer_target==150 && playback_game.waiting_report);
    assert(playback_game.requested && !dongle_playback_mic_ready());
    uint32_t sent=playback_game.sent_since;
    ++delay_revision; reported_delay=1500; now=sent+499; dongle_playback_poll();
    assert(!dongle_playback_mic_ready());
    ++now; dongle_playback_poll(); assert(dongle_playback_mic_ready());
    // An old normal-buffer receipt cannot authorize capture on a new control session.
    ++control_session; assert(!dongle_playback_mic_ready());
    dongle_playback_poll(); assert(playback_game.waiting_report && !dongle_playback_mic_ready());
    sent=playback_game.sent_since; ++delay_revision; now=sent+500; dongle_playback_poll();
    assert(dongle_playback_mic_ready());
    // A missing model/profile after AACP reconnect is not evidence that an old
    // Game Mode request stopped applying on the already-connected AirPods.
    playback_game.changed=true; playback_game.target=60; game_eligible=false;
    assert(!dongle_playback_mic_ready());
    playback_game.changed=false; playback_game.waiting_tx=true;
    assert(!dongle_playback_mic_ready());
    playback_game.waiting_tx=false; playback_game.waiting_report=true;
    assert(!dongle_playback_mic_ready());
    playback_game.waiting_report=false; playback_game.restore_failed=true;
    assert(!dongle_playback_mic_ready());
    // A failed fresh-150 attempt can be retried once by actually closing/opening capture.
    game_eligible=true; playback_game.changed=true; playback_game.target=150;
    *demand=false; dongle_playback_poll(); assert(playback_game.restore_failed);
    *demand=true; dongle_playback_poll();
    assert(!playback_game.restore_failed && playback_game.waiting_report);
    sent=playback_game.sent_since; ++delay_revision; now=sent+500; dongle_playback_poll();
    assert(dongle_playback_mic_ready());
    // A close/open coalesced by USB callbacks has final wanted=true, but a new revision.
    playback_game.restore_failed=playback_game.changed=true; playback_game.target=150;
    ++control_session; dongle_playback_poll();
    assert(playback_game.restore_failed); // An ordinary AACP session change is not a user retry.
    dongle_playback_mic_request_changed(); dongle_playback_poll();
    assert(!playback_game.restore_failed && playback_game.waiting_report);
    sent=playback_game.sent_since; ++delay_revision; now=sent+500; dongle_playback_poll();
    assert(dongle_playback_mic_ready());
    // Reopening during physical cleanup cannot clear that operation's failure guard.
    playback_game.restore_failed=true; mic_active=true;
    *demand=false; dongle_playback_poll(); *demand=true; dongle_playback_poll();
    assert(playback_game.restore_failed && !dongle_playback_mic_ready());
    playback_game.restore_failed=false; playback_game.changed=false;
    *demand=false; mic_active=true; playback_recoveries=0;
    uint32_t before=dongle_playback_mic_recovery_revision();
    assert(dongle_playback_mic_recover()); dongle_playback_poll();
    assert(mic_recovery_pending && mic_recovery_started && playback_recoveries==1);
    dongle_playback_poll(); assert(playback_recoveries==1);
    assert(dongle_playback_mic_recovery_revision()==before && playback_game.requested);
    // A release/new peer before another poll cannot carry the old repair forward.
    signaling_released(11); assert(mic_recovery_pending);
    signaling_released(10); assert(!mic_recovery_pending && !mic_prepare_open);
    mic_active=false; control_session=1; game_session=1;
}

int main(void) {
    standalone_boot_connects_in_normal_mode();
    profile_then_pair(true, true);
    profile_then_pair(false, true);
    profile_then_pair(true, false);
    game_receipts_follow_actual_send();
    game_waits_for_encoder_warmup();
    saved_pair_reconnects_with_backoff();
    microphone_and_toggle_cancel_live_clock();
    microphone_prepares_normal_buffer_and_binds_repair(false);
    microphone_prepares_normal_buffer_and_binds_repair(true);
    playback_game=(playback_game_t){.requested=true};game_eligible=true;
    assert(dongle_playback_game_toggle()==DONGLE_ACCEPTED && !playback_game.requested);
    assert(game_feedback_count==1 && !game_feedback_enabled);
    assert(dongle_playback_game_toggle()==DONGLE_ACCEPTED && playback_game.requested);
    assert(game_feedback_count==2 && game_feedback_enabled);
    playback_game.requested=false;game_eligible=false;
    assert(dongle_playback_game_toggle()==DONGLE_UNSUPPORTED && !playback_game.requested);
    assert(game_feedback_count==2);
    inspect_enabled=true;inspect_deadline=now+10;
    now+=9;dongle_playback_poll();assert(inspect_enabled);
    ++now;dongle_playback_poll();assert(!inspect_enabled);
    puts("playback lifecycle: Pair cancels pending profile reconnect, failures stay closed, fresh profile works");
}
