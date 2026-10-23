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
#include "dongle_settings.h"
#include "../tinyusb/dongle_rpc.h"
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
static const uint8_t aacp_request_keys[] = {4,0,4,0,0x30,0,5,0};

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
#define AACP_MIC_PREPARE_TIMEOUT_MS 1000
static aacp_pkt_t aacp_tx_queue[AACP_TX_QUEUE_LEN];
static uint8_t aacp_tx_head, aacp_tx_count;
static uint8_t research_tx[512];
static uint32_t research_since;
static bool research_pending;
static int research_result;
static uint8_t model_report[384], hearing_report[11], assist_report[11], pme_report[140], ear_report[32];
typedef struct { uint8_t *packet; uint16_t capacity, length; uint32_t updated, revision; } cached_report_t;
static cached_report_t reports[] = {
    {model_report, sizeof(model_report), 0, 0, 0}, {hearing_report, sizeof(hearing_report), 0, 0, 0},
    {assist_report, sizeof(assist_report), 0, 0, 0}, {pme_report, sizeof(pme_report), 0, 0, 0},
    {ear_report, sizeof(ear_report), 0, 0, 0}
};
static dongle_readback_t control_readback;
static dongle_settings_t settings;
static uint8_t setting_tx[11];
static uint32_t setting_since;
static bool setting_pending;
static int setting_result;
static uint8_t receive_buffer_tx[11];
static uint32_t receive_buffer_serial;
static uint16_t receive_buffer_last;
static uint8_t pair_irk[16], pair_key_address[6];
static bool pair_irk_known;
static uint32_t control_session;
// Static entries stay valid until their queued transmission completes.
static const uint8_t control_anc[] = {4,0,4,0,9,0,0x0d,2,0,0,0};
static const uint8_t control_transparency[] = {4,0,4,0,9,0,0x0d,3,0,0,0};
static void control_reset(void) {
    ++control_session;
    dongle_readback_clear(&control_readback);
    settings_reset(&settings);
    for (unsigned i = 0; i < sizeof(reports) / sizeof(reports[0]); ++i) {
        reports[i].length = 0;
        reports[i].revision = 0;
    }
    if (research_pending || research_result) {
        aacp_research_cancel();
        research_result = 2;
    }
    if (setting_pending || setting_result) {
        aacp_setting_cancel();
        setting_result = 2;
    }
}
static uint16_t aacp_cid;
static bool aacp_session, aacp_connected, aacp_got_control, aacp_init_sent;
static bd_addr_t aacp_addr;
static uint8_t aacp_attempts;
static uint32_t aacp_channel_since_ms, aacp_tx_since_ms;
static btstack_timer_source_t aacp_connect_timer;

#define TYPE58_HEADER_LEN 22
static bool aacp_mic_running, aacp_mic_want, aacp_mic_failed, aacp_mic_remote_active;
static bool aacp_mic_stopping;
static bool aacp_mic_preparing;
static bool aacp_mic_start_waiting_for_reset;
static bool aacp_mic_gap_reset;
static uint32_t aacp_mic_prepare_since_ms;
static uint32_t aacp_mic_completed_seen;
static uint8_t aacp_mic_restarts;
static uint8_t aacp_mic_stop_retries;
static uint32_t aacp_mic_stop_since_ms, aacp_last_uplink_ms;
static btstack_timer_source_t aacp_mic_stats_timer;
static struct {
    uint32_t sdu_interval, au_interval;
    uint16_t demux_short;
    uint32_t last_sdu_ms;
} mic_stats;

static void aacp_packet_handler(uint8_t, uint16_t, uint8_t *, uint16_t);
static void aacp_schedule_retry(void);
static void aacp_try_mic_start(void);

static void aacp_mic_teardown(void) {
    aacp_mic_running = false;
    aacp_mic_preparing = false;
    aacp_mic_gap_reset = false;
    btstack_run_loop_remove_timer(&aacp_mic_stats_timer);
    aacp_mic_pcm_reset();
}

// Clear ownership before disconnect: a late event must not revive this channel.
void aacp_disconnect(void) {
    control_reset();
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
    pair_irk_known=false;
    memset(pair_irk,0,sizeof(pair_irk));
    memset(pair_key_address,0,sizeof(pair_key_address));
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
    if (memcmp(pair_key_address,addr,sizeof(pair_key_address))) {
        pair_irk_known=false;
        memset(pair_irk,0,sizeof(pair_irk));
        memcpy(pair_key_address,addr,sizeof(pair_key_address));
    }
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

uint32_t aacp_control_session(void) { return control_session; }
bool aacp_control_ready(void) { return aacp_connected && aacp_got_control && aacp_init_sent; }
bool aacp_control_address(bd_addr_t address) {
    if (!address || !aacp_control_ready()) return false;
    memcpy(address, aacp_addr, sizeof(bd_addr_t));
    return true;
}
const dongle_readback_t *aacp_control_readback(void) { return &control_readback; }
bool aacp_model_game_supported(void) {
    return settings.profile && settings.profile->silent_allow_off && settings.firmware_major>=8;
}
uint32_t aacp_receive_buffer_serial(void) { return receive_buffer_serial; }
uint16_t aacp_receive_buffer_last(void) { return receive_buffer_last; }
uint8_t aacp_receive_buffer(uint16_t milliseconds) {
    if (milliseconds!=60 && milliseconds!=150) return DONGLE_MALFORMED;
    if (!aacp_control_ready()) return DONGLE_DISCONNECTED;
    if (!aacp_model_game_supported()) return DONGLE_UNSUPPORTED;
    if ((milliseconds==60 && (usb_mic_requested() || aacp_mic_active())) ||
        aacp_tx_count>=AACP_TX_QUEUE_LEN-2) return DONGLE_BUSY;
    for (unsigned i=0;i<aacp_tx_count;++i)
        if (aacp_tx_queue[(aacp_tx_head+i)%AACP_TX_QUEUE_LEN].data==receive_buffer_tx) return DONGLE_BUSY;
    if (l2cap_get_remote_mtu_for_local_cid(aacp_cid)<sizeof(receive_buffer_tx)) return DONGLE_UNSUPPORTED;
    memcpy(receive_buffer_tx,"\x04\0\x04\0\x09\0\x0b",7);
    dongle_put32(receive_buffer_tx+7,milliseconds);
    return aacp_tx_enqueue(receive_buffer_tx,sizeof(receive_buffer_tx)) ? DONGLE_ACCEPTED : DONGLE_BUSY;
}
bool aacp_pair_irk(uint8_t address[6], uint8_t key[16]) {
    if (!pair_irk_known || !address || !key) return false;
    memcpy(address,pair_key_address,6); memcpy(key,pair_irk,16); return true;
}
static void pair_keys_report(const uint8_t *p, uint16_t n) {
    pair_irk_known=false;
    memset(pair_irk,0,sizeof(pair_irk));
    if (n!=47 || p[5] || p[6]!=2) return;
    const uint8_t *identity=NULL;
    unsigned kinds=0;
    for (unsigned off=7;off<n;off+=20) {
        uint8_t id=p[off];
        if ((id!=1 && id!=4) || (kinds&(1u<<id)) || p[off+1] || p[off+2]!=16 || p[off+3]) return;
        bool nonzero=false;
        for (unsigned i=0;i<16;++i) nonzero|=p[off+4+i]!=0;
        if (!nonzero) return;
        kinds|=1u<<id;
        if (id==1) identity=p+off+4;
    }
    if (!identity || kinds!=((1u<<1)|(1u<<4))) return;
    memcpy(pair_irk,identity,16); memcpy(pair_key_address,aacp_addr,6); pair_irk_known=true;
}
uint8_t aacp_setting_write(uint8_t id, uint32_t value) {
    if (setting_index(id)<0) return DONGLE_UNSUPPORTED;
    if (!setting_value_valid(id,value)) return DONGLE_MALFORMED;
    if (!aacp_control_ready()) return DONGLE_DISCONNECTED;
    if (!settings_supported(&settings,id,value)) return DONGLE_UNSUPPORTED;
    if (setting_pending || setting_result || usb_mic_requested() || aacp_mic_active() ||
        aacp_tx_count>=AACP_TX_QUEUE_LEN-2)
        return DONGLE_BUSY;
    if (l2cap_get_remote_mtu_for_local_cid(aacp_cid)<sizeof(setting_tx)) return DONGLE_UNSUPPORTED;
    memcpy(setting_tx,"\x04\0\x04\0\x09\0",6);
    setting_tx[6]=id; dongle_put32(setting_tx+7,value);
    setting_since=btstack_run_loop_get_time_ms();
    setting_pending=true;
    if (!aacp_tx_enqueue(setting_tx,sizeof(setting_tx))) {
        setting_pending=false;
        return DONGLE_BUSY;
    }
    return DONGLE_ACCEPTED;
}
void aacp_setting_cancel(void) {
    unsigned kept=0;
    for (unsigned i=0;i<aacp_tx_count;++i) {
        aacp_pkt_t packet=aacp_tx_queue[(aacp_tx_head+i)%AACP_TX_QUEUE_LEN];
        if (packet.data==setting_tx) continue;
        aacp_tx_queue[(aacp_tx_head+kept++)%AACP_TX_QUEUE_LEN]=packet;
    }
    aacp_tx_count=(uint8_t)kept;
    setting_pending=false;
    setting_result=0;
}
int aacp_setting_take(void) {
    int result=setting_result;
    setting_result=0;
    return result;
}
uint16_t aacp_settings_snapshot(uint8_t *out, uint16_t capacity) {
    if (!aacp_control_ready()) return 0;
    return settings_snapshot(&settings,btstack_run_loop_get_time_ms(),out,capacity);
}
bool aacp_control_anc(uint8_t mode) {
    if (!aacp_control_ready() || !control_readback.anc || (mode != 2 && mode != 3)) return false;
    // Keep capacity for microphone STOP and recovery even if a host floods controls.
    for (unsigned i = 0; i < aacp_tx_count; ++i) {
        const uint8_t *p = aacp_tx_queue[(aacp_tx_head + i) % AACP_TX_QUEUE_LEN].data;
        if (p == control_anc || p == control_transparency) return false;
    }
    if (aacp_tx_count >= AACP_TX_QUEUE_LEN - 2) return false;
    return aacp_tx_enqueue(mode == 2 ? control_anc : control_transparency, sizeof(control_anc));
}

void aacp_research_cancel(void) {
    unsigned kept = 0;
    for (unsigned i = 0; i < aacp_tx_count; ++i) {
        aacp_pkt_t packet = aacp_tx_queue[(aacp_tx_head + i) % AACP_TX_QUEUE_LEN];
        if (packet.data == research_tx) continue;
        aacp_tx_queue[(aacp_tx_head + kept++) % AACP_TX_QUEUE_LEN] = packet;
    }
    aacp_tx_count = (uint8_t)kept;
    research_pending = false;
    research_result = 0;
}
static bool research_valid(const uint8_t *p, uint16_t n) {
    static const uint8_t header[] = {4,0,4,0};
    if (!p || n < 6 || n > sizeof(research_tx) || memcmp(p, header, sizeof(header)) || p[5]) return false;
    if (p[4] == 9 && n == 11 && p[9] == 0 && p[10] == 0) {
        if (p[6] == 0x2c) return p[7] <= 2 && (p[8] == 1 || p[8] == 2);
        if (p[6] == 0x33) return (p[7] == 1 || p[7] == 2) && p[8] == 0;
        if (p[6] == 0x0d) return (p[7] == 2 || p[7] == 3) && p[8] == 0;
    }
    if (p[4] == 0x53) return n == 140 && p[6] == 0x84 && p[7] == 0 && p[8] == 2 &&
        p[9] <= 2 && (p[10] == 1 || p[10] == 2) && (p[11] == 1 || p[11] == 2);
    if (p[4] == 0x0f) return n == sizeof(aacp_request_notifications) &&
        memcmp(p, aacp_request_notifications, n) == 0;
    if (p[4] != 0x1b || n != 41 || little_endian_read_16(p, 14) != 25 ||
        dongle_u32(p + 10) >= 1000000 || memcmp(p + 35, "+00:00", 6)) return false;
    for (unsigned i = 16; i < 35; ++i) {
        uint8_t separator = i == 20 || i == 23 ? '-' : i == 26 ? 'T' : i == 29 || i == 32 ? ':' : 0;
        if (separator ? p[i] != separator : (p[i] < '0' || p[i] > '9')) return false;
    }
    return true;
}
bool aacp_research_send(const uint8_t *packet, uint16_t length) {
    if (!aacp_control_ready() || research_pending || research_result || !research_valid(packet, length) ||
        aacp_tx_count >= AACP_TX_QUEUE_LEN - 2 || length > l2cap_get_remote_mtu_for_local_cid(aacp_cid)) return false;
    memcpy(research_tx, packet, length);
    research_since = btstack_run_loop_get_time_ms();
    research_pending = true;
    if (aacp_tx_enqueue(research_tx, length)) return true;
    research_pending = false;
    return false;
}
int aacp_research_take(void) {
    int result = research_result;
    research_result = 0;
    return result;
}
uint16_t aacp_research_cached(uint8_t slot, uint8_t *out, uint16_t capacity) {
    if (slot==6) return aacp_settings_snapshot(out,capacity);
    if (!aacp_control_ready() || slot < 1 || slot > sizeof(reports) / sizeof(reports[0]) || !out) return 0;
    cached_report_t *report = &reports[slot - 1];
    if (!report->length || capacity < report->length + 8u) return 0;
    dongle_put32(out, btstack_run_loop_get_time_ms() - report->updated);
    dongle_put32(out + 4, report->revision);
    memcpy(out + 8, report->packet, report->length);
    return report->length + 8;
}
static void research_cache(const uint8_t *p, uint16_t n) {
    int slot = -1;
    if (p[4] == 0x1d) slot = 0;
    else if (p[4] == 9 && n >= 7 && p[6] == 0x2c) slot = 1;
    else if (p[4] == 9 && n >= 7 && p[6] == 0x33) slot = 2;
    else if (p[4] == 0x53) {
        if (n == 10 && p[6] == 2 && p[7] == 0 && p[8] == 1) return;
        slot = 3;
    } else if (p[4] == 6) slot = 4;
    if (slot < 0) return;
    cached_report_t *report = &reports[slot];
    if (++report->revision == 0) ++report->revision;
    report->length = 0;
    if (n > report->capacity) return;
    memcpy(report->packet, p, n);
    report->length = n;
    report->updated = btstack_run_loop_get_time_ms();
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
    aacp_mic_start_waiting_for_reset = false;
    uint8_t kept = 0;
    for (unsigned i = 0; i < aacp_tx_count; ++i) {
        aacp_pkt_t p = aacp_tx_queue[(aacp_tx_head + i) % AACP_TX_QUEUE_LEN];
        if (p.data == aacp_mic_start_bytes || p.data == aacp_mic_stop_bytes) continue;
        aacp_tx_queue[(aacp_tx_head + kept++) % AACP_TX_QUEUE_LEN] = p;
    }
    aacp_tx_count = kept;
}

static void aacp_channel_failed(void) {
    control_reset();
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

static void aacp_mic_update_progress(void) {
    uint32_t completed = aacp_mic_dec_completed();
    if (completed == aacp_mic_completed_seen) return;
    aacp_mic_completed_seen = completed;
    if (aacp_mic_want && aacp_mic_running && usb_mic_requested()) {
        mic_stats.last_sdu_ms = btstack_run_loop_get_time_ms();
        aacp_mic_gap_reset = false;
    }
}

static void aacp_mic_stats_report(btstack_timer_source_t *ts) {
    if (aacp_mic_want && !usb_mic_requested()) {
        aacp_mic_stop();
        return;
    }
    if (!aacp_mic_want || !aacp_mic_running || !aacp_connected) return;
    aacp_mic_update_progress();
    uint32_t now = btstack_run_loop_get_time_ms();
    printf("[MIC] sdu/s=%lu au/s=%lu malformed=%u\n",
           (unsigned long)mic_stats.sdu_interval,
           (unsigned long)mic_stats.au_interval, mic_stats.demux_short);
    aacp_mic_dec_report();
    mic_stats.sdu_interval = mic_stats.au_interval = 0;
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
        aacp_mic_start_queued()) return;
    if (!aacp_mic_dec_ready()) {
        if (aacp_mic_preparing) {
            aacp_mic_failed = true;
            aacp_mic_preparing = false;
            printf("[MIC] decoder worker reset failed\n");
        }
        return;
    }
    if (!aacp_mic_preparing) {
        if (!aacp_mic_dec_reset()) {
            aacp_mic_failed = true;
            return;
        }
        aacp_mic_dec_reset_stats();
        aacp_mic_preparing = true;
        aacp_mic_prepare_since_ms = btstack_run_loop_get_time_ms();
    }
    if (!aacp_mic_dec_reset_done()) {
        if ((uint32_t)(btstack_run_loop_get_time_ms() - aacp_mic_prepare_since_ms) >=
            AACP_MIC_PREPARE_TIMEOUT_MS) {
            aacp_mic_failed = true;
            printf("[MIC] decoder worker reset timed out\n");
        }
        return;
    }
    if (aacp_tx_enqueue(aacp_mic_start_bytes, sizeof(aacp_mic_start_bytes))) {
        memset(&mic_stats, 0, sizeof(mic_stats));
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
    uint32_t now=btstack_run_loop_get_time_ms();
    if ((p.data==setting_tx || p.data==research_tx) &&
        (!dongle_rpc_transport_current() || usb_mic_requested() || aacp_mic_active() ||
         (uint32_t)(now-(p.data==setting_tx ? setting_since : research_since))>=3000u)) {
        if (p.data==setting_tx) {
            aacp_setting_cancel();
            setting_result=2;
        } else {
            aacp_research_cancel();
            research_result=2;
        }
        if (aacp_tx_count) l2cap_request_can_send_now_event(aacp_cid);
        return;
    }
    if (p.data == aacp_mic_start_bytes && !usb_mic_requested()) {
        aacp_mic_stop();
        return;
    }
    if (p.data == aacp_mic_start_bytes && (!aacp_mic_want || aacp_mic_failed)) {
        aacp_drop_mic_commands();
        return;
    }
    if (p.data == aacp_mic_start_bytes && !aacp_mic_dec_reset_done()) {
        if (!aacp_mic_dec_ready()) {
            aacp_mic_failed = true;
            aacp_mic_preparing = false;
            aacp_drop_mic_commands();
            printf("[MIC] decoder worker unavailable before Start\n");
        } else {
            aacp_mic_start_waiting_for_reset = true;
        }
        return;
    }
    if (l2cap_send(aacp_cid, (uint8_t *)p.data, p.len) != ERROR_CODE_SUCCESS) {
        l2cap_request_can_send_now_event(aacp_cid);
        return;
    }
    aacp_tx_head = (uint8_t)((aacp_tx_head + 1) % AACP_TX_QUEUE_LEN);
    aacp_tx_count--;
    // Pro 3 can leave Allow Off unreported; a sent choice is not a device report.
    if (p.data==setting_tx && setting_tx[6]==0x34 && settings.profile && settings.profile->silent_allow_off)
        settings.allow_off_requested=dongle_u32(setting_tx+7)==1;
    if (p.data==setting_tx) { setting_pending=false; setting_result=1; }
    if (p.data==receive_buffer_tx) {
        receive_buffer_last=(uint16_t)dongle_u32(p.data+7);
        if (++receive_buffer_serial==0) ++receive_buffer_serial;
    }
    aacp_tx_since_ms = btstack_run_loop_get_time_ms();
    if (p.data == research_tx) { research_pending = false; research_result = 1; }
    if (p.data == aacp_request_notifications) aacp_init_sent = true;
    if (p.data == aacp_mic_start_bytes) {
        aacp_mic_preparing = false;
        aacp_mic_running = true;
        aacp_mic_remote_active = true;
        aacp_mic_completed_seen = aacp_mic_dec_completed();
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
    aacp_mic_update_progress();
    if (aacp_mic_start_waiting_for_reset && aacp_mic_dec_reset_done()) {
        aacp_mic_start_waiting_for_reset = false;
        if (aacp_cid != 0 && aacp_tx_count != 0)
            l2cap_request_can_send_now_event(aacp_cid);
    }
    uint32_t now = btstack_run_loop_get_time_ms();
    if (research_pending && (uint32_t)(now - research_since) >= 3000u) {
        aacp_research_cancel();
        research_result = 2;
    }
    if (setting_pending && (uint32_t)(now-setting_since)>=3000u) {
        aacp_setting_cancel();
        setting_result=2;
    }
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
    mic_stats.sdu_interval++;
    if (!aacp_mic_gap_reset && (uint32_t)(now - mic_stats.last_sdu_ms) >= 2000) {
        if (!aacp_mic_dec_reset()) return;
        aacp_mic_gap_reset = true;
    }
    for (unsigned off = TYPE58_HEADER_LEN; off < size;) {
        if (!usb_mic_requested()) {
            aacp_mic_stop();
            return;
        }
        uint8_t len = sdu[off + 4];
        aacp_mic_dec_submit(sdu + off + 5, len);
        mic_stats.au_interval++;
        off += 5 + len;
    }
}

static void aacp_packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (type == L2CAP_DATA_PACKET) {
        if (!aacp_session || !aacp_connected || channel != aacp_cid ||
            !aacp_has_header(packet, size)) return;
        // All microphone messages, including malformed ones, stay out of control readiness.
        if (little_endian_read_16(packet, 4) == 0x58) {
            aacp_handle_audio_sdu(packet, size);
        } else {
            settings_packet(&settings,packet,size,btstack_run_loop_get_time_ms());
            if (packet[4]==0x31) pair_keys_report(packet,size);
            if (size>=7 && packet[4]==9 && packet[5]==0 && packet[6]==0x0d) {
                const setting_report_t *anc=&settings.reports[setting_index(0x0d)];
                control_readback.anc=anc->known ? (uint8_t)anc->value : 0;
            }
            research_cache(packet, size);
            dongle_readback_packet(&control_readback, packet, size, btstack_run_loop_get_time_ms());
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
            aacp_tx_enqueue(aacp_request_keys, sizeof(aacp_request_keys));
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
