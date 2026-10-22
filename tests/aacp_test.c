#include <assert.h>
#include <stdio.h>
#include "../src/btstack/btstack_aacp.c"

static uint32_t now;
static uint16_t next_cid, last_disconnected;
static unsigned connects, disconnects, sends, starts, stops, decoded, resets;
static uint8_t create_result, send_result;
static uint16_t remote_mtu = 512;
static bool decoder_ready = true, decode_ok = true;
static bool decoder_reset_done = true, complete_on_submit = true;
static uint32_t completed;
static bool usb_capture_wanted;
static bd_addr_t peer = {1, 2, 3, 4, 5, 6};

void btstack_run_loop_remove_timer(btstack_timer_source_t *timer) { timer->active = false; }
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *timer,
                                      void (*handler)(btstack_timer_source_t *)) {
    timer->handler = handler;
}
void btstack_run_loop_set_timer(btstack_timer_source_t *timer, uint32_t delay) { timer->delay = delay; }
void btstack_run_loop_add_timer(btstack_timer_source_t *timer) { timer->active = true; }
uint32_t btstack_run_loop_get_time_ms(void) { return now; }
uint8_t l2cap_create_channel(btstack_packet_handler_t handler, bd_addr_t addr,
                             uint16_t psm, uint16_t mtu, uint16_t *cid) {
    (void)handler; (void)addr; (void)mtu;
    assert(psm == AACP_PSM);
    connects++;
    if (create_result == 0) *cid = next_cid++;
    return create_result;
}
void l2cap_disconnect(uint16_t cid) { disconnects++; last_disconnected = cid; }
void l2cap_request_can_send_now_event(uint16_t cid) { assert(cid != 0); }
uint16_t l2cap_get_remote_mtu_for_local_cid(uint16_t cid) { assert(cid != 0); return remote_mtu; }
uint8_t l2cap_send(uint16_t cid, uint8_t *data, uint16_t len) {
    assert(cid == aacp_cid && len != 0);
    if (send_result != 0) return send_result;
    sends++;
    if (data == aacp_mic_start_bytes) starts++;
    if (data == aacp_mic_stop_bytes) stops++;
    return 0;
}
bool aacp_mic_dec_ready(void) { return decoder_ready; }
bool usb_mic_requested(void) { return usb_capture_wanted; }
bool aacp_mic_dec_reset(void) { resets++; return decoder_ready; }
bool aacp_mic_dec_reset_done(void) { return decoder_reset_done; }
uint32_t aacp_mic_dec_completed(void) { return completed; }
void aacp_mic_dec_reset_stats(void) {}
void aacp_mic_dec_report(void) {}
void aacp_mic_pcm_reset(void) {}
bool aacp_mic_dec_submit(const uint8_t *au, uint16_t len) {
    assert(au != NULL && len != 0);
    decoded++;
    if (decode_ok && complete_on_submit) completed++;
    return decode_ok;
}

static void fire(btstack_timer_source_t *timer) {
    assert(timer->active);
    timer->active = false;
    timer->handler(timer);
}
static void event(uint8_t type, uint16_t cid) {
    uint8_t p[] = {type, 2, (uint8_t)cid, (uint8_t)(cid >> 8)};
    aacp_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}
static void opened(uint16_t cid, uint8_t status, bd_addr_t addr) {
    uint8_t p[26] = {L2CAP_EVENT_CHANNEL_OPENED, 24, status};
    for (unsigned i = 0; i < 6; ++i) p[3 + i] = addr[5 - i];
    p[13] = (uint8_t)cid;
    p[14] = (uint8_t)(cid >> 8);
    aacp_packet_handler(HCI_EVENT_PACKET, 0, p, sizeof(p));
}
static void control(uint16_t cid) {
    uint8_t p[] = {4, 0, 4, 0, 9, 0, 0x0d, 3, 0, 0, 0};
    aacp_packet_handler(L2CAP_DATA_PACKET, cid, p, sizeof(p));
}
static void drain(void) {
    unsigned limit = 20;
    while (aacp_tx_count != 0 && limit-- != 0) event(L2CAP_EVENT_CAN_SEND_NOW, aacp_cid);
    assert(aacp_tx_count == 0);
}
static void quiet_after_stop(void) {
    now += AACP_MIC_QUIET_MS;
    aacp_poll();
    drain();
}
static void fixture(void) {
    aacp_init();
    aacp_research_cancel();
    remote_mtu = 512;
    now = 0;
    next_cid = 0x40;
    last_disconnected = 0;
    connects = disconnects = sends = starts = stops = decoded = resets = 0;
    create_result = send_result = 0;
    decoder_ready = decode_ok = true;
    decoder_reset_done = complete_on_submit = true;
    completed = 0;
    usb_capture_wanted = false;
}
static void request_capture(void) {
    usb_capture_wanted = true;
    aacp_mic_start();
}
static void close_capture(void) {
    usb_capture_wanted = false;
    aacp_mic_stop();
}
static void connect_open(void) {
    aacp_connect(peer);
    fire(&aacp_connect_timer);
    opened(aacp_cid, 0, peer);
}
static void ready(void) {
    connect_open();
    drain();
    control(aacp_cid);
}
static void capture(void) {
    ready();
    request_capture();
    drain();
    assert(aacp_mic_active());
}
static unsigned audio(uint8_t *p, uint16_t cid, unsigned size) {
    aacp_packet_handler(L2CAP_DATA_PACKET, cid, p, (uint16_t)size);
    aacp_poll();
    return decoded;
}

static void test_handshake_and_decoder_gate(void) {
    fixture();
    decoder_ready = false;
    request_capture();
    connect_open();
    control(aacp_cid);
    assert(starts == 0 && !aacp_mic_active());
    drain();
    aacp_poll();
    assert(starts == 0);
    decoder_ready = true;
    aacp_poll();
    assert(aacp_tx_count == 1 && !aacp_mic_active());
    drain();
    assert(starts == 1 && aacp_mic_active());
}

static void test_usb_cancel_and_stop_are_not_lost(void) {
    fixture();
    ready();
    request_capture();
    close_capture();
    drain();
    quiet_after_stop();
    assert(starts == 0 && stops == 0 && !aacp_mic_active());
    request_capture();
    drain();
    close_capture();
    assert(aacp_mic_active()); // STOP has not left the queue yet.
    request_capture();
    close_capture();
    drain();
    quiet_after_stop();
    assert(starts == 1 && stops == 1 && !aacp_mic_active());
    aacp_poll();
    assert(aacp_tx_count == 0);
}

static void test_stale_channel_events(void) {
    fixture();
    connect_open();
    uint16_t old = aacp_cid;
    bd_addr_t second = {7, 8, 9, 10, 11, 12};
    aacp_connect(second);
    fire(&aacp_connect_timer);
    uint16_t current = aacp_cid;
    opened(old, 0, peer);
    assert(aacp_cid == current && !aacp_connected && last_disconnected == old);
    opened(current, 0, second);
    event(L2CAP_EVENT_CHANNEL_CLOSED, old);
    event(L2CAP_EVENT_CAN_SEND_NOW, old);
    control(old);
    assert(aacp_cid == current && aacp_connected && !aacp_got_control && sends == 0);
    aacp_disconnect();
    opened(current, 0, second);
    assert(aacp_cid == 0 && !aacp_connected && !aacp_connect_timer.active);
}

static void test_bounded_connect_and_send_timeouts(void) {
    fixture();
    create_result = 1;
    aacp_connect(peer);
    for (unsigned i = 0; i < AACP_MAX_ATTEMPTS; ++i) fire(&aacp_connect_timer);
    assert(connects == AACP_MAX_ATTEMPTS && !aacp_connect_timer.active);
    fixture();
    connect_open();
    send_result = 1;
    event(L2CAP_EVENT_CAN_SEND_NOW, aacp_cid);
    assert(sends == 0);
    now = AACP_CHANNEL_TIMEOUT_MS;
    aacp_poll();
    assert(!aacp_connected && aacp_cid == 0 && aacp_connect_timer.active);
    aacp_disconnect();
    assert(!aacp_connect_timer.active);
    fixture();
    aacp_connect(peer);
    fire(&aacp_connect_timer);
    now = AACP_CHANNEL_TIMEOUT_MS;
    aacp_poll();
    assert(aacp_cid == 0 && disconnects == 1 && aacp_connect_timer.active);
}

static void test_audio_requires_complete_current_requested_sdu(void) {
    fixture();
    capture();
    uint8_t p[64] = {4, 0, 4, 0, 0x58, 0, 1, 0};
    p[26] = 2; p[27] = 0x11; p[28] = 0x22;
    assert(audio(p, aacp_cid, 29) == 1);
    unsigned valid_last = mic_stats.last_sdu_ms;
    for (unsigned len = 0; len < 29; ++len) {
        assert(audio(p, aacp_cid, len) == 1);
    }
    // A valid first AU followed by a truncated second AU rejects the whole SDU.
    p[33] = 3;
    assert(audio(p, aacp_cid, 35) == 1);
    p[1] = 1;
    assert(audio(p, aacp_cid, 29) == 1);
    p[1] = 0; p[3] = 1;
    assert(audio(p, aacp_cid, 29) == 1);
    p[3] = 0; p[26] = 0;
    assert(audio(p, aacp_cid, 27) == 1);
    p[26] = 2;
    now = 500;
    decode_ok = false;
    assert(audio(p, aacp_cid, 29) == 2);
    assert(mic_stats.last_sdu_ms == valid_last);
    decode_ok = true;
    assert(audio(p, aacp_cid + 1, 29) == 2);
    close_capture();
    assert(audio(p, aacp_cid, 29) == 2);
}

static void test_watchdog_is_bounded_and_cancelable(void) {
    fixture();
    capture();
    now = 2000;
    fire(&aacp_mic_stats_timer);
    aacp_poll();
    drain();
    quiet_after_stop();
    assert(starts == 2 && stops == 1);
    now += 2000;
    fire(&aacp_mic_stats_timer);
    aacp_poll();
    drain();
    quiet_after_stop();
    assert(starts == 2 && stops == 2 && aacp_mic_failed);
    aacp_poll();
    assert(aacp_tx_count == 0);
    close_capture();
    request_capture();
    drain();
    assert(starts == 3);
    now += 2000;
    fire(&aacp_mic_stats_timer);
    aacp_poll();
    close_capture();
    drain();
    quiet_after_stop();
    assert(starts == 3 && stops == 3 && !aacp_mic_stats_timer.active);
}

static void test_reconnect_never_invents_microphone_demand(void) {
    fixture();
    ready();
    event(L2CAP_EVENT_CHANNEL_CLOSED, aacp_cid);
    fire(&aacp_connect_timer);
    opened(aacp_cid, 0, peer);
    drain();
    control(aacp_cid);
    aacp_poll();
    assert(starts == 0);
    request_capture();
    drain();
    event(L2CAP_EVENT_CHANNEL_CLOSED, aacp_cid);
    close_capture();
    fire(&aacp_connect_timer);
    opened(aacp_cid, 0, peer);
    drain();
    control(aacp_cid);
    aacp_poll();
    assert(starts == 1 && !aacp_mic_want);
}

static void test_unwanted_uplink_retries_stop_then_closes(void) {
    fixture();
    ready();
    uint8_t p[29] = {4, 0, 4, 0, 0x58, 0, 1, 0};
    p[26] = 2;
    assert(audio(p, aacp_cid, sizeof(p)) == 0);
    drain();
    assert(stops == 1 && starts == 0 && aacp_mic_stopping);
    for (now = 100; now <= 2000; now += 100) {
        audio(p, aacp_cid, sizeof(p));
        aacp_poll();
    }
    drain();
    assert(stops == 2 && starts == 0 && aacp_mic_stopping);
    for (unsigned i = 0; i < 20; ++i) {
        now += 100;
        audio(p, aacp_cid, sizeof(p));
        aacp_poll();
    }
    assert(aacp_cid == 0 && !aacp_mic_active() && decoded == 0);
    assert(aacp_connect_timer.active && !aacp_mic_want);
}

static void test_gap_and_wrap_clear_predictor_history(void) {
    fixture();
    capture();
    uint8_t p[29] = {4, 0, 4, 0, 0x58, 0, 1, 0};
    p[26] = 2;
    unsigned before = resets;
    p[26] = 0;
    audio(p, aacp_cid, sizeof(p));
    assert(resets == before + 1 && decoded == 0);
    p[26] = 2;
    mic_stats.last_sdu_ms = UINT32_MAX - 999;
    now = 1000;
    audio(p, aacp_cid, sizeof(p));
    assert(resets == before + 2 && decoded == 1 && mic_stats.last_sdu_ms == now);
}

static void test_short_events_cannot_change_channel(void) {
    fixture();
    connect_open();
    uint16_t cid = aacp_cid;
    for (unsigned n = 0; n < 21; ++n) {
        uint8_t p[26] = {L2CAP_EVENT_CHANNEL_OPENED};
        p[1] = n >= 2 ? (uint8_t)(n - 2) : 0;
        aacp_packet_handler(HCI_EVENT_PACKET, 0, p, (uint16_t)n);
        assert(aacp_cid == cid && aacp_connected);
    }
    for (unsigned n = 0; n < 4; ++n) {
        uint8_t p[4] = {L2CAP_EVENT_CHANNEL_CLOSED};
        p[1] = n >= 2 ? (uint8_t)(n - 2) : 0;
        aacp_packet_handler(HCI_EVENT_PACKET, 0, p, (uint16_t)n);
        assert(aacp_cid == cid && aacp_connected);
    }
}

static void test_usb_irq_cancel_precedes_main_loop_notification(void) {
    fixture();
    ready();
    request_capture();
    assert(aacp_mic_start_queued());
    usb_capture_wanted = false; // USB IRQ closes capture; main has not polled yet.
    drain();
    assert(starts == 0 && !aacp_mic_want && !aacp_mic_active());

    fixture();
    capture();
    uint8_t p[29] = {4, 0, 4, 0, 0x58, 0, 1, 0};
    p[26] = 2;
    usb_capture_wanted = false;
    assert(audio(p, aacp_cid, sizeof(p)) == 0);
    drain();
    quiet_after_stop();
    assert(stops == 1 && !aacp_mic_active());

    fixture();
    capture();
    usb_capture_wanted = false;
    aacp_poll();
    drain();
    quiet_after_stop();
    assert(stops == 1 && !aacp_mic_active() && !aacp_mic_want);

    fixture();
    capture();
    usb_capture_wanted = false;
    now = 2000;
    fire(&aacp_mic_stats_timer);
    drain();
    quiet_after_stop();
    assert(starts == 1 && stops == 1 && !aacp_mic_active());
}

static void test_worker_reset_ack_and_timeout(void) {
    fixture();
    ready();
    decoder_reset_done = false;
    unsigned before = resets;
    request_capture();
    for (unsigned i = 0; i < 20; ++i) aacp_poll();
    assert(resets == before + 1 && starts == 0 && !aacp_mic_start_queued());
    decoder_reset_done = true;
    aacp_poll();
    drain();
    assert(starts == 1);

    fixture();
    ready();
    decoder_reset_done = false;
    request_capture();
    now = AACP_MIC_PREPARE_TIMEOUT_MS;
    aacp_poll();
    assert(aacp_mic_failed && starts == 0 && !aacp_mic_start_queued());
    decoder_reset_done = true;
    aacp_poll();
    assert(starts == 0);
    close_capture();
    request_capture();
    drain();
    assert(starts == 1);
}

static void test_queued_frames_do_not_feed_decode_watchdog(void) {
    fixture();
    capture();
    complete_on_submit = false;
    uint8_t p[29] = {4, 0, 4, 0, 0x58, 0, 1, 0};
    p[26] = 2;
    now = 1500;
    audio(p, aacp_cid, sizeof(p));
    assert(decoded == 1 && mic_stats.last_sdu_ms == 0);
    now = 2000;
    fire(&aacp_mic_stats_timer);
    assert(!aacp_mic_running); // arrivals cannot hide a stalled worker

    fixture();
    capture();
    now = 1500;
    completed++;
    aacp_poll();
    assert(mic_stats.last_sdu_ms == 1500);
    now = 2000;
    fire(&aacp_mic_stats_timer);
    assert(aacp_mic_running);
    close_capture();
    completed++;
    aacp_poll();
    assert(mic_stats.last_sdu_ms == 1500);
}

static void test_worker_fault_and_reset_after_start_queued(void) {
    fixture();
    ready();
    decoder_reset_done = false;
    request_capture();
    decoder_ready = false;
    aacp_poll();
    assert(aacp_mic_failed && !aacp_mic_preparing && starts == 0);

    fixture();
    ready();
    request_capture();
    assert(aacp_mic_start_queued());
    decoder_reset_done = false;
    event(L2CAP_EVENT_CAN_SEND_NOW, aacp_cid);
    assert(starts == 0 && aacp_mic_start_waiting_for_reset);
    aacp_poll();
    assert(starts == 0);
    decoder_reset_done = true;
    aacp_poll();
    assert(!aacp_mic_start_waiting_for_reset);
    drain();
    assert(starts == 1);

    fixture();
    ready();
    request_capture();
    decoder_ready = decoder_reset_done = false;
    drain();
    assert(starts == 0 && aacp_mic_failed && !aacp_mic_start_queued());
}

static void test_host_controls_preserve_mic_capacity_and_readback(void) {
    fixture();
    assert(!aacp_control_ready() && !aacp_control_anc(2));
    ready();
    assert(aacp_control_ready() && aacp_control_readback()->anc == 3);
    uint8_t report[] = {4,0,4,0,9,0,0x0d,3,0,0,0};
    audio(report, aacp_cid, sizeof(report));
    assert(aacp_control_readback()->anc == 3);
    assert(!aacp_control_anc(4));
    assert(aacp_control_anc(2));
    assert(!aacp_control_anc(3)); // bounded host queue leaves STOP room
    assert(aacp_control_readback()->anc == 3); // acceptance is not confirmation
    drain();
    assert(aacp_control_readback()->anc == 3);
    report[7] = 2; audio(report, aacp_cid, sizeof(report));
    assert(aacp_control_readback()->anc == 2);
    uint32_t session = aacp_control_session();
    aacp_disconnect();
    assert(aacp_control_session() != session && !aacp_control_readback()->anc);
    assert(!aacp_control_anc(3));
}

static void test_research_lifecycle_and_cache(void) {
    fixture();
    uint8_t hearing[] = {4,0,4,0,9,0,0x2c,1,1,0,0};
    uint8_t output[512];
    assert(!aacp_research_send(hearing, sizeof(hearing)));
    ready();
    bd_addr_t address;
    assert(aacp_control_address(address) && memcmp(address, peer, 6) == 0);
    assert(aacp_research_send(hearing, sizeof(hearing)));
    assert(!aacp_research_send(hearing, sizeof(hearing)));
    assert(aacp_research_take() == 0);
    aacp_research_cancel();
    assert(aacp_tx_count == 0);
    assert(aacp_research_send(hearing, sizeof(hearing)));
    drain();
    assert(aacp_research_take() == 1 && aacp_research_take() == 0);
    assert(aacp_research_cached(2, output, sizeof(output)) == 0);
    audio(hearing, aacp_cid, sizeof(hearing));
    now = 123;
    assert(aacp_research_cached(2, output, sizeof(output)) == 19);
    assert(dongle_u32(output) == 123 && dongle_u32(output + 4) == 1 &&
           memcmp(output + 8, hearing, sizeof(hearing)) == 0);
    audio(hearing, aacp_cid, sizeof(hearing));
    assert(aacp_research_cached(2, output, sizeof(output)) == 19);
    assert(dongle_u32(output) == 0 && dongle_u32(output + 4) == 2);
    uint8_t oversized[12] = {4,0,4,0,9,0,0x2c};
    audio(oversized, aacp_cid, sizeof(oversized));
    assert(aacp_research_cached(2, output, sizeof(output)) == 0);
    assert(reports[1].revision == 3);
    audio(hearing, aacp_cid, sizeof(hearing));
    assert(aacp_research_cached(2, output, sizeof(output)) == 19 && dongle_u32(output + 4) == 4);
    reports[1].revision = UINT32_MAX;
    audio(hearing, aacp_cid, sizeof(hearing));
    assert(aacp_research_cached(2, output, sizeof(output)) == 19 && dongle_u32(output + 4) == 1);
    assert(!aacp_research_send(aacp_mic_start_bytes, sizeof(aacp_mic_start_bytes)));
    remote_mtu = 10;
    assert(!aacp_research_send(hearing, sizeof(hearing)));
    remote_mtu = 512;
    for (unsigned i = 0; i < 6; ++i) assert(aacp_tx_enqueue(control_anc, sizeof(control_anc)));
    assert(!aacp_research_send(hearing, sizeof(hearing)));
    drain();
    assert(aacp_research_send(hearing, sizeof(hearing)));
    now += 3000;
    aacp_poll();
    assert(aacp_research_take() == 2 && aacp_tx_count == 0 && aacp_control_ready());
    assert(aacp_research_send(hearing, sizeof(hearing)));
    aacp_disconnect();
    assert(aacp_research_take() == 2);
    assert(aacp_research_cached(2, output, sizeof(output)) == 0);
    assert(reports[1].revision == 0);
}
static void test_research_allowed_packets(void) {
    fixture(); ready();
    uint8_t pme[140] = {4,0,4,0,0x53,0,0x84,0,2,1,1,2};
    assert(aacp_research_send(pme, sizeof(pme)));
    drain(); assert(aacp_research_take() == 1);
    pme[8] = 1;
    assert(!aacp_research_send(pme, sizeof(pme)));
    uint8_t clock[41] = {4,0,4,0,0x1b,0};
    clock[14] = 25;
    memcpy(clock + 16, "2026-10-22T15:34:02+00:00", 25);
    assert(aacp_research_send(clock, sizeof(clock)));
    drain(); assert(aacp_research_take() == 1);
    clock[36] = '1';
    assert(!aacp_research_send(clock, sizeof(clock)));
    assert(aacp_research_send(aacp_request_notifications, sizeof(aacp_request_notifications)));
    drain(); assert(aacp_research_take() == 1);
}

int main(void) {
    test_handshake_and_decoder_gate();
    test_usb_cancel_and_stop_are_not_lost();
    test_stale_channel_events();
    test_bounded_connect_and_send_timeouts();
    test_audio_requires_complete_current_requested_sdu();
    test_watchdog_is_bounded_and_cancelable();
    test_reconnect_never_invents_microphone_demand();
    test_unwanted_uplink_retries_stop_then_closes();
    test_gap_and_wrap_clear_predictor_history();
    test_short_events_cannot_change_channel();
    test_usb_irq_cancel_precedes_main_loop_notification();
    test_worker_reset_ack_and_timeout();
    test_queued_frames_do_not_feed_decode_watchdog();
    test_worker_fault_and_reset_after_start_queued();
    test_host_controls_preserve_mic_capacity_and_readback();
    test_research_lifecycle_and_cache();
    test_research_allowed_packets();
    puts("AACP lifecycle and packet validation: 17 groups passed");
    return 0;
}
