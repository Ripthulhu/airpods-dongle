// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include "../src/btstack/volume_sync.h"
#include "../src/btstack/volume_scale.h"

static const uint8_t pair[6] = {1,2,3,4,5,6}, other[6] = {2,3,4,5,6,7};

static volume_sync_t connected(void) {
    volume_sync_t state = {0};
    assert(volume_sync_select(&state, pair));
    volume_sync_connection(&state, 7, 0);
    return state;
}

static void send(volume_sync_t *s, uint32_t now, uint8_t expected, uint8_t label) {
    uint8_t level;
    assert(volume_sync_due(s, now, &level) && level == expected);
    volume_sync_send(s, now, true, label);
    assert(!volume_sync_get(s, &level));
}

static void pending_before_setup(void) {
    volume_sync_t state = {0};
    uint8_t level = 255;
    assert(!volume_sync_get(&state, &level) && level == 255);
    assert(!volume_sync_request(&state, 50, false, 0));
    assert(volume_sync_select(&state, pair));
    assert(volume_sync_request(&state, 50, false, 0));
    assert(!volume_sync_due(&state, 20000, &level));
    assert(!volume_sync_select(&state, pair) && state.pending);
    volume_sync_connection(&state, 7, 20000);
    send(&state, 20000, 50, 1);
    assert(volume_sync_response(&state, 7, 1, true, 49));
    assert(volume_sync_get(&state, &level) && level == 49);
}

static void busy_and_latest_clamped_ack(void) {
    volume_sync_t state = connected();
    uint8_t level;
    assert(volume_sync_notification(&state, 7, 48));
    assert(volume_sync_get(&state, &level) && level == 48);
    assert(volume_sync_request(&state, 80, false, 0));
    assert(volume_sync_due(&state, 0, &level) && level == 80);
    volume_sync_send(&state, 0, false, 0);
    assert(!volume_sync_due(&state, 49, &level));
    assert(!volume_sync_get(&state, &level));
    assert(state.confirmed == 48); // A busy API return never confirms the request.
    assert(volume_sync_request(&state, 96, false, 50));
    send(&state, 50, 96, 1);
    assert(volume_sync_request(&state, 100, false, 51));
    assert(!volume_sync_response(&state, 8, 1, true, 96));
    assert(!volume_sync_response(&state, 7, 15, true, 96));
    assert(!volume_sync_response(&state, 7, 1, true, 128));
    assert(volume_sync_notification(&state, 7, 48)); // Late initial/interim readback.
    assert(state.desired == 100 && !volume_sync_get(&state, &level));
    assert(volume_sync_response(&state, 7, 1, true, 95));
    assert(state.pending && state.desired == 100 && !volume_sync_get(&state, &level));
    send(&state, 100, 100, 2);
    assert(!volume_sync_response(&state, 7, 1, true, 95)); // Old ACK cannot release the new SET.
    assert(state.inflight && state.desired == 100);
    assert(volume_sync_response(&state, 7, 2, true, 99));
    assert(volume_sync_get(&state, &level) && level == 99);
    assert(!volume_sync_due(&state, 2000, &level)); // Quantization is not a corrective loop.
}

static void notifications_and_same_level(void) {
    volume_sync_t state = connected();
    uint8_t level;
    assert(!volume_sync_notification(&state, 0, 50));
    assert(!volume_sync_notification(&state, 8, 50));
    assert(!volume_sync_notification(&state, 7, 128));
    assert(!volume_sync_get(&state, &level));
    assert(volume_sync_notification(&state, 7, 0));
    assert(volume_sync_get(&state, &level) && level == 0);
    assert(volume_sync_request(&state, 0, false, 10));
    assert(!state.pending && !volume_sync_due(&state, 10, &level));
    assert(volume_sync_get(&state, &level) && level == 0);
    assert(volume_sync_request(&state, 40, false, 20));
    assert(volume_sync_notification(&state, 7, 40));
    assert(!state.pending && volume_sync_get(&state, &level) && level == 40);
    assert(!volume_sync_request(&state, 128, false, 30));
    assert(volume_sync_get(&state, &level) && level == 40);
    state.request_revision = UINT32_MAX;
    assert(volume_sync_request(&state, 40, false, 31) && state.request_revision == 1);
    assert(!state.pending); // Even a same-level explicit intent has a fresh ordering revision.
}

static void relative_bursts_use_latest_intent(void) {
    volume_sync_t state = connected();
    uint8_t level;
    volume_sync_step(&state, true, 0); // An unknown peer must not imply volume zero.
    assert(!state.pending);
    assert(volume_sync_notification(&state, 7, 100));
    volume_sync_step(&state, true, 1);
    send(&state, 1, 110, 1);
    volume_sync_step(&state, true, 2);
    volume_sync_step(&state, true, 3);
    volume_sync_step(&state, true, 4);
    assert(state.desired == 127 && !state.phone_request);
    assert(volume_sync_response(&state, 7, 1, true, 110));
    assert(!volume_sync_get(&state, &level));
    send(&state, 51, 127, 2);
    for (unsigned n = 0; n < 14; ++n) volume_sync_step(&state, false, 52 + n);
    assert(state.desired == 0);
    assert(volume_sync_response(&state, 7, 2, true, 127));
    send(&state, 101, 0, 3);
    assert(volume_sync_response(&state, 7, 3, true, 0));
    assert(volume_sync_get(&state, &level) && level == 0);
}

static void peer_and_connection_fencing(void) {
    volume_sync_t state = connected();
    uint8_t level;
    assert(volume_sync_notification(&state, 7, 80));
    volume_sync_connection(&state, 7, 100); // The same CID can denote a new connection.
    assert(!volume_sync_get(&state, &level));
    assert(!volume_sync_response(&state, 7, 1, true, 80));
    assert(volume_sync_request(&state, 70, false, 110));
    send(&state, 110, 70, 2);
    volume_sync_connection(&state, 0, 120);
    assert(state.pending && !state.inflight && !volume_sync_get(&state, &level));
    assert(!volume_sync_response(&state, 7, 2, true, 70));
    volume_sync_connection(&state, 8, 130);
    assert(!volume_sync_response(&state, 7, 2, true, 70));
    send(&state, 130, 70, 1);
    assert(volume_sync_select(&state, other));
    assert(!state.pending && !state.inflight && !state.cid && !volume_sync_get(&state, &level));
    assert(!volume_sync_response(&state, 8, 1, true, 70));
    assert(!volume_sync_notification(&state, 8, 70));
    assert(volume_sync_select(&state, NULL));
    const uint8_t erased[6] = {255,255,255,255,255,255};
    assert(!volume_sync_select(&state, erased));
    assert(!volume_sync_request(&state, 80, false, 140));
}

static void timeout_is_bounded_and_unknown(void) {
    volume_sync_t state = connected();
    uint8_t level;
    uint32_t start = UINT32_MAX - 20u;
    assert(volume_sync_notification(&state, 7, 40));
    assert(volume_sync_request(&state, 70, false, start));
    for (unsigned attempt = 0; attempt < BT_VOLUME_ATTEMPTS; ++attempt) {
        uint32_t sent = start + attempt * (BT_VOLUME_ACK_MS + BT_VOLUME_RETRY_MS);
        send(&state, sent, 70, (uint8_t)(attempt + 1));
        if (attempt) assert(!volume_sync_response(&state, 7, (uint8_t)attempt, true, 40));
        assert(!volume_sync_due(&state, sent + BT_VOLUME_ACK_MS - 1u, &level));
        assert(!volume_sync_due(&state, sent + BT_VOLUME_ACK_MS, &level));
        assert(!state.known && !state.inflight);
        assert(!volume_sync_get(&state, &level));
        assert(!volume_sync_due(&state, sent + BT_VOLUME_ACK_MS + BT_VOLUME_RETRY_MS - 1u, &level));
    }
    assert(!state.pending);
    for (unsigned n = 0; n < 10000; ++n)
        assert(!volume_sync_due(&state, start + 4000u + n, &level));
    assert(!volume_sync_response(&state, 7, 3, true, 70));
    assert(volume_sync_notification(&state, 7, 65));
    assert(volume_sync_get(&state, &level) && level == 65);
}

static void busy_deadline_and_rejection(void) {
    volume_sync_t state = connected();
    uint8_t level;
    assert(volume_sync_notification(&state, 7, 40));
    assert(volume_sync_request(&state, 70, false, 0));
    unsigned attempts = 0;
    for (unsigned now = 0; now <= BT_VOLUME_REQUEST_MS; ++now) {
        if (volume_sync_due(&state, now, &level)) {
            volume_sync_send(&state, now, false, 0);
            ++attempts;
        }
    }
    assert(attempts == BT_VOLUME_REQUEST_MS / BT_VOLUME_RETRY_MS && !state.pending);
    assert(volume_sync_get(&state, &level) && level == 40); // No write was accepted.
    assert(volume_sync_request(&state, 70, false, 6000));
    send(&state, 6000, 70, 1);
    assert(volume_sync_response(&state, 7, 1, false, 255));
    assert(!state.pending && !volume_sync_due(&state, 6100, &level));
    assert(volume_sync_get(&state, &level) && level == 40);
}

static void ownership_only_cancels_phone_intent(void) {
    volume_sync_t state = connected();
    uint8_t level;
    assert(volume_sync_notification(&state, 7, 40));
    assert(volume_sync_request(&state, 70, true, 0));
    volume_sync_cancel_phone(&state);
    assert(!state.pending && volume_sync_get(&state, &level) && level == 40);
    assert(volume_sync_request(&state, 70, false, 0));
    volume_sync_cancel_phone(&state);
    assert(state.pending);
    assert(volume_sync_request(&state, 80, true, 0));
    send(&state, 0, 80, 1);
    volume_sync_cancel_phone(&state);
    assert(!state.pending && state.inflight && !volume_sync_get(&state, &level));
    assert(volume_sync_response(&state, 7, 1, true, 78));
    assert(volume_sync_get(&state, &level) && level == 78);
    assert(!volume_sync_due(&state, 2000, &level));
    assert(volume_sync_request(&state, 90, true, 2000));
    volume_sync_connection(&state, 7, 2010);
    assert(!state.pending && !volume_sync_get(&state, &level));
}

int main(void) {
    pending_before_setup();
    busy_and_latest_clamped_ack();
    notifications_and_same_level();
    relative_bursts_use_latest_intent();
    peer_and_connection_fencing();
    timeout_is_bounded_and_unknown();
    busy_deadline_and_rejection();
    ownership_only_cancels_phone_intent();
    for (unsigned level = 0; level <= 127; ++level)
        assert(usb_volume_to_bt(bt_volume_to_usb((uint8_t)level)) == level);
    assert(usb_volume_to_bt(INT16_MIN) == 0 && usb_volume_to_bt(INT16_MAX) == 127);
    puts("volume sync: busy/latest requests, confirmed/clamped ACKs, peer/CID/label fencing, bounded timeouts and ownership passed");
}
