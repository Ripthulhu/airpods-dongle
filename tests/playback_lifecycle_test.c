// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/btstack/playback_options.h"
#include "../src/tinyusb/dongle_protocol.h"

#define ERROR_CODE_SUCCESS 0
static bool is_usb_streaming, phone_active, phone_opened, profile_pending;
static bool a2dp_connect_pending, a2dp_is_connected_flag, profile_reconnect, a2dp_accept_incoming;
static bool allow_switch_slot, finish_scan_avdtp_codec;
static bool have_aaceld_codec_capabilities, finish_setup_aac;
static playback_options_t playback_options, profile_previous;
static playback_game_t playback_game;
static uint8_t address[6], profile_peer[6], game_peer[6], connected_address[6];
static uint8_t cur_codec, cur_capability;
static uint32_t now, profile_since, game_session, delay_revision, actual_bitrate;
static uint16_t reported_delay;
static void *handleAAC;
static struct { uint16_t avdtp_cid, avrcp_cid, local_seid; bool streaming; uint8_t codec_ready_to_send; } media_tracker;
static unsigned connects, disconnects, scans, keys_dropped;
static bool slot_write_ok, inquiry_ok, slot_cleared;

static bool usb_mic_requested(void) { return false; }
static bool aacp_mic_active(void) { return false; }
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
static void aacp_disconnect(void) {}
static uint32_t aacp_control_session(void) { return 1; }
static uint32_t aacp_receive_buffer_serial(void) { return 0; }
static uint16_t aacp_receive_buffer_last(void) { return 0; }
static bool aacp_control_ready(void) { return false; }
static bool playback_game_eligible(void) { return false; }
static uint8_t playback_input(void) { return 0; }
static uint8_t aacp_receive_buffer(uint16_t target) { (void)target; return 0; }
static void playback_idle_poll(uint32_t time) { (void)time; }
static void playback_prepare_packet(void *context, uint32_t time) { (void)context; (void)time; }
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

int main(void) {
    profile_then_pair(true, true);
    profile_then_pair(false, true);
    profile_then_pair(true, false);
    puts("playback lifecycle: Pair cancels pending profile reconnect, failures stay closed, fresh profile works");
}
