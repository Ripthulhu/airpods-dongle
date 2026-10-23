// SPDX-License-Identifier: GPL-3.0-only
#include "btstack_att.h"
#include "btstack_aacp.h"
#include "btstack_avdtp_source.h"
#include "phone_sink.h"
#include "../tinyusb/uac.h"
#include "../tinyusb/dongle_rpc.h"
#include "btstack.h"
#include <string.h>

#define ATT_PSM 0x001f
#define ATT_TIMEOUT_MS 3000u
static uint16_t att_cid, tx_length, rx_length;
static uint8_t tx[DONGLE_ATT_MAX], rx[DONGLE_ATT_MAX];
static bd_addr_t peer;
static uint32_t session, started;
static bool opened, pending, sent;
static dongle_att_result_t result;
static void packet_handler(uint8_t, uint16_t, uint8_t *, uint16_t);

static void close_channel(void) {
    uint16_t cid = att_cid;
    att_cid = 0;
    opened = pending = sent = false;
    tx_length = rx_length = 0;
    if (cid) l2cap_disconnect(cid);
}
static void fail(void) {
    bool affected = pending || result != DONGLE_ATT_WAIT;
    close_channel();
    result = affected ? DONGLE_ATT_FAILED : DONGLE_ATT_WAIT;
}
void dongle_att_cancel(void) { close_channel(); result = DONGLE_ATT_WAIT; }

static bool same_peer(void) {
    bd_addr_t address;
    return aacp_control_session() == session && aacp_control_address(address) &&
           memcmp(peer, address, sizeof(peer)) == 0;
}
static bool valid_request(const uint8_t *p, uint16_t n) {
    if (!p || !n || n > DONGLE_ATT_MAX) return false;
    switch (p[0]) {
        case 0x06: return n == 23 && little_endian_read_16(p, 1) != 0 &&
                          little_endian_read_16(p, 1) <= little_endian_read_16(p, 3);
        case 0x08: return (n == 7 || n == 21) && little_endian_read_16(p, 1) != 0 &&
                          little_endian_read_16(p, 1) <= little_endian_read_16(p, 3);
        case 0x0a: return n == 3 && little_endian_read_16(p, 1) != 0;
        case 0x12: case 0x52: return n >= 4 && little_endian_read_16(p, 1) != 0;
        default: return false;
    }
}
bool dongle_att_request(const uint8_t *p, uint16_t n) {
    dongle_att_poll();
    if (pending || result != DONGLE_ATT_WAIT || !valid_request(p, n)) return false;
    bd_addr_t address;
    if (!aacp_control_address(address)) return false;
    if (att_cid && (!same_peer() || memcmp(peer, address, sizeof(peer)))) close_channel();
    memcpy(peer, address, sizeof(peer));
    session = aacp_control_session();
    memcpy(tx, p, n);
    tx_length = n;
    rx_length = 0;
    pending = true;
    sent = false;
    started = btstack_run_loop_get_time_ms();
    if (!att_cid) {
        if (l2cap_create_channel(packet_handler, peer, ATT_PSM, DONGLE_ATT_MAX, &att_cid) != ERROR_CODE_SUCCESS) {
            fail();
            return true;
        }
    } else l2cap_request_can_send_now_event(att_cid);
    return true;
}
void dongle_att_poll(void) {
    if (att_cid && !same_peer()) { fail(); return; }
    if (pending && (uint32_t)(btstack_run_loop_get_time_ms() - started) >= ATT_TIMEOUT_MS) fail();
}
dongle_att_result_t dongle_att_take(uint8_t *out, uint16_t capacity, uint16_t *length) {
    dongle_att_poll();
    if (length) *length = 0;
    dongle_att_result_t value = result;
    if (value == DONGLE_ATT_DONE && rx_length) {
        if (!out || capacity < rx_length) {
            close_channel();
            result = DONGLE_ATT_WAIT;
            return DONGLE_ATT_FAILED;
        }
        memcpy(out, rx, rx_length);
        if (length) *length = rx_length;
    }
    if (value != DONGLE_ATT_WAIT) { result = DONGLE_ATT_WAIT; rx_length = 0; }
    return value;
}
static bool matches_response(const uint8_t *p, uint16_t n) {
    if (n == 5 && p[0] == 1 && p[1] == tx[0]) return true;
    switch (tx[0]) {
        case 0x06: return p[0] == 0x07;
        case 0x08: return p[0] == 0x09;
        case 0x0a: return p[0] == 0x0b;
        case 0x12: return n == 1 && p[0] == 0x13;
        default: return false;
    }
}
static bool guarded_write_ready(void) {
    // The host's confirmed self-check request is a one-byte ASCII '1' Write
    // Request. Recheck at the radio boundary, including after send retries.
    if (tx_length != 4 || tx[0] != 0x12 || tx[3] != '1') return true;
    if (get_usb_streaming() || check_is_streaming() || dongle_playback_transitioning() || usb_mic_requested() ||
        aacp_mic_active() || phone_sink_connected()) return false;
    uint8_t ears[40];
    static const uint8_t header[] = {4,0,4,0,6,0};
    uint16_t length = aacp_research_cached(5, ears, sizeof(ears));
    return length >= 16 && dongle_u32(ears) <= 30000u &&
           memcmp(ears + 8, header, sizeof(header)) == 0 && ears[14] == 2 && ears[15] == 2;
}
static void packet_handler(uint8_t type, uint16_t channel, uint8_t *p, uint16_t n) {
    if (type == L2CAP_DATA_PACKET) {
        if (!att_cid || !opened || channel != att_cid) return;
        if (!same_peer() || n == 0 || n > DONGLE_ATT_MAX) { fail(); return; }
        if (n >= 3 && p[0] == 0x1b && little_endian_read_16(p, 1)) return;
        if (!pending || !sent || !matches_response(p, n)) { fail(); return; }
        memcpy(rx, p, n);
        rx_length = n;
        pending = false;
        result = DONGLE_ATT_DONE;
        return;
    }
    if (type != HCI_EVENT_PACKET || n < 2 || (unsigned)p[1] + 2 != n) return;
    switch (hci_event_packet_get_type(p)) {
        case L2CAP_EVENT_CHANNEL_OPENED: {
            if (n < 21) return;
            uint16_t cid = l2cap_event_channel_opened_get_local_cid(p);
            uint8_t status = l2cap_event_channel_opened_get_status(p);
            bd_addr_t address;
            l2cap_event_channel_opened_get_address(p, address);
            if (!pending || !att_cid || cid != att_cid || !same_peer() || memcmp(address, peer, sizeof(peer))) {
                if (!status) l2cap_disconnect(cid);
                return;
            }
            if (status) { fail(); return; }
            opened = true;
            l2cap_request_can_send_now_event(att_cid);
            break;
        }
        case L2CAP_EVENT_CAN_SEND_NOW:
            if (n < 4 || !pending || !opened || sent ||
                l2cap_event_can_send_now_get_local_cid(p) != att_cid) return;
            if (!same_peer() || !dongle_rpc_transport_current() || usb_mic_requested() || aacp_mic_active() ||
                (uint32_t)(btstack_run_loop_get_time_ms()-started)>=ATT_TIMEOUT_MS ||
                tx_length > l2cap_get_remote_mtu_for_local_cid(att_cid) ||
                !guarded_write_ready()) { fail(); return; }
            if (l2cap_send(att_cid, tx, tx_length) != ERROR_CODE_SUCCESS) {
                l2cap_request_can_send_now_event(att_cid);
                return;
            }
            sent = true;
            if (tx[0] == 0x52) { pending = false; result = DONGLE_ATT_DONE; }
            break;
        case L2CAP_EVENT_CHANNEL_CLOSED:
            if (n >= 4 && att_cid && l2cap_event_channel_closed_get_local_cid(p) == att_cid) {
                att_cid = 0;
                fail();
            }
            break;
        default: break;
    }
}
