// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
// Derived from librepods (https://github.com/librepods-org/librepods), GPL-3.0.
//
// AACP (Apple Accessory Protocol) channel — the AirPods hi-res mic transport.
//
// Opens an outgoing L2CAP channel on PSM 0x1001 over the existing ACL link,
// runs the fixed init sequence (handshake / SET_FEATURE_FLAGS /
// REQUEST_NOTIFICATIONS), then drives the microphone stream: a START opcode
// makes the AirPods push type-0x58 SDUs carrying AAC-ELD access units, which
// are demuxed here and handed to the decoder (aacp_mic_dec.c).
//
// All byte sequences and the 0x58 demux layout are ports of librepods
// (linux-rust branch, GPL-3.0) — see that project's aacp_audio.rs.
//
// LOGGING RULE: these handlers run in the BTstack run loop, where a burst of
// blocking log output has caused A2DP underruns. No per-packet logging on the
// audio path; telemetry is aggregated and printed at most once per second.
//

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "btstack_aacp.h"
#include "aacp_mic_dec.h"
#include "../tinyusb/uac.h"

#define AACP_PSM              0x1001
// Requested local MTU. BTstack clamps this to l2cap_max_mtu() =
// HCI_ACL_PAYLOAD_SIZE - 4 (currently 1691), which is what actually gets
// advertised. Measured audio SDUs top out around 354 bytes, well under that;
// if larger SDUs ever get dropped, raise HCI_ACL_PAYLOAD_SIZE in
// btstack_config.h rather than this constant.
#define AACP_LOCAL_MTU        4096
#define AACP_CONNECT_DELAY_MS 1500   // let A2DP settle before opening the channel
#define AACP_RETRY_DELAY_MS   3000   // delay between connect retries
#define AACP_MAX_ATTEMPTS     3      // initial attempt + retries per A2DP session

// Mic lifecycle: the ONLY trigger is the USB alt setting — main.c calls
// aacp_mic_start()/aacp_mic_stop() when the host opens/closes the recording
// stream. If the host opens the mic before the AACP channel is up (or the
// channel drops mid-capture), the start is remembered and fired as soon as
// the first control packet confirms the channel.

// --- Fixed init byte sequences (verbatim from librepods) ---

// 1) Handshake: 16 bytes sent raw (the 04 00 04 00 data header is part of it).
static const uint8_t aacp_handshake[] = {
    0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 2) SET_FEATURE_FLAGS (opcode 0x004D): 04 00 04 00 | 4D 00 | FF 00 00 00 00 00 00 00
static const uint8_t aacp_set_feature_flags[] = {
    0x04, 0x00, 0x04, 0x00, 0x4D, 0x00,
    0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 3) REQUEST_NOTIFICATIONS (opcode 0x000F): 04 00 04 00 | 0F 00 | FF FF FF FF
static const uint8_t aacp_request_notifications[] = {
    0x04, 0x00, 0x04, 0x00, 0x0F, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF
};

// --- Mic stream control byte sequences (verbatim from librepods) ---

static const uint8_t aacp_mic_start_bytes[] = {
    0x04, 0x00, 0x04, 0x00, 0x58, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x01, 0x82, 0x00, 0x00, 0x00,
    0x04, 0x96, 0x00
};

static const uint8_t aacp_mic_stop_bytes[] = {
    0x04, 0x00, 0x04, 0x00, 0x58, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x03, 0x01
};

// TX entries reference static command bytes; there are no per-packet allocations.
typedef struct {
    const uint8_t *data;
    uint16_t len;
} aacp_pkt_t;
#define AACP_TX_QUEUE_LEN 8
#define AACP_CHANNEL_TIMEOUT_MS 5000
#define AACP_MIC_RESTARTS 1
#define AACP_MIC_QUIET_MS 500
#define AACP_MIC_DRAIN_MS 2000
static aacp_pkt_t aacp_tx_queue[AACP_TX_QUEUE_LEN];
static uint8_t aacp_tx_head, aacp_tx_count;
static uint16_t aacp_cid;
static bool aacp_session, aacp_connected, aacp_got_control, aacp_init_sent;
static bd_addr_t aacp_addr;
static uint8_t aacp_attempts;
static uint32_t aacp_channel_since_ms, aacp_tx_since_ms;
static btstack_timer_source_t aacp_connect_timer;

#define TYPE58_HEADER_LEN 22
static bool aacp_mic_running, aacp_mic_want, aacp_mic_failed, aacp_mic_remote_active;
static bool aacp_mic_stopping;
static uint8_t aacp_mic_restarts;
static uint8_t aacp_mic_stop_retries;
static uint32_t aacp_mic_stop_since_ms, aacp_last_uplink_ms;
static btstack_timer_source_t aacp_mic_stats_timer;
static struct {
    uint32_t sdu_total, au_total, sdu_interval, au_interval, au_bytes_interval;
    uint16_t au_min, au_max, sdu_max, demux_short;
    uint32_t last_sdu_ms;
} mic_stats;

static void aacp_packet_handler(uint8_t, uint16_t, uint8_t *, uint16_t);
static void aacp_schedule_retry(void);
static void aacp_try_mic_start(void);

static void aacp_mic_teardown(void) {
    aacp_mic_running = false;
    btstack_run_loop_remove_timer(&aacp_mic_stats_timer);
    aacp_mic_pcm_reset();
}

// Clear ownership before disconnect: a late event must not revive this channel.
void aacp_disconnect(void) {
    uint16_t cid = aacp_cid;
    aacp_session = false;
    aacp_cid = 0;
    aacp_connected = aacp_got_control = aacp_init_sent = false;
    aacp_mic_remote_active = false;
    aacp_mic_stopping = false;
    aacp_tx_head = aacp_tx_count = 0;
    btstack_run_loop_remove_timer(&aacp_connect_timer);
    aacp_mic_teardown();
    if (cid != 0) l2cap_disconnect(cid);
}

void aacp_init(void) {
    aacp_disconnect();
    aacp_mic_want = aacp_mic_failed = false;
    aacp_mic_restarts = 0;
    aacp_attempts = 0;
}

static void aacp_do_connect(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (!aacp_session || aacp_cid != 0) return;
    aacp_attempts++;
    aacp_channel_since_ms = btstack_run_loop_get_time_ms();
    uint8_t status = l2cap_create_channel(aacp_packet_handler, aacp_addr, AACP_PSM,
                                         AACP_LOCAL_MTU, &aacp_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[AACP] channel request failed: 0x%02x\n", status);
        aacp_cid = 0;
        aacp_schedule_retry();
    }
}

static void aacp_schedule_retry(void) {
    if (!aacp_session || aacp_attempts >= AACP_MAX_ATTEMPTS) return;
    btstack_run_loop_remove_timer(&aacp_connect_timer);
    btstack_run_loop_set_timer_handler(&aacp_connect_timer, aacp_do_connect);
    btstack_run_loop_set_timer(&aacp_connect_timer, AACP_RETRY_DELAY_MS);
    btstack_run_loop_add_timer(&aacp_connect_timer);
}

void aacp_connect(bd_addr_t addr) {
    if (aacp_session && memcmp(aacp_addr, addr, sizeof(bd_addr_t)) == 0) return;
    aacp_disconnect();
    memcpy(aacp_addr, addr, sizeof(bd_addr_t));
    aacp_session = true;
    aacp_attempts = 0;
    btstack_run_loop_set_timer_handler(&aacp_connect_timer, aacp_do_connect);
    btstack_run_loop_set_timer(&aacp_connect_timer, AACP_CONNECT_DELAY_MS);
    btstack_run_loop_add_timer(&aacp_connect_timer);
}

bool aacp_is_connected(void) { return aacp_connected; }
bool aacp_mic_active(void) { return aacp_mic_remote_active; }

static bool aacp_tx_enqueue(const uint8_t *data, uint16_t len) {
    if (aacp_cid == 0 || !aacp_connected || aacp_tx_count == AACP_TX_QUEUE_LEN)
        return false;
    uint8_t slot = (uint8_t)((aacp_tx_head + aacp_tx_count) % AACP_TX_QUEUE_LEN);
    aacp_tx_queue[slot] = (aacp_pkt_t){ data, len };
    if (aacp_tx_count++ == 0) aacp_tx_since_ms = btstack_run_loop_get_time_ms();
    l2cap_request_can_send_now_event(aacp_cid);
    return true;
}

static bool aacp_mic_start_queued(void) {
    for (unsigned i = 0; i < aacp_tx_count; ++i) {
        if (aacp_tx_queue[(aacp_tx_head + i) % AACP_TX_QUEUE_LEN].data == aacp_mic_start_bytes)
            return true;
    }
    return false;
}

// USB close cancels queued STARTs too, including watchdog restarts.
static void aacp_drop_mic_commands(void) {
    uint8_t kept = 0;
    for (unsigned i = 0; i < aacp_tx_count; ++i) {
        aacp_pkt_t p = aacp_tx_queue[(aacp_tx_head + i) % AACP_TX_QUEUE_LEN];
        if (p.data == aacp_mic_start_bytes || p.data == aacp_mic_stop_bytes) continue;
        aacp_tx_queue[(aacp_tx_head + kept++) % AACP_TX_QUEUE_LEN] = p;
    }
    aacp_tx_count = kept;
}

static void aacp_channel_failed(void) {
    uint16_t cid = aacp_cid;
    aacp_cid = 0;
    aacp_connected = aacp_got_control = aacp_init_sent = false;
    aacp_mic_remote_active = false;
    aacp_mic_stopping = false;
    aacp_tx_head = aacp_tx_count = 0;
    aacp_mic_teardown();
    if (cid != 0) l2cap_disconnect(cid);
    aacp_schedule_retry();
}

static void aacp_mic_stats_report(btstack_timer_source_t *ts) {
    if (aacp_mic_want && !usb_mic_requested()) {
        aacp_mic_stop();
        return;
    }
    if (!aacp_mic_want || !aacp_mic_running || !aacp_connected) return;
    uint32_t now = btstack_run_loop_get_time_ms();
    printf("[MIC] sdu/s=%lu au/s=%lu malformed=%u\n",
           (unsigned long)mic_stats.sdu_interval,
           (unsigned long)mic_stats.au_interval, mic_stats.demux_short);
    aacp_mic_dec_report();
    mic_stats.sdu_interval = mic_stats.au_interval = mic_stats.au_bytes_interval = 0;
    if ((uint32_t)(now - mic_stats.last_sdu_ms) >= 2000) {
        aacp_drop_mic_commands();
        bool restart = aacp_mic_restarts++ < AACP_MIC_RESTARTS;
        aacp_mic_teardown();
        if (!aacp_tx_enqueue(aacp_mic_stop_bytes, sizeof(aacp_mic_stop_bytes))) {
            aacp_channel_failed();
            return;
        }
        if (!restart) {
            aacp_mic_failed = true;
            printf("[MIC] no valid audio after retry; close capture before retrying\n");
        }
        // The poll queues a new START behind STOP while USB still owns capture.
        return;
    }
    btstack_run_loop_set_timer(ts, 1000);
    btstack_run_loop_add_timer(ts);
}

static void aacp_try_mic_start(void) {
    if (!usb_mic_requested() || !aacp_mic_want || aacp_mic_failed || aacp_mic_running ||
        aacp_mic_remote_active || aacp_mic_stopping ||
        !aacp_connected || !aacp_got_control || !aacp_init_sent ||
        !aacp_mic_dec_ready() || aacp_mic_start_queued()) return;
    if (!aacp_mic_dec_reset()) {
        aacp_mic_failed = true;
        return;
    }
    aacp_mic_dec_reset_stats();
    if (aacp_tx_enqueue(aacp_mic_start_bytes, sizeof(aacp_mic_start_bytes))) {
        memset(&mic_stats, 0, sizeof(mic_stats));
        mic_stats.au_min = UINT16_MAX;
    }
}

void aacp_mic_start(void) {
    if (!aacp_mic_want) {
        aacp_mic_failed = false;
        aacp_mic_restarts = 0;
    }
    aacp_mic_want = true;
    aacp_try_mic_start();
}

void aacp_mic_stop(void) {
    aacp_mic_want = false;
    aacp_mic_failed = false;
    aacp_mic_restarts = 0;
    aacp_drop_mic_commands();
    bool sent = aacp_mic_remote_active;
    aacp_mic_teardown();
    if (sent && !aacp_tx_enqueue(aacp_mic_stop_bytes, sizeof(aacp_mic_stop_bytes)))
        aacp_channel_failed();
}

static void aacp_handle_can_send_now(void) {
    if (!aacp_connected || aacp_tx_count == 0) return;
    aacp_pkt_t p = aacp_tx_queue[aacp_tx_head];
    if (p.data == aacp_mic_start_bytes && !usb_mic_requested()) {
        aacp_mic_stop();
        return;
    }
    if (p.data == aacp_mic_start_bytes && (!aacp_mic_want || aacp_mic_failed)) {
        aacp_drop_mic_commands();
        return;
    }
    if (l2cap_send(aacp_cid, (uint8_t *)p.data, p.len) != ERROR_CODE_SUCCESS) {
        l2cap_request_can_send_now_event(aacp_cid);
        return;
    }
    aacp_tx_head = (uint8_t)((aacp_tx_head + 1) % AACP_TX_QUEUE_LEN);
    aacp_tx_count--;
    aacp_tx_since_ms = btstack_run_loop_get_time_ms();
    if (p.data == aacp_request_notifications) aacp_init_sent = true;
    if (p.data == aacp_mic_start_bytes) {
        aacp_mic_running = true;
        aacp_mic_remote_active = true;
        mic_stats.last_sdu_ms = btstack_run_loop_get_time_ms();
        btstack_run_loop_remove_timer(&aacp_mic_stats_timer);
        btstack_run_loop_set_timer_handler(&aacp_mic_stats_timer, aacp_mic_stats_report);
        btstack_run_loop_set_timer(&aacp_mic_stats_timer, 1000);
        btstack_run_loop_add_timer(&aacp_mic_stats_timer);
    }
    if (p.data == aacp_mic_stop_bytes) {
        if (!aacp_mic_stopping) aacp_mic_stop_retries = 0;
        aacp_mic_stopping = true;
        aacp_mic_stop_since_ms = aacp_last_uplink_ms = btstack_run_loop_get_time_ms();
    }
    if (aacp_tx_count != 0) l2cap_request_can_send_now_event(aacp_cid);
    aacp_try_mic_start();
}

void aacp_poll(void) {
    // USB IRQs can revoke capture while the main loop still holds cached intent.
    if (aacp_mic_want && !usb_mic_requested()) aacp_mic_stop();
    uint32_t now = btstack_run_loop_get_time_ms();
    if (aacp_cid != 0 &&
        ((!aacp_connected || !aacp_got_control || !aacp_init_sent) &&
         (uint32_t)(now - aacp_channel_since_ms) >= AACP_CHANNEL_TIMEOUT_MS)) {
        aacp_channel_failed();
        return;
    }
    if (aacp_tx_count != 0 &&
        (uint32_t)(now - aacp_tx_since_ms) >= AACP_CHANNEL_TIMEOUT_MS) {
        aacp_channel_failed();
        return;
    }
    // A successful write is not proof the uplink stopped; drain before restarting.
    if (aacp_mic_stopping) {
        if ((uint32_t)(now - aacp_last_uplink_ms) >= AACP_MIC_QUIET_MS) {
            aacp_mic_stopping = aacp_mic_remote_active = false;
        } else if ((uint32_t)(now - aacp_mic_stop_since_ms) >= AACP_MIC_DRAIN_MS) {
            if (aacp_mic_stop_retries++ == 0) {
                aacp_mic_stop_since_ms = now;
                if (!aacp_tx_enqueue(aacp_mic_stop_bytes, sizeof(aacp_mic_stop_bytes)))
                    aacp_channel_failed();
            } else {
                aacp_channel_failed();
            }
            return;
        }
    }
    aacp_try_mic_start();
}

static bool aacp_has_header(const uint8_t *sdu, uint16_t size) {
    return size >= 6 && sdu[0] == 4 && sdu[1] == 0 && sdu[2] == 4 && sdu[3] == 0;
}

// Validate the entire SDU before decoding any AU, as in NTPods aap::for_each_au.
static bool aacp_valid_audio_sdu(const uint8_t *sdu, uint16_t size) {
    if (!aacp_has_header(sdu, size) || size <= TYPE58_HEADER_LEN ||
        little_endian_read_16(sdu, 4) != 0x58 ||
        little_endian_read_16(sdu, 6) != 1) return false;
    unsigned off = TYPE58_HEADER_LEN;
    while (off < size) {
        if ((unsigned)size - off < 5) return false;
        unsigned len = sdu[off + 4];
        if (len == 0 || len > (unsigned)size - off - 5) return false;
        off += 5 + len;
    }
    return true;
}

static void aacp_handle_audio_sdu(const uint8_t *sdu, uint16_t size) {
    if (!aacp_valid_audio_sdu(sdu, size)) {
        mic_stats.demux_short++;
        if (aacp_mic_want && aacp_mic_running) aacp_mic_dec_reset();
        return;
    }
    uint32_t now = btstack_run_loop_get_time_ms();
    // Late or unsolicited uplink packets must not reach a closed USB recording.
    if (!usb_mic_requested() && aacp_mic_want) aacp_mic_stop();
    if (!usb_mic_requested() || !aacp_mic_want || !aacp_mic_running) {
        aacp_last_uplink_ms = now;
        if (!aacp_mic_want && !aacp_mic_remote_active && !aacp_mic_stopping) {
            aacp_mic_remote_active = true;
            if (!aacp_tx_enqueue(aacp_mic_stop_bytes, sizeof(aacp_mic_stop_bytes)))
                aacp_channel_failed();
        }
        return;
    }
    if ((uint32_t)(now - mic_stats.last_sdu_ms) >= 2000 && !aacp_mic_dec_reset()) return;
    mic_stats.sdu_total++;
    mic_stats.sdu_interval++;
    if (size > mic_stats.sdu_max) mic_stats.sdu_max = size;
    bool decoded = false;
    for (unsigned off = TYPE58_HEADER_LEN; off < size;) {
        if (!usb_mic_requested()) {
            aacp_mic_stop();
            return;
        }
        uint8_t len = sdu[off + 4];
        decoded |= aacp_mic_dec_decode(sdu + off + 5, len);
        mic_stats.au_total++;
        mic_stats.au_interval++;
        mic_stats.au_bytes_interval += len;
        if (len < mic_stats.au_min) mic_stats.au_min = len;
        if (len > mic_stats.au_max) mic_stats.au_max = len;
        off += 5 + len;
    }
    if (decoded) mic_stats.last_sdu_ms = now;
}

static void aacp_packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (type == L2CAP_DATA_PACKET) {
        if (!aacp_session || !aacp_connected || channel != aacp_cid ||
            !aacp_has_header(packet, size)) return;
        // All microphone messages, including malformed ones, stay out of control readiness.
        if (little_endian_read_16(packet, 4) == 0x58) {
            aacp_handle_audio_sdu(packet, size);
        } else {
            aacp_got_control = true;
            aacp_try_mic_start();
        }
        return;
    }
    if (type != HCI_EVENT_PACKET || size < 2 || (unsigned)packet[1] + 2 != size) return;
    switch (hci_event_packet_get_type(packet)) {
        case L2CAP_EVENT_CHANNEL_OPENED: {
            if (size < 21) return;
            uint8_t status = l2cap_event_channel_opened_get_status(packet);
            uint16_t cid = l2cap_event_channel_opened_get_local_cid(packet);
            bd_addr_t addr;
            l2cap_event_channel_opened_get_address(packet, addr);
            if (!aacp_session || aacp_cid == 0 || cid != aacp_cid ||
                memcmp(addr, aacp_addr, sizeof(addr)) != 0) {
                if (status == ERROR_CODE_SUCCESS) l2cap_disconnect(cid);
                return;
            }
            if (status != ERROR_CODE_SUCCESS) {
                aacp_channel_failed();
                return;
            }
            aacp_connected = true;
            aacp_got_control = aacp_init_sent = false;
            aacp_channel_since_ms = btstack_run_loop_get_time_ms();
            aacp_tx_enqueue(aacp_handshake, sizeof(aacp_handshake));
            aacp_tx_enqueue(aacp_set_feature_flags, sizeof(aacp_set_feature_flags));
            aacp_tx_enqueue(aacp_request_notifications, sizeof(aacp_request_notifications));
            break;
        }
        case L2CAP_EVENT_CAN_SEND_NOW:
            if (size >= 4 && aacp_session && aacp_connected &&
                l2cap_event_can_send_now_get_local_cid(packet) == aacp_cid)
                aacp_handle_can_send_now();
            break;
        case L2CAP_EVENT_CHANNEL_CLOSED:
            if (size >= 4 && aacp_session &&
                l2cap_event_channel_closed_get_local_cid(packet) == aacp_cid)
                aacp_channel_failed();
            break;
        default:
            break;
    }
}
