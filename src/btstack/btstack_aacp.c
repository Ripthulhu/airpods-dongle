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
#include <stdatomic.h>

#include "btstack.h"
#include "btstack_aacp.h"
#include "aacp_mic_dec.h"
#include "btstack_avdtp_source.h"
#include "dongle_settings.h"
#include "btstack_ble_scan.h"
#include "../tinyusb/dongle_rpc.h"
#include "../tinyusb/uac.h"
#include "phone_call.h"
#include "peer_address.h"
#include "aacp_routing.h"
#if AIRPODS_HEAD_TRACKING
#include "head_motion.h"
#include "../tinyusb/motion_usb.h"
#endif

static atomic_bool audio_authority;

bool aacp_audio_owned(void) { return atomic_load_explicit(&audio_authority, memory_order_acquire); }

static _Atomic aacp_mic_owner_t mic_owner;
static atomic_flag mic_reader = ATOMIC_FLAG_INIT;

static aacp_mic_owner_t requested_mic_owner(void) {
    if (!aacp_audio_owned()) return AACP_MIC_NONE;
    if (usb_mic_requested()) return AACP_MIC_USB;
    return phone_call_mic_requested() ? AACP_MIC_CALL : AACP_MIC_NONE;
}

bool aacp_mic_requested(void) { return requested_mic_owner() != AACP_MIC_NONE; }

void aacp_mic_call_changed(void) {
    if (atomic_load_explicit(&mic_owner, memory_order_acquire) != AACP_MIC_CALL) return;
    atomic_store_explicit(&mic_owner, AACP_MIC_NONE, memory_order_release);
    aacp_mic_pcm_reset();
}

bool aacp_mic_update_owner(bool fresh_usb_request) {
    aacp_mic_owner_t requested = requested_mic_owner();
    if (!fresh_usb_request && requested == atomic_load_explicit(&mic_owner, memory_order_acquire)) return false;
    atomic_store_explicit(&mic_owner, AACP_MIC_NONE, memory_order_release);
    dongle_playback_mic_request_changed();
    aacp_mic_stop();
    aacp_mic_pcm_reset();
    atomic_store_explicit(&mic_owner, requested, memory_order_release);
    return true;
}

static uint32_t mic_read(aacp_mic_owner_t owner, int16_t *pcm, uint32_t frames) {
    if (atomic_flag_test_and_set_explicit(&mic_reader, memory_order_acquire)) return 0;
    uint32_t copied = 0;
    bool allowed = aacp_audio_owned() &&
        (owner == AACP_MIC_USB ? usb_mic_requested() : phone_call_mic_requested() && !usb_mic_requested());
    if (owner == atomic_load_explicit(&mic_owner, memory_order_acquire) && allowed) {
        copied = aacp_mic_pcm_read(pcm, frames);
        allowed = aacp_audio_owned() &&
            (owner == AACP_MIC_USB ? usb_mic_requested() : phone_call_mic_requested() && !usb_mic_requested());
        if (owner != atomic_load_explicit(&mic_owner, memory_order_acquire) || !allowed) copied = 0;
    }
    atomic_flag_clear_explicit(&mic_reader, memory_order_release);
    return copied;
}
uint32_t aacp_mic_usb_read(int16_t *pcm, uint32_t frames) { return mic_read(AACP_MIC_USB, pcm, frames); }
uint32_t aacp_mic_call_read(int16_t *pcm, uint32_t frames) { return mic_read(AACP_MIC_CALL, pcm, frames); }

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
#define AACP_INIT_RETRY_MS    3000u

// Mic lifecycle: the ONLY trigger is the USB alt setting — main.c calls
// aacp_mic_start()/aacp_mic_stop() when the host opens/closes the recording
// stream. If the host opens the mic before the AACP channel is up (or the
// channel drops mid-capture), the start is remembered and fired as soon as
// the first control packet confirms the channel.

// --- Fixed init byte sequences (verbatim from librepods) ---

// Service 4 version 1.3, with one bounded 1.2 fallback for older peers.
static const uint8_t aacp_handshake[] = {
    0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t aacp_handshake_legacy[] = {
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
static const uint8_t aacp_request_capabilities[] = {4,0,4,0,1,0,8};
static const uint8_t aacp_request_source[] = {4,0,4,0,0x0d,0};

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
static uint32_t research_eq_revision;
static bool research_pending;
static int research_result;
static uint8_t model_report[384], hearing_report[11], assist_report[11], pme_report[140], ear_report[32], eq_report[13];
typedef struct { uint8_t *packet; uint16_t capacity, length; uint32_t updated, revision; } cached_report_t;
static cached_report_t reports[] = {
    {model_report, sizeof(model_report), 0, 0, 0}, {hearing_report, sizeof(hearing_report), 0, 0, 0},
    {assist_report, sizeof(assist_report), 0, 0, 0}, {pme_report, sizeof(pme_report), 0, 0, 0},
    {ear_report, sizeof(ear_report), 0, 0, 0}, {eq_report, sizeof(eq_report), 0, 0, 0}
};
static dongle_readback_t control_readback;
static dongle_settings_t settings;
static uint8_t setting_tx[AACP_CONTROL_COMMAND_SIZE];
static uint32_t setting_since;
static bool setting_pending;
static int setting_result;
static uint8_t receive_buffer_tx[AACP_CONTROL_COMMAND_SIZE];
static uint32_t receive_buffer_serial;
static uint16_t receive_buffer_last;
static uint8_t pair_irk[16], pair_advertising_key[16], pair_key_address[6];
static bool pair_irk_known, pair_case_supported;
static uint32_t control_session;
// Validated AudioSource CALL state belongs to the current AACP peer/session.
static bool foreign_call;
enum remote_playback { REMOTE_UNKNOWN, REMOTE_ACTIVE, REMOTE_IDLE, REMOTE_RETURNING, REMOTE_CONSUMED, REMOTE_HANDOFF };
enum route_topology { TOPO_UNKNOWN, TOPO_LOCAL, TOPO_BOTH, TOPO_FOREIGN_ONLY };
enum route_source_class { SOURCE_ANONYMOUS, SOURCE_LOCAL, SOURCE_PEER, SOURCE_OTHER, SOURCE_INVALID };
#define SOURCE_REPLY_MS 800u
#define SOURCE_REFRESH_MS 4000u
// Source requests have no wire transaction ID. Only retain a directed reply long
// enough for the companion device list, and cancel it on newer routing activity.
static struct {
    uint32_t sent, count, settle_since, subscribed_at;
    uint8_t host[6], type, reply_class;
    bool settling, subscribed, queued, awaiting, directed, busy_seen;
} source_read;
// One immutable queued packet at a time, not a new packet queue or audio buffer.
static struct {
    uint8_t peer[6], other[6];
    uint8_t tx[144], stage, tx_kind;
    bool have_other, known, relinquished, pending, queued, confirmed;
    enum route_topology topology;
    uint8_t call_host[6];
    enum remote_playback remote;
    uint32_t idle_since, automatic_returns;
    uint32_t since, own_sent;
} routing;
static uint32_t routing_revision = 1, routing_nonce;
// Only routing changes are recorded. Formatting happens on a dedicated CDC read.
static struct route_event {uint32_t ms; char kind; uint8_t a,b;} route_events[8];
static uint32_t route_sequence;
static uint8_t route_event_count;
static void route_event(char kind, unsigned a, unsigned b) {
    route_events[route_sequence++ % 8u]=(struct route_event){btstack_run_loop_get_time_ms(),kind,(uint8_t)a,(uint8_t)b};
    if (route_event_count<8) ++route_event_count;
}
void aacp_route_avdtp_event(uint8_t stage,uint8_t status) {route_event('a',stage,status);}
static bool routing_peer_listed(void) {
    return routing.topology==TOPO_BOTH || routing.topology==TOPO_FOREIGN_ONLY;
}
static uint8_t routing_action;
#define AACP_TAKEOVER_MS 10000u
#define AUTO_IDLE_SETTLE_MS 350u
static void routing_poll(uint32_t now);
static void routing_packet(const uint8_t *packet, uint16_t size);
static void routing_cancel(void);
static void routing_authority(bool allowed);
// Static entries stay valid until their queued transmission completes.
static const uint8_t control_anc[] = {AACP_HEADER_BYTES,AACP_OPCODE_CONTROL_COMMAND,0,
    DONGLE_SETTING_LISTENING_MODE,DONGLE_LISTENING_MODE_ANC,0,0,0};
static const uint8_t control_transparency[] = {AACP_HEADER_BYTES,AACP_OPCODE_CONTROL_COMMAND,0,
    DONGLE_SETTING_LISTENING_MODE,DONGLE_LISTENING_MODE_TRANSPARENCY,0,0,0};
static void control_reset(void) {
#if AIRPODS_HEAD_TRACKING
    head_motion_reset();
#endif
    ++control_session;
    route_event('e',0,0);
    dongle_readback_clear(&control_readback);
    settings_reset(&settings);
    routing.queued = routing.confirmed = routing.known = routing.have_other = false;
    if (routing.remote == REMOTE_RETURNING) routing.pending = false;
    routing.remote = REMOTE_UNKNOWN;
    routing.topology = TOPO_UNKNOWN;
    memset(&source_read,0,sizeof(source_read));
    source_read.reply_class=255;
    foreign_call=false;
    routing.stage = 0;
    routing_authority(false);
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
static uint16_t aacp_cid, retiring_aacp_cid;
static bd_addr_t retiring_aacp_peer;
static bool aacp_session, aacp_connected, aacp_got_control, aacp_init_sent, aacp_init_retried;
static bd_addr_t aacp_addr;
static uint8_t aacp_attempts;
static uint32_t aacp_channel_since_ms, aacp_tx_since_ms;
static btstack_timer_source_t aacp_connect_timer;

#define TYPE58_HEADER_LEN 22
static bool aacp_mic_running, aacp_mic_want, aacp_mic_failed, aacp_mic_remote_active;
static bool aacp_mic_stopping;
static bool aacp_mic_restore_required, aacp_mic_restore_started, aacp_mic_restore_complete;
static uint32_t aacp_mic_restore_revision, aacp_mic_restore_since;
static bool aacp_mic_playback_waiting;
static uint32_t aacp_mic_playback_since;
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
    aacp_mic_playback_waiting=false;
    aacp_mic_gap_reset = false;
    btstack_run_loop_remove_timer(&aacp_mic_stats_timer);
    aacp_mic_pcm_reset();
}

static uint16_t aacp_detach_channel(void) {
    uint16_t cid = aacp_cid;
    aacp_cid=0;
    if (cid && !aacp_connected) {
        // BTstack still owns this pending page. A raw disconnect here removes
        // its WAIT_CONNECTION_COMPLETE state and leaks a channel on page failure.
        retiring_aacp_cid=cid;memcpy(retiring_aacp_peer,aacp_addr,6);
        return 0;
    }
    return cid;
}

// Clear ownership before disconnect: a late event must not revive this channel.
void aacp_disconnect(void) {
    if (aacp_mic_restore_required && aacp_mic_restore_started)
        aacp_mic_recovery_failed();
    control_reset();
    routing_cancel();
    uint16_t cid=aacp_detach_channel();
    aacp_session = false;
    aacp_connected = aacp_got_control = aacp_init_sent = aacp_init_retried = false;
    aacp_mic_remote_active = false;
    aacp_mic_stopping = false;
    aacp_mic_restore_required=aacp_mic_restore_started=aacp_mic_restore_complete=false;
    aacp_tx_head = aacp_tx_count = 0;
    btstack_run_loop_remove_timer(&aacp_connect_timer);
    aacp_mic_teardown();
    if (cid) l2cap_disconnect(cid);
}

void aacp_media_disconnected(void) {
    bool requested = routing.remote != REMOTE_RETURNING && routing.stage == 0 && aacp_takeover_pending(routing.peer);
    uint32_t since = routing.since;
    aacp_disconnect();
    routing.pending = requested;
    routing.since = since;
}

void aacp_init(void) {
    memset(&routing, 0, sizeof(routing));
    routing_revision=1; routing_nonce=0; routing_action=0;
    atomic_store_explicit(&audio_authority, false, memory_order_release);
    pair_irk_known=false;
    pair_case_supported=false;
    memset(pair_irk,0,sizeof(pair_irk));
    memset(pair_advertising_key,0,sizeof(pair_advertising_key));
    memset(pair_key_address,0,sizeof(pair_key_address));
    aacp_disconnect();
    aacp_mic_want = aacp_mic_failed = false;
    aacp_mic_restarts = 0;
    aacp_attempts = 0;
}

static void aacp_do_connect(btstack_timer_source_t *ts) {
    UNUSED(ts);
    if (!aacp_session || aacp_cid || retiring_aacp_cid) return;
    aacp_attempts++;
    aacp_channel_since_ms = btstack_run_loop_get_time_ms();
    uint8_t status = l2cap_create_channel(aacp_packet_handler, aacp_addr, AACP_PSM,
                                         AACP_LOCAL_MTU, &aacp_cid);
    route_event('c',0,status);
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
    bool requested = aacp_takeover_pending(addr);
    uint32_t requested_since = routing.since;
    aacp_disconnect();
    if (memcmp(routing.peer, addr, 6)) {
        memset(&routing, 0, sizeof(routing));
        memcpy(routing.peer, addr, 6);
    }
    routing.pending = requested;
    routing.since = requested_since;
    // A new media signaling connection must resolve who owns the buds before Start.
    atomic_store_explicit(&audio_authority, false, memory_order_release);
    if (memcmp(pair_key_address,addr,sizeof(pair_key_address))) {
        pair_irk_known=false;
        pair_case_supported=false;
        memset(pair_irk,0,sizeof(pair_irk));
        memset(pair_advertising_key,0,sizeof(pair_advertising_key));
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
bool aacp_mic_active(void) { return aacp_mic_remote_active || aacp_mic_restore_required; }
bool aacp_mic_uplink_active(void) { return aacp_mic_remote_active || aacp_mic_stopping; }
void aacp_mic_recovery_failed(void) {
    aacp_mic_want=false; aacp_mic_failed=true;
}

static bool aacp_tx_enqueue(const uint8_t *data, uint16_t len) {
    if (aacp_cid == 0 || !aacp_connected || aacp_tx_count == AACP_TX_QUEUE_LEN)
        return false;
    uint8_t slot = (uint8_t)((aacp_tx_head + aacp_tx_count) % AACP_TX_QUEUE_LEN);
    aacp_tx_queue[slot] = (aacp_pkt_t){ data, len };
    if (aacp_tx_count++ == 0) aacp_tx_since_ms = btstack_run_loop_get_time_ms();
    l2cap_request_can_send_now_event(aacp_cid);
    return true;
}

// Publish the complete batch before requesting a callback, retaining two urgent-control slots.
static bool aacp_startup_enqueue(bool legacy) {
    const aacp_pkt_t packets[] = {
        {legacy ? aacp_handshake_legacy : aacp_handshake, sizeof(aacp_handshake)},
        {aacp_set_feature_flags, sizeof(aacp_set_feature_flags)},
        {aacp_request_notifications, sizeof(aacp_request_notifications)},
        {aacp_request_capabilities, sizeof(aacp_request_capabilities)},
        {aacp_request_keys, sizeof(aacp_request_keys)}
    };
    unsigned count=legacy ? 4u : 5u;
    if (!aacp_session || !aacp_connected || !aacp_cid ||
        aacp_tx_count>AACP_TX_QUEUE_LEN-count-2u) return false;
    if (!aacp_tx_count) aacp_tx_since_ms=btstack_run_loop_get_time_ms();
    for (unsigned i=0;i<count;++i)
        aacp_tx_queue[(aacp_tx_head+aacp_tx_count+i)%AACP_TX_QUEUE_LEN]=packets[i];
    aacp_tx_count=(uint8_t)(aacp_tx_count+count);
    if (legacy) { aacp_init_retried=true; aacp_init_sent=false; }
    l2cap_request_can_send_now_event(aacp_cid);
    return true;
}

static void routing_cancel(void) {
    unsigned kept = 0;
    for (unsigned i = 0; i < aacp_tx_count; ++i) {
        aacp_pkt_t packet = aacp_tx_queue[(aacp_tx_head + i) % AACP_TX_QUEUE_LEN];
        if (packet.data != routing.tx)
            aacp_tx_queue[(aacp_tx_head + kept++) % AACP_TX_QUEUE_LEN] = packet;
    }
    aacp_tx_count = (uint8_t)kept;
    routing.pending = routing.queued = routing.confirmed = false;
    if (routing.remote == REMOTE_RETURNING) routing.remote = REMOTE_CONSUMED;
    routing.stage = 0;
}

void aacp_receive_buffer_cancel(void) {
    unsigned kept = 0;
    for (unsigned i = 0; i < aacp_tx_count; ++i) {
        aacp_pkt_t packet = aacp_tx_queue[(aacp_tx_head + i) % AACP_TX_QUEUE_LEN];
        if (packet.data != receive_buffer_tx)
            aacp_tx_queue[(aacp_tx_head + kept++) % AACP_TX_QUEUE_LEN] = packet;
    }
    aacp_tx_count = (uint8_t)kept;
}

static void routing_authority(bool allowed) {
    bool previous = atomic_exchange_explicit(&audio_authority, allowed, memory_order_acq_rel);
    if (previous != allowed) dongle_playback_ownership_changed(allowed);
#if AIRPODS_HEAD_TRACKING
    if (previous && !allowed) usb_motion_invalidate();
#endif
}

static bool routing_request_takeover(const uint8_t peer[6],bool automatic) {
    if (!peer || !peer_address_valid(peer) || usb_mic_requested() ||
        phone_call_mic_requested() || aacp_mic_active() ||
        (foreign_call && !memcmp(peer,aacp_addr,6))) return false;
    if (routing.pending && !aacp_takeover_pending(routing.peer)) routing_cancel();
    if (routing.pending && memcmp(peer, routing.peer, 6)) return false;
    source_read.awaiting=source_read.directed=source_read.busy_seen=false;
    routing.remote = automatic ? REMOTE_RETURNING : REMOTE_CONSUMED;
    if (!++routing_revision) ++routing_revision;
    routing_nonce=0; routing_action=0;
    if (!automatic) dongle_playback_connect_requested();
    if (routing.pending) return true;
    if (memcmp(peer, routing.peer, 6)) {
        routing_cancel();
        memset(&routing, 0, sizeof(routing));
        memcpy(routing.peer, peer, 6);
        atomic_store_explicit(&audio_authority, false, memory_order_release);
    }
    routing.pending = true;
    routing.confirmed = false;
    routing.stage = 0;
    routing.since = btstack_run_loop_get_time_ms();
    return true;
}
bool aacp_takeover_request(const uint8_t peer[6]) { return routing_request_takeover(peer,false); }

bool aacp_takeover_pending(const uint8_t peer[6]) {
    return peer && routing.pending && !(foreign_call && !memcmp(peer,aacp_addr,6)) && !memcmp(peer, routing.peer, 6) &&
           (routing.remote != REMOTE_RETURNING || dongle_playback_return_signal(peer, btstack_run_loop_get_time_ms())) &&
           (uint32_t)(btstack_run_loop_get_time_ms() - routing.since) < AACP_TAKEOVER_MS;
}

aacp_routing_state_t aacp_routing_state(void) {
    bool pending=aacp_takeover_pending(routing.peer);
    return (aacp_routing_state_t){routing_revision,
        pending ? btstack_run_loop_get_time_ms()-routing.since : 0, routing_nonce,
        (routing.known?1u:0u)|(aacp_audio_owned()?2u:0u)|(pending?4u:0u),
        routing.stage, routing_action};
}

void aacp_routing_override(void) {
    if (!++routing_revision) ++routing_revision;
    routing_nonce=0; routing_action=0;
    if(!aacp_audio_owned()) routing.relinquished=true;
    routing_cancel();
    source_read.awaiting=source_read.directed=source_read.busy_seen=false;
    routing.remote = REMOTE_CONSUMED;
}

void aacp_routing_receipt(uint32_t nonce, uint8_t action) {
    routing_nonce=nonce; routing_action=action;
}

static bool routing_source_fresh(uint32_t now) {
    return source_read.subscribed && now-(source_read.count ? source_read.sent : source_read.subscribed_at)<SOURCE_REPLY_MS;
}
static void routing_source_poll(uint32_t now) {
    if (!routing_source_fresh(now))
        source_read.awaiting=source_read.directed=source_read.busy_seen=false;
    if (source_read.queued || routing.pending || aacp_audio_owned() || !aacp_control_ready() || aacp_tx_count) return;
    if (!source_read.subscribed) {
        if (!source_read.settling) {
            if (!dongle_playback_auto_standby(aacp_addr)) return;
            source_read.settling=true;source_read.settle_since=now;
        }
        if (now-source_read.settle_since<300u || !dongle_playback_auto_standby(aacp_addr)) return;
        source_read.queued=true; // Re-arm notifications after the service reported ready.
        if (!aacp_tx_enqueue(aacp_request_notifications,sizeof(aacp_request_notifications))) source_read.queued=false;
        return;
    }
    if (routing.remote==REMOTE_IDLE || routing_source_fresh(now) ||
        (source_read.count && now-source_read.sent<SOURCE_REFRESH_MS) || !dongle_playback_return_signal(aacp_addr,now)) return;
    source_read.queued=true;
    if (!aacp_tx_enqueue(aacp_request_source,sizeof(aacp_request_source))) source_read.queued=false;
}
static void routing_poll(uint32_t now) {
    routing_source_poll(now);
    if (routing.remote == REMOTE_IDLE && !routing.pending && !aacp_audio_owned() &&
        aacp_control_ready() && !memcmp(routing.peer, aacp_addr, 6)) {
        uint32_t signal = dongle_playback_return_signal(aacp_addr, now);
        // Require sound after the remote stopped, even when Windows kept sending USB silence.
        if (signal && now-routing.idle_since>=AUTO_IDLE_SETTLE_MS &&
            now - (signal - 1u) < now - routing.idle_since && routing_request_takeover(aacp_addr,true)) {
            ++routing.automatic_returns;
        }
    } else if (routing.remote == REMOTE_RETURNING && !dongle_playback_return_signal(aacp_addr, now)) {
        routing_cancel();
        routing.remote = REMOTE_IDLE;
        routing.idle_since = now;
    }
    if (routing.pending && (uint32_t)(now - routing.since) >= AACP_TAKEOVER_MS) {
        if (!aacp_audio_owned()) routing.relinquished = true;
        routing_cancel();
        return;
    }
    if (!aacp_control_ready() || routing.queued || memcmp(routing.peer, aacp_addr, 6)) return;
    // The phone has already yielded. Give OWN a chance to complete on its own
    // before asking the other host to hand over or negotiating an unowned Start.
    if (routing.remote==REMOTE_RETURNING && routing.pending && routing.stage && routing.stage<3) {
        if (routing.confirmed) routing.stage=3;
        else if (now-routing.own_sent<250u) return;
    }
    if (routing.pending && routing.stage >= 3) {
        if (routing.confirmed || aacp_audio_owned()) {
            routing.pending = routing.relinquished = false;
            if (routing.remote == REMOTE_RETURNING) {
                routing.remote = REMOTE_CONSUMED;
                dongle_playback_automatic_returned();
            }
            bool already_owned = aacp_audio_owned();
            routing_authority(true);
            if (already_owned) dongle_playback_ownership_changed(true);
        }
        return;
    }
    uint8_t local[6];
    gap_local_bd_addr(local);
    if (!aacp_route_address_valid(local) || aacp_tx_count >= AACP_TX_QUEUE_LEN - 2u) return;
    aacp_route_kind_t kind;
    if (routing.pending) {
        if (routing.stage && !routing.have_other) { routing.stage = 3; return; }
        kind = routing.stage == 0 ? AACP_ROUTE_OWN :
               routing.stage == 1 ? AACP_ROUTE_MEDIA : AACP_ROUTE_HIJACK;
    } else return;
    uint16_t length = (uint16_t)aacp_route_build(routing.tx, sizeof(routing.tx), kind, local, routing.other);
    if (!length) return;
    if (length > l2cap_get_remote_mtu_for_local_cid(aacp_cid)) {
        if (routing.pending && !aacp_audio_owned()) routing.relinquished = true;
        routing_cancel();
        return;
    }
    // Enqueue may dispatch CAN_SEND synchronously: publish immutable buffer state first.
    routing.queued = true;
    routing.tx_kind = (uint8_t)kind;
    if (!aacp_tx_enqueue(routing.tx, length)) routing.queued = false;
}

static void routing_remote_changed(enum remote_playback remote) {
    if (routing.remote == REMOTE_RETURNING) routing_cancel();
    if (remote == REMOTE_IDLE && routing.remote != REMOTE_IDLE)
        routing.idle_since = btstack_run_loop_get_time_ms();
    routing.remote = remote;
}

static void routing_match_source_reply(void) {
    if (!source_read.directed || !routing_peer_listed()) return;
    source_read.directed=false;
    if (!routing_source_fresh(btstack_run_loop_get_time_ms()) ||
        memcmp(source_read.host,routing.other,6) || (!source_read.type && foreign_call)) return;
    routing_remote_changed(source_read.type ? REMOTE_ACTIVE : REMOTE_IDLE);
}

static void routing_packet(const uint8_t *packet, uint16_t size) {
    uint8_t local[6], other[6], source[6], type;
    bool owned, sole;
    gap_local_bd_addr(local);
    if (aacp_route_ownership(packet, size, &owned)) {
        route_event('o',owned,0);
        routing.known = true;
        if (owned) {
            source_read.awaiting=source_read.directed=source_read.busy_seen=false;
            if (routing.pending) routing.confirmed = routing.stage != 0;
            else if (!routing.relinquished) routing_authority(true);
        } else {
            // Current owner reports can lag the explicit OWN/MEDIA/HIJACK sequence.
            if (routing.pending && !aacp_takeover_pending(routing.peer)) routing_cancel();
            routing.confirmed = false;
            routing.relinquished = true;
            routing_authority(false);
        }
    } else if (aacp_route_devices(packet, size, local, other, &sole)) {
        if(sole)foreign_call=false;
        bool changed = memcmp(other, routing.other, 6) != 0;
        bool peer_left = sole && routing_peer_listed() &&
            (routing.remote == REMOTE_ACTIVE || routing.remote == REMOTE_IDLE || routing.remote == REMOTE_HANDOFF);
        bool sole_claim = sole && routing.pending && routing.stage;
        if (changed && routing.have_other && routing.pending) {
            if (!sole_claim && !aacp_audio_owned()) routing.relinquished = true;
            routing_cancel(); // Drop any packet still addressed to the vanished/replaced host.
            if (sole_claim) {
                routing.pending = routing.confirmed = true;
                routing.stage = 3; // The transmitted OWN now has positive exclusive-host evidence.
            }
        }
        memcpy(routing.other, other, 6);
        routing.have_other = aacp_route_address_valid(other);
        bool local_member = sole || packet[8] == 2;
        route_event('d',packet[8],local_member);
        // A controls-only dongle need not be listed as an audio host. In that
        // case only an observed source transition on this named peer can qualify idle.
        routing.topology = sole ? TOPO_LOCAL : packet[8] == 2 ? TOPO_BOTH :
            packet[8] == 1 && routing.have_other ? TOPO_FOREIGN_ONLY : TOPO_UNKNOWN;
        if (peer_left || (sole && routing.relinquished && routing.remote == REMOTE_UNKNOWN))
            routing_remote_changed(REMOTE_IDLE);
        else if (changed || routing.topology==TOPO_UNKNOWN) routing_remote_changed(REMOTE_UNKNOWN);
        if (sole && !routing.relinquished && !routing.pending) routing_authority(true);
        if (sole && routing.pending && routing.stage) routing.confirmed = true;
        if (sole || routing.topology==TOPO_UNKNOWN) source_read.directed=false;
        routing_match_source_reply();
    } else if (aacp_route_source(packet, size, source, &type)) {
        bool from_other = routing_peer_listed() && !memcmp(source, routing.other, 6);
        bool from_local = !memcmp(source, local, 6);
        bool anonymous = !memcmp(source, (uint8_t[6]){0}, 6);
        uint8_t source_class=anonymous ? SOURCE_ANONYMOUS : from_local ? SOURCE_LOCAL :
            from_other ? SOURCE_PEER : aacp_route_address_valid(source) ? SOURCE_OTHER : SOURCE_INVALID;
        route_event('s',type,source_class);
        bool fresh=routing_source_fresh(btstack_run_loop_get_time_ms());
        if (!type && fresh && source_read.busy_seen) return; // A late reply cannot end a newer call or stream.
        if (fresh && type) {
            // A 0D snapshot of the same phone's already-active music does not start a new stream.
            bool confirms_media=source_read.awaiting && source_read.count && type==2 && from_other &&
                routing.remote==REMOTE_ACTIVE && !foreign_call;
            if (!confirms_media) source_read.busy_seen=true;
        }
        source_read.directed=false;
        if (fresh && source_read.awaiting) {
            source_read.awaiting=false;
            if (source_read.count) source_read.reply_class=source_class;
            if ((type || source_read.count) &&
                (from_other || (!from_local && aacp_route_address_valid(source) && routing.topology==TOPO_UNKNOWN)) &&
                (!foreign_call || type || !memcmp(source,routing.call_host,6))) {
                source_read.directed=true; source_read.type=type;
                memcpy(source_read.host,source,6);
            }
        }
        if (type == 1 && !from_local) {
            foreign_call = true;
            memcpy(routing.call_host,source,6);
        } else if (foreign_call) {
            bool same_caller = !memcmp(source,routing.call_host,6);
            bool anonymous_end = anonymous && !type &&
                (!routing_peer_listed() || !memcmp(routing.other,routing.call_host,6));
            if (!same_caller && !anonymous_end && !(from_local && type)) return;
            foreign_call = false;
        }
        if (type && memcmp(source, local, 6)) {
            if (routing.topology==TOPO_LOCAL) routing.topology=TOPO_UNKNOWN;
            routing_remote_changed(from_other ? REMOTE_ACTIVE : REMOTE_UNKNOWN);
        }
        else if (type == 0 && routing_peer_listed() &&
                 ((from_other && (routing.remote == REMOTE_ACTIVE || (routing.topology==TOPO_BOTH &&
                                (routing.remote == REMOTE_UNKNOWN || routing.remote == REMOTE_HANDOFF)))) ||
                  (anonymous && routing.remote == REMOTE_ACTIVE)))
            routing_remote_changed(REMOTE_IDLE);
        else if (type && routing.remote != REMOTE_RETURNING)
            routing_remote_changed(REMOTE_UNKNOWN);
        if(foreign_call) {
            if(!memcmp(routing.peer,aacp_addr,6)) {
                routing.known=routing.relinquished=true;
                routing_cancel();
            }
            routing_authority(false);
            return;
        }
        routing.known = true;
        routing_match_source_reply();
        if(!type)return;
        if (!memcmp(source, local, 6)) {
            if (routing.pending) routing.confirmed = routing.stage != 0;
            else if (!routing.relinquished) routing_authority(true);
        } else {
            if (routing.pending && !aacp_takeover_pending(routing.peer)) routing_cancel();
            routing.confirmed = false;
            routing.relinquished = true;
            routing_authority(false);
        }
    } else if (routing.have_other && aacp_route_hijack(packet, size, routing.other)) {
        source_read.awaiting=source_read.directed=false;
        source_read.busy_seen=true;
        routing.known = routing.relinquished = true;
        routing_cancel(); // The other host's explicit newer intent wins; do not fight it.
        routing_remote_changed(routing_peer_listed() ? REMOTE_HANDOFF : REMOTE_UNKNOWN);
        routing_authority(false);
    } else if (aacp_route_header(packet, size, 0x2e)) {
        source_read.awaiting=source_read.directed=false;
        route_event('x',size>=9 ? packet[8] : 255,0);
        routing.topology = TOPO_UNKNOWN;
        routing_remote_changed(REMOTE_UNKNOWN);
    }
}

void aacp_routing_report(void) {
    printf("[ROUTING] known=%u owned=%u yielded=%u other=%u pending=%u stage=%u queued=%u confirmed=%u age_ms=%lu remote=%u returns=%lu\n",
        routing.known, aacp_audio_owned(), routing.relinquished, routing.have_other,
        routing.pending, routing.stage, routing.queued, routing.confirmed,
        (unsigned long)(routing.pending ? btstack_run_loop_get_time_ms() - routing.since : 0),
        (unsigned)routing.remote, (unsigned long)routing.automatic_returns);
}

uint32_t aacp_control_session(void) { return control_session; }
void aacp_route_history_report(void) {
    printf("[ROUTE] epoch=%lu top=%u remote=%u call=%u pending=%u owned=%u returns=%lu sub=%u reads=%lu read_age=%lu reply=%u seq=%lu hist=",
        (unsigned long)control_session,(unsigned)routing.topology,(unsigned)routing.remote,foreign_call,
        routing.pending,aacp_audio_owned(),(unsigned long)routing.automatic_returns,source_read.subscribed,(unsigned long)source_read.count,
        (unsigned long)(source_read.count ? btstack_run_loop_get_time_ms()-source_read.sent : 0),
        source_read.reply_class,(unsigned long)route_sequence);
    for (unsigned i=0;i<route_event_count;i++) {
        const struct route_event *event=&route_events[(route_sequence-route_event_count+i)%8u];
        printf("%s%lu:%c:%u:%u",i?",":"",(unsigned long)event->ms,event->kind,event->a,event->b);
    }
    printf("\n");
}
bool aacp_control_ready(void) { return aacp_connected && aacp_got_control && aacp_init_sent; }
bool aacp_secure_control_peer(const uint8_t addr[6]) {
    if (!aacp_control_ready() || !addr || memcmp(addr,aacp_addr,6)) return false;
    const hci_connection_t *acl = hci_connection_for_bd_addr_and_type(aacp_addr,BD_ADDR_TYPE_ACL);
    return acl && acl->state==OPEN && gap_security_level(acl->con_handle)>=LEVEL_2;
}
bool aacp_control_address(bd_addr_t address) {
    if (!address || !aacp_control_ready()) return false;
    memcpy(address, aacp_addr, sizeof(bd_addr_t));
    return true;
}
const dongle_readback_t *aacp_control_readback(void) { return &control_readback; }
bool aacp_settings_model_known(void) {return settings.profile && settings.firmware_major;}
bool aacp_model_known(void) { return settings.profile && settings.firmware_major; }
bool aacp_model_game_supported(void) {
    return settings.profile && settings.profile->silent_allow_off && settings.firmware_major>=8;
}
uint32_t aacp_receive_buffer_serial(void) { return receive_buffer_serial; }
uint16_t aacp_receive_buffer_last(void) { return receive_buffer_last; }
uint8_t aacp_receive_buffer(uint16_t milliseconds) {
    if (milliseconds!=60 && milliseconds!=150) return DONGLE_MALFORMED;
    if (!aacp_control_ready()) return DONGLE_DISCONNECTED;
    if (!aacp_audio_owned()) return DONGLE_BUSY;
    if (!aacp_model_game_supported()) return DONGLE_UNSUPPORTED;
    if ((milliseconds==60 && (aacp_mic_requested() || aacp_mic_active())) ||
        aacp_tx_count>=AACP_TX_QUEUE_LEN-2) return DONGLE_BUSY;
    for (unsigned i=0;i<aacp_tx_count;++i)
        if (aacp_tx_queue[(aacp_tx_head+i)%AACP_TX_QUEUE_LEN].data==receive_buffer_tx) return DONGLE_BUSY;
    if (l2cap_get_remote_mtu_for_local_cid(aacp_cid)<sizeof(receive_buffer_tx)) return DONGLE_UNSUPPORTED;
    memcpy(receive_buffer_tx,AACP_CONTROL_COMMAND_HEADER,AACP_CONTROL_COMMAND_HEADER_SIZE);
    receive_buffer_tx[AACP_CONTROL_COMMAND_OFFSET_IDENTIFIER]=AACP_CONTROL_RECEIVE_BUFFER;
    dongle_put32(receive_buffer_tx+AACP_CONTROL_COMMAND_OFFSET_VALUE,milliseconds);
    return aacp_tx_enqueue(receive_buffer_tx,sizeof(receive_buffer_tx)) ? DONGLE_ACCEPTED : DONGLE_BUSY;
}
bool aacp_pair_irk(uint8_t address[6], uint8_t key[16]) {
    if (!pair_irk_known || !address || !key) return false;
    memcpy(address,pair_key_address,6); memcpy(key,pair_irk,16); return true;
}
bool aacp_pair_case_key(uint8_t address[6], uint8_t key[16]) {
    if (!pair_irk_known || !pair_case_supported || !address || !key) return false;
    memcpy(address,pair_key_address,6);
    memcpy(key,pair_advertising_key,16);
    return true;
}
static void pair_keys_report(const uint8_t *p, uint16_t n) {
    pair_irk_known=false;
    memset(pair_irk,0,sizeof(pair_irk));
    memset(pair_advertising_key,0,sizeof(pair_advertising_key));
    if (n!=47 || p[5] || p[6]!=2) return;
    const uint8_t *identity=NULL, *advertising=NULL;
    unsigned kinds=0;
    for (unsigned off=7;off<n;off+=20) {
        uint8_t id=p[off];
        if ((id!=1 && id!=4) || (kinds&(1u<<id)) || p[off+1] || p[off+2]!=16 || p[off+3]) return;
        bool nonzero=false;
        for (unsigned i=0;i<16;++i) nonzero|=p[off+4+i]!=0;
        if (!nonzero) return;
        kinds|=1u<<id;
        if (id==1) identity=p+off+4;
        else advertising=p+off+4;
    }
    if (!identity || kinds!=((1u<<1)|(1u<<4))) return;
    memcpy(pair_irk,identity,16);
    memcpy(pair_advertising_key,advertising,16);
    memcpy(pair_key_address,aacp_addr,6); pair_irk_known=true;
}
uint8_t aacp_setting_write(uint8_t id, uint32_t value) {
    if (setting_index(id)<0) return DONGLE_UNSUPPORTED;
    if (!setting_value_valid(id,value)) return DONGLE_MALFORMED;
    if (!aacp_control_ready()) return DONGLE_DISCONNECTED;
    if (!settings_supported(&settings,id,value)) return DONGLE_UNSUPPORTED;
    if (setting_pending || setting_result || aacp_mic_requested() || aacp_mic_active() ||
        aacp_tx_count>=AACP_TX_QUEUE_LEN-2)
        return DONGLE_BUSY;
    if (l2cap_get_remote_mtu_for_local_cid(aacp_cid)<sizeof(setting_tx)) return DONGLE_UNSUPPORTED;
    memcpy(setting_tx,AACP_CONTROL_COMMAND_HEADER,AACP_CONTROL_COMMAND_HEADER_SIZE);
    setting_tx[AACP_CONTROL_COMMAND_OFFSET_IDENTIFIER]=id; dongle_put32(setting_tx+AACP_CONTROL_COMMAND_OFFSET_VALUE,value);
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
    return settings_snapshot(&settings,btstack_run_loop_get_time_ms(),out,capacity,false);
}
uint16_t aacp_settings_snapshot_extended(uint8_t *out, uint16_t capacity) {
    if (!aacp_control_ready()) return 0;
    return settings_snapshot(&settings,btstack_run_loop_get_time_ms(),out,capacity,true);
}
bool aacp_control_anc(uint8_t mode) {
    if (!aacp_control_ready() || !control_readback.anc ||
        (mode != DONGLE_LISTENING_MODE_ANC && mode != DONGLE_LISTENING_MODE_TRANSPARENCY)) return false;
    // Keep capacity for microphone STOP and recovery even if a host floods controls.
    for (unsigned i = 0; i < aacp_tx_count; ++i) {
        const uint8_t *p = aacp_tx_queue[(aacp_tx_head + i) % AACP_TX_QUEUE_LEN].data;
        if (p == control_anc || p == control_transparency) return false;
    }
    if (aacp_tx_count >= AACP_TX_QUEUE_LEN - 2) return false;
    return aacp_tx_enqueue(mode == DONGLE_LISTENING_MODE_ANC ? control_anc : control_transparency, sizeof(control_anc));
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
    research_eq_revision = 0;
}
static bool eq_report_valid(const uint8_t *p, uint16_t n) {
    static const uint8_t prefix[]={AACP_HEADER_BYTES,AACP_OPCODE_EQUALIZER,0,5,0,1};
    return p && n==13 && !memcmp(p,prefix,sizeof(prefix)) && (p[9]==1 || p[9]==2) &&
        p[10]<=100 && p[11]<=100 && p[12]<=100;
}
static bool research_valid(const uint8_t *p, uint16_t n) {
    if (!p || n < 6 || n > sizeof(research_tx) || memcmp(p, AACP_HEADER, AACP_HEADER_SIZE) || p[5]) return false;
    if (p[AACP_OFFSET_OPCODE]==AACP_OPCODE_EQUALIZER)
        return eq_report_valid(p,n) && settings.profile && reports[5].length==sizeof(eq_report);
    if (p[AACP_OFFSET_OPCODE] == AACP_OPCODE_CONTROL_COMMAND && n == AACP_CONTROL_COMMAND_SIZE && p[9] == 0 && p[10] == 0) {
        uint8_t id = p[AACP_CONTROL_COMMAND_OFFSET_IDENTIFIER];
        if (id == AACP_CONTROL_HEARING_AID) return p[7] <= 2 && (p[8] == 1 || p[8] == 2);
        if (id == AACP_CONTROL_HEARING_ASSISTANCE_GATE) return (p[7] == 1 || p[7] == 2) && p[8] == 0;
        if (id == DONGLE_SETTING_LISTENING_MODE)
            return (p[7] == DONGLE_LISTENING_MODE_ANC || p[7] == DONGLE_LISTENING_MODE_TRANSPARENCY) && p[8] == 0;
    }
    if (p[AACP_OFFSET_OPCODE] == AACP_OPCODE_PME) return n == 140 && p[6] == 0x84 && p[7] == 0 && p[8] == 2 &&
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
    research_eq_revision = packet[AACP_OFFSET_OPCODE]==AACP_OPCODE_EQUALIZER ? reports[5].revision : 0;
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
    if (slot==DONGLE_CACHED_REPORT_SETTINGS_SNAPSHOT) return aacp_settings_snapshot(out,capacity);
    // The EQ report occupies the sixth raw cache entry; slot 6 remains the scalar snapshot.
    if (slot==DONGLE_CACHED_REPORT_EQUALIZER) slot=DONGLE_CACHED_REPORT_SETTINGS_SNAPSHOT;
    if (!aacp_control_ready() || slot < 1 || slot > sizeof(reports) / sizeof(reports[0]) || !out) return 0;
    cached_report_t *report = &reports[slot - 1];
    if (!report->length || capacity < report->length + DONGLE_CACHED_REPORT_ENVELOPE_SIZE) return 0;
    dongle_put32(out, btstack_run_loop_get_time_ms() - report->updated);
    dongle_put32(out + 4, report->revision);
    memcpy(out + DONGLE_CACHED_REPORT_ENVELOPE_SIZE, report->packet, report->length);
    return report->length + DONGLE_CACHED_REPORT_ENVELOPE_SIZE;
}
static void research_cache(const uint8_t *p, uint16_t n) {
    int slot = -1;
    // Raw cache index is the cached-report slot minus one.
    uint8_t opcode = p[AACP_OFFSET_OPCODE];
    if (opcode == AACP_OPCODE_DEVICE_METADATA) slot = DONGLE_CACHED_REPORT_MODEL_METADATA - 1;
    else if (opcode == AACP_OPCODE_CONTROL_COMMAND && n >= 7 && p[AACP_CONTROL_COMMAND_OFFSET_IDENTIFIER] == AACP_CONTROL_HEARING_AID)
        slot = DONGLE_CACHED_REPORT_HEARING_AID - 1;
    else if (opcode == AACP_OPCODE_CONTROL_COMMAND && n >= 7 && p[AACP_CONTROL_COMMAND_OFFSET_IDENTIFIER] == AACP_CONTROL_HEARING_ASSISTANCE_GATE)
        slot = DONGLE_CACHED_REPORT_HEARING_ASSISTANCE_GATE - 1;
    else if (opcode == AACP_OPCODE_PME) {
        if (n == 10 && p[6] == 2 && p[7] == 0 && p[8] == 1) return;
        slot = DONGLE_CACHED_REPORT_PME - 1;
    } else if (opcode == AACP_OPCODE_EAR_STATE) slot = DONGLE_CACHED_REPORT_EAR_STATE - 1;
    // Slot 6 is the generated scalar snapshot, so the equalizer report takes the sixth raw entry.
    else if (opcode == AACP_OPCODE_EQUALIZER) slot = DONGLE_CACHED_REPORT_EQUALIZER - 2;
    if (slot < 0) return;
    cached_report_t *report = &reports[slot];
    if (++report->revision == 0) ++report->revision;
    report->length = 0;
    if (n > report->capacity || (slot==5 && !eq_report_valid(p,n))) return;
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
    // A healthy channel starts a fresh recovery budget; incomplete handshakes do not.
    if (aacp_control_ready()) aacp_attempts = 0;
    control_reset();
    uint16_t cid=aacp_detach_channel();
    aacp_connected = aacp_got_control = aacp_init_sent = aacp_init_retried = false;
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
    if (aacp_mic_want && aacp_mic_running && aacp_mic_requested()) {
        mic_stats.last_sdu_ms = btstack_run_loop_get_time_ms();
        aacp_mic_gap_reset = false;
    }
}

static void aacp_mic_stats_report(btstack_timer_source_t *ts) {
    if (aacp_mic_want && !aacp_mic_requested()) {
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
            dongle_playback_mic_recovery_abort();
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
    if (!aacp_mic_requested() || !aacp_mic_want || aacp_mic_failed || aacp_mic_running ||
        aacp_mic_remote_active || aacp_mic_stopping || aacp_mic_restore_required ||
        !aacp_connected || !aacp_got_control || !aacp_init_sent ||
        aacp_mic_start_queued()) return;
    if (!dongle_playback_mic_ready()) {
        uint32_t now=btstack_run_loop_get_time_ms();
        if (!aacp_mic_playback_waiting) {
            aacp_mic_playback_waiting=true; aacp_mic_playback_since=now;
        } else if ((uint32_t)(now-aacp_mic_playback_since)>=3000u) {
            aacp_mic_failed=true;
            printf("[MIC] normal playback buffer not ready; close capture before retrying\n");
        }
        return;
    }
    aacp_mic_playback_waiting=false;
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
    aacp_mic_playback_waiting=false;
    aacp_mic_restarts = 0;
    aacp_drop_mic_commands();
    bool sent = aacp_mic_remote_active;
    aacp_mic_teardown();
    if (sent && !aacp_tx_enqueue(aacp_mic_stop_bytes, sizeof(aacp_mic_stop_bytes)))
        dongle_playback_mic_recovery_abort();
}

static void aacp_handle_can_send_now(void) {
    if (!aacp_connected || aacp_tx_count == 0) return;
    aacp_pkt_t p = aacp_tx_queue[aacp_tx_head];
    uint32_t now=btstack_run_loop_get_time_ms();
    bool resubscribe=source_read.queued && p.data==aacp_request_notifications;
    if ((p.data==aacp_request_source || resubscribe) && (routing.pending || aacp_audio_owned() ||
        (resubscribe ? !dongle_playback_auto_standby(aacp_addr) : !dongle_playback_return_signal(aacp_addr,now)))) {
        aacp_tx_head=(uint8_t)((aacp_tx_head+1u)%AACP_TX_QUEUE_LEN); --aacp_tx_count;
        source_read.queued=false;
        if (aacp_tx_count) l2cap_request_can_send_now_event(aacp_cid);
        return;
    }
    if (p.data==routing.tx && !aacp_takeover_pending(aacp_addr)) {
        bool pc_paused = routing.remote == REMOTE_RETURNING && now-routing.since<AACP_TAKEOVER_MS &&
            !dongle_playback_return_signal(aacp_addr,now);
        if (!aacp_audio_owned()) routing.relinquished=true;
        routing_cancel();
        if (pc_paused) routing_remote_changed(REMOTE_IDLE);
        if (aacp_tx_count) l2cap_request_can_send_now_event(aacp_cid);
        return;
    }
    // A late positive response can make the still-unsent fallback handshake unnecessary.
    if (p.data==aacp_handshake_legacy && aacp_got_control) {
        aacp_tx_head=(uint8_t)((aacp_tx_head+1u)%AACP_TX_QUEUE_LEN);
        --aacp_tx_count;
        if (aacp_tx_count) l2cap_request_can_send_now_event(aacp_cid);
        return;
    }
    if ((p.data==setting_tx || p.data==research_tx) &&
        (!dongle_rpc_transport_current() || aacp_mic_requested() || aacp_mic_active() ||
         (p.data==research_tx && research_tx[AACP_OFFSET_OPCODE]==AACP_OPCODE_EQUALIZER &&
          (research_eq_revision!=reports[5].revision || !research_valid(p.data,p.len))) ||
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
    if (p.data == aacp_mic_start_bytes && !aacp_mic_requested()) {
        aacp_mic_stop();
        return;
    }
    if (p.data == aacp_mic_start_bytes && (!aacp_mic_want || aacp_mic_failed)) {
        aacp_drop_mic_commands();
        return;
    }
    if (p.data == aacp_mic_start_bytes &&
        (aacp_mic_restore_required || !dongle_playback_mic_ready())) {
        // Retire only this stale Start; a queued Stop must retain its drain receipt.
        aacp_tx_head=(uint8_t)((aacp_tx_head+1u)%AACP_TX_QUEUE_LEN);
        --aacp_tx_count;
        aacp_mic_preparing=aacp_mic_start_waiting_for_reset=false;
        if (aacp_tx_count) l2cap_request_can_send_now_event(aacp_cid);
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
    if (resubscribe || p.data==aacp_request_source) {
        if (resubscribe) { source_read.subscribed=true;source_read.subscribed_at=now; }
        else { source_read.sent=now; ++source_read.count; source_read.reply_class=255; }
        source_read.awaiting=true;source_read.queued=source_read.busy_seen=source_read.directed=false;
    }
    // Pro 3 can leave Allow Off unreported; a sent choice is not a device report.
    if (p.data==setting_tx && setting_tx[AACP_CONTROL_COMMAND_OFFSET_IDENTIFIER]==DONGLE_SETTING_ALLOW_OFF &&
        settings.profile && settings.profile->silent_allow_off)
        settings.allow_off_requested=dongle_u32(setting_tx+AACP_CONTROL_COMMAND_OFFSET_VALUE)==DONGLE_SETTING_ALLOW_OFF_ON;
    if (p.data==setting_tx) {
        setting_pending=false; setting_result=1;
        dongle_playback_accessory_event(setting_tx,sizeof(setting_tx));
    }
    if (p.data==receive_buffer_tx) {
        receive_buffer_last=(uint16_t)dongle_u32(p.data+7);
        if (++receive_buffer_serial==0) ++receive_buffer_serial;
        dongle_playback_receive_buffer_sent(now);
    }
    if (p.data == routing.tx) {
        routing.queued = false;
        if (routing.tx_kind == AACP_ROUTE_OWN) {routing.stage = 1;routing.own_sent=now;}
        else if (routing.tx_kind == AACP_ROUTE_MEDIA) routing.stage = 2;
        else if (routing.tx_kind == AACP_ROUTE_HIJACK) routing.stage = 3;
    }
    aacp_tx_since_ms = btstack_run_loop_get_time_ms();
    if (p.data == research_tx) { research_pending = false; research_result = 1; }
    if (p.data==aacp_request_keys || (aacp_init_retried && p.data==aacp_request_capabilities))
        aacp_init_sent = true;
    if (p.data == aacp_mic_start_bytes) {
        aacp_mic_preparing = false;
        aacp_mic_running = true;
        aacp_mic_remote_active = true;
        aacp_mic_restore_required=true;
        aacp_mic_restore_started=aacp_mic_restore_complete=false;
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
        aacp_mic_restore_since=aacp_mic_stop_since_ms;
    }
    if (aacp_tx_count != 0) l2cap_request_can_send_now_event(aacp_cid);
    aacp_try_mic_start();
}

#if AIRPODS_HEAD_TRACKING
static bool head_motion_send(uint8_t *packet, uint16_t size) {
    return aacp_connected && aacp_cid && !aacp_tx_count &&
           size <= l2cap_get_remote_mtu_for_local_cid(aacp_cid) &&
           l2cap_can_send_packet_now(aacp_cid) &&
           l2cap_send(aacp_cid, packet, size) == ERROR_CODE_SUCCESS;
}
#endif

void aacp_poll(void) {
    // USB IRQs can revoke capture while the main loop still holds cached intent.
    if (aacp_mic_want && !aacp_mic_requested()) aacp_mic_stop();
    aacp_mic_update_progress();
    if (aacp_mic_start_waiting_for_reset && aacp_mic_dec_reset_done()) {
        aacp_mic_start_waiting_for_reset = false;
        if (aacp_cid != 0 && aacp_tx_count != 0)
            l2cap_request_can_send_now_event(aacp_cid);
    }
    uint32_t now = btstack_run_loop_get_time_ms();
    routing_poll(now);
    if (research_pending && (uint32_t)(now - research_since) >= 3000u) {
        aacp_research_cancel();
        research_result = 2;
    }
    if (setting_pending && (uint32_t)(now-setting_since)>=3000u) {
        aacp_setting_cancel();
        setting_result=2;
    }
    if (aacp_connected && (!aacp_got_control || !aacp_init_sent) &&
        (uint32_t)(now - aacp_channel_since_ms) >= AACP_CHANNEL_TIMEOUT_MS) {
        aacp_channel_failed();
        return;
    }
    if (aacp_tx_count != 0 &&
        (uint32_t)(now - aacp_tx_since_ms) >= AACP_CHANNEL_TIMEOUT_MS) {
        aacp_channel_failed();
        return;
    }
    // Never restart a healthy service or extend the existing channel deadline.
    if (aacp_session && aacp_connected && aacp_init_sent && !aacp_got_control &&
        !aacp_init_retried && (uint32_t)(now-aacp_channel_since_ms)>=AACP_INIT_RETRY_MS)
        (void)aacp_startup_enqueue(true);
    // A successful write is not proof the uplink stopped; drain before restarting.
    if (aacp_mic_stopping) {
        if ((uint32_t)(now - aacp_last_uplink_ms) >= AACP_MIC_QUIET_MS) {
            aacp_mic_stopping = aacp_mic_remote_active = false;
        } else if ((uint32_t)(now - aacp_mic_stop_since_ms) >= AACP_MIC_DRAIN_MS) {
            if (aacp_mic_stop_retries++ == 0) {
                aacp_mic_stop_since_ms = now;
                if (!aacp_tx_enqueue(aacp_mic_stop_bytes, sizeof(aacp_mic_stop_bytes)))
                    dongle_playback_mic_recovery_abort();
            } else {
                dongle_playback_mic_recovery_abort();
            }
            return;
        }
    }
    if (aacp_mic_restore_required && !aacp_mic_remote_active && !aacp_mic_stopping) {
        if (!aacp_audio_owned()) {
            aacp_mic_restore_required = aacp_mic_restore_started = aacp_mic_restore_complete = false;
            return; // The active host owns receiver preparation; never restart its output.
        }
        if (!aacp_mic_restore_started) {
            aacp_mic_restore_revision=dongle_playback_mic_recovery_revision();
            aacp_mic_restore_started=dongle_playback_mic_recover();
            if (!aacp_mic_restore_started && (uint32_t)(now-aacp_mic_restore_since)>=8000u) {
                dongle_playback_mic_recovery_abort();
                return;
            }
        } else if (dongle_playback_mic_recovery_revision()!=aacp_mic_restore_revision) {
            if (!aacp_mic_restore_complete) {
                aacp_mic_restore_complete=true;
                aacp_mic_restore_since=now;
            }
            if ((uint32_t)(now-aacp_mic_restore_since)>=AACP_MIC_QUIET_MS &&
                (uint32_t)(now-aacp_last_uplink_ms)>=AACP_MIC_QUIET_MS) {
                aacp_mic_restore_required=aacp_mic_restore_started=aacp_mic_restore_complete=false;
            } else if ((uint32_t)(now-aacp_mic_restore_since)>=AACP_MIC_DRAIN_MS) {
                dongle_playback_mic_recovery_abort();
                return;
            }
        }
    }
    aacp_try_mic_start();
#if AIRPODS_HEAD_TRACKING
    bool motion_allowed = aacp_control_ready() && aacp_audio_owned() &&
        settings.profile && settings.profile->models[0] == DONGLE_CASE_BATTERY_PROFILE_MODEL &&
        settings.firmware_major >= 8 && !aacp_mic_requested() && !aacp_mic_active();
    head_motion_poll(now, motion_allowed ? (dongle_playback_game_timing() ? 30 : 100) : 0,
                     head_motion_send);
#endif
}

static bool aacp_has_header(const uint8_t *sdu, uint16_t size) {
    return size >= 6 && sdu[0] == 4 && sdu[1] == 0 && sdu[2] == 4 && sdu[3] == 0;
}

static bool aacp_connect_accepted(const uint8_t *p,uint16_t n) {
    static const uint8_t prefix[]={1,0,4,0,0,0,1,0};
    return n==18 && !memcmp(p,prefix,sizeof(prefix)) && (p[8]==2 || p[8]==3) && !p[9];
}

// Known entry widths match Android Capability.kt; unknown IDs cannot prove service readiness.
static unsigned aacp_capability_width(uint8_t id) {
    if ((id>=3 && id<=8) || id==0x30) return 4;
    if (id==1 || id==2 || (id>=9 && id<=0x0d) || (id>=0x0f && id<=0x19) ||
        (id>=0x20 && id<=0x26) || id==0x28 || id==0x29 || (id>=0x31 && id<=0x38)) return 1;
    switch(id) {
        case 0x40: case 0x50: case 0x60: case 0x80: case 0x90: case 0xa0:
        case 0xb0: case 0xc0: case 0xd0: case 0xe0: case 0xf0: return 1;
        default: return 0;
    }
}
static bool aacp_capabilities_valid(const uint8_t *p,uint16_t n) {
    if (n<7 || n>327 || p[AACP_OFFSET_OPCODE]!=AACP_OPCODE_CAPABILITIES || p[5]) return false;
    uint8_t seen[32]={0};
    unsigned offset=7;
    for (unsigned i=0;i<p[6];++i) {
        if (offset>=n) return false;
        uint8_t id=p[offset++],bit=(uint8_t)(1u<<(id&7));
        unsigned width=aacp_capability_width(id);
        if (!width || (seen[id/8]&bit) || width>n-offset) return false;
        seen[id/8]|=bit;offset+=width;
    }
    return offset==n;
}
static bool aacp_legacy_control_valid(const uint8_t *p,uint16_t n) {
    if (p[5]) return false;
    if (p[AACP_OFFSET_OPCODE]==AACP_OPCODE_CONTROL_COMMAND && n==AACP_CONTROL_COMMAND_SIZE) {
        bool owned;
        uint8_t id=p[AACP_CONTROL_COMMAND_OFFSET_IDENTIFIER];
        uint32_t value=dongle_u32(p+AACP_CONTROL_COMMAND_OFFSET_VALUE);
        // Received upper bytes are previous actions, as in settings_packet.
        if (id==DONGLE_SETTING_HOLD_ACTIONS) value&=DONGLE_SETTING_HOLD_ACTIONS_REPORT_MASK;
        return aacp_route_ownership(p,n,&owned) ||
            (setting_index(id)>=0 && setting_value_valid(id,value));
    }
    if (p[AACP_OFFSET_OPCODE]!=AACP_OPCODE_BATTERY_REPORT || n<7) return false;
    uint8_t count=p[AACP_BATTERY_OFFSET_COUNT];
    if (!count || count>3 || n!=AACP_BATTERY_OFFSET_ENTRIES+AACP_BATTERY_ENTRY_SIZE*count) return false;
    unsigned seen=0;
    for (unsigned i=0;i<count;++i) {
        const uint8_t *battery=p+AACP_BATTERY_OFFSET_ENTRIES+AACP_BATTERY_ENTRY_SIZE*i;
        uint8_t component=battery[AACP_BATTERY_ENTRY_OFFSET_COMPONENT], level=battery[AACP_BATTERY_ENTRY_OFFSET_LEVEL_PERCENT];
        unsigned bit=component==AACP_BATTERY_COMPONENT_LEFT ? DONGLE_COMPONENT_BIT_LEFT :
                     component==AACP_BATTERY_COMPONENT_RIGHT ? DONGLE_COMPONENT_BIT_RIGHT :
                     component==AACP_BATTERY_COMPONENT_CASE ? DONGLE_COMPONENT_BIT_CASE : 0u;
        if (!bit || (seen&bit) || (level>100 && level!=AACP_BATTERY_LEVEL_UNKNOWN)) return false;
        seen|=bit;
    }
    return true;
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
    if (!aacp_mic_requested() && aacp_mic_want) aacp_mic_stop();
    if (!aacp_mic_requested() || !aacp_mic_want || !aacp_mic_running) {
        aacp_last_uplink_ms = now;
        if (!aacp_mic_want && !aacp_mic_remote_active && !aacp_mic_stopping &&
            !aacp_mic_restore_required) {
            aacp_mic_remote_active = true;
            aacp_mic_restore_required=true;
            aacp_mic_restore_started=aacp_mic_restore_complete=false;
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
        if (!aacp_mic_requested()) {
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
        if (!aacp_session || !aacp_connected || channel != aacp_cid) return;
        if (aacp_connect_accepted(packet,size)) {
            aacp_got_control=true;
            aacp_try_mic_start();
            return;
        }
#if AIRPODS_HEAD_TRACKING
        if (aacp_audio_owned() && !aacp_mic_requested() && !aacp_mic_active() &&
            head_motion_receive(packet, size, btstack_run_loop_get_time_ms())) return;
        if (size == 10 && aacp_has_header(packet, size) && packet[4] == 8 && !packet[5])
            head_motion_role(packet + 6);
#endif
        if (!aacp_has_header(packet,size)) return;
        // All microphone messages, including malformed ones, stay out of control readiness.
        if (little_endian_read_16(packet, 4) == 0x58) {
            aacp_handle_audio_sdu(packet, size);
        } else {
            routing_packet(packet, size);
            settings_packet(&settings,packet,size,btstack_run_loop_get_time_ms());
            uint8_t opcode=packet[AACP_OFFSET_OPCODE];
            if (opcode==AACP_OPCODE_DEVICE_METADATA)
                pair_case_supported=!packet[5] && settings.profile &&
                    settings.profile->models[0]==DONGLE_CASE_BATTERY_PROFILE_MODEL && settings.firmware_major;
            if (opcode==AACP_OPCODE_KEY_REPORT) pair_keys_report(packet,size);
            if (size>=7 && opcode==AACP_OPCODE_CONTROL_COMMAND && packet[5]==0 &&
                packet[AACP_CONTROL_COMMAND_OFFSET_IDENTIFIER]==DONGLE_SETTING_LISTENING_MODE) {
                const setting_report_t *anc=&settings.reports[setting_index(DONGLE_SETTING_LISTENING_MODE)];
                control_readback.anc=anc->known ? (uint8_t)anc->value : 0;
            }
            research_cache(packet, size);
            dongle_readback_packet(&control_readback, packet, size, dongle_uptime_ms());
            if((settings.profile && settings.firmware_major) ||
               (size==AACP_CONTROL_COMMAND_SIZE && opcode==AACP_OPCODE_CONTROL_COMMAND))
                dongle_playback_accessory_event(packet,size);
            if (aacp_capabilities_valid(packet,size) || aacp_legacy_control_valid(packet,size) ||
                (opcode==AACP_OPCODE_DEVICE_METADATA && !packet[5] && settings.profile && settings.firmware_major))
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
            if (cid && cid==retiring_aacp_cid) {
                if (memcmp(addr,retiring_aacp_peer,6)) return;
                route_event('c',2,status);
                retiring_aacp_cid=0;
                if (status==ERROR_CODE_SUCCESS) l2cap_disconnect(cid);
                aacp_schedule_retry();
                return;
            }
            if (aacp_cid && cid==aacp_cid && memcmp(addr,aacp_addr,6)) return;
            if (!aacp_session || aacp_cid == 0 || cid != aacp_cid) {
                if (status == ERROR_CODE_SUCCESS) l2cap_disconnect(cid);
                return;
            }
            route_event('c',1,status);
            if (status != ERROR_CODE_SUCCESS) {
                aacp_cid=0; // BTstack frees failed channels immediately after this callback.
                aacp_channel_failed();
                return;
            }
            aacp_connected = true;
            aacp_got_control = aacp_init_sent = aacp_init_retried = false;
            aacp_channel_since_ms = btstack_run_loop_get_time_ms();
            if (!aacp_startup_enqueue(false)) aacp_channel_failed();
            break;
        }
        case L2CAP_EVENT_CAN_SEND_NOW:
            if (size >= 4 && aacp_session && aacp_connected &&
                l2cap_event_can_send_now_get_local_cid(packet) == aacp_cid)
                aacp_handle_can_send_now();
            break;
        case L2CAP_EVENT_CHANNEL_CLOSED:
            if (size<4) break;
            if (retiring_aacp_cid && l2cap_event_channel_closed_get_local_cid(packet)==retiring_aacp_cid) {
                retiring_aacp_cid=0;
                aacp_schedule_retry();
            } else if (aacp_session && aacp_cid && l2cap_event_channel_closed_get_local_cid(packet)==aacp_cid) {
                aacp_cid=0; // This channel is already closed.
                aacp_channel_failed();
            }
            break;
        default:
            break;
    }
}
