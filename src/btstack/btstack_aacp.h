// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 han-um
// Derived from librepods (https://github.com/librepods-org/librepods), GPL-3.0.
//
// AACP (Apple Accessory Protocol) channel for the AirPods hi-res mic:
// establishes L2CAP PSM 0x1001, runs the init handshake, START/STOPs the
// mic stream, and demuxes type-0x58 audio SDUs into AAC-ELD access units.
//
// Ported from librepods (linux-rust branch) aacp.rs / aacp_audio.rs.
//

#ifndef BTSTACK_AACP_H
#define BTSTACK_AACP_H

#include <stdint.h>
#include <stdbool.h>
#include "bluetooth.h"
#include "../tinyusb/dongle_protocol.h"

// One-time setup. Call once from btstack_main() after l2cap_init().
void aacp_init(void);

// Initiate the AACP L2CAP connection (PSM 0x1001) to the given AirPods address.
// Call once the A2DP signaling connection to the same device is established.
// A short settle delay is applied internally before the outgoing connect.
void aacp_connect(bd_addr_t addr);

// Tear down the AACP channel if open.
void aacp_disconnect(void);
// A retiring media link must retain a fresh, unsent button request for its replacement.
void aacp_media_disconnected(void);

// Service deferred capture and bounded channel deadlines from the main loop,
// with the same async-context lock used for START/STOP.
void aacp_poll(void);

// True once the channel is open (handshake may still be in flight).
bool aacp_is_connected(void);

// Audio authority is separate from the retained control connection. Unknown or
// relinquished ownership cannot start media or change physical volume.
bool aacp_audio_owned(void);
// Explicit button/app Connect intent, bound to the selected saved AirPods peer.
// Bounded transmission is not a remote acknowledgment; repeated presses coalesce.
bool aacp_takeover_request(const uint8_t peer[6]);
// Outgoing media connections require a current, peer-bound user request.
bool aacp_takeover_pending(const uint8_t peer[6]);
typedef struct {
    uint32_t revision, age_ms, nonce;
    uint8_t flags, stage, action;
} aacp_routing_state_t;
aacp_routing_state_t aacp_routing_state(void);
// Manual choices retire automatic receipts even when the takeover itself coalesces.
void aacp_routing_override(void);
void aacp_routing_receipt(uint32_t nonce, uint8_t action);
void aacp_routing_report(void);
// Cancel only unsent receiver-buffer changes when another host owns audio.
void aacp_receive_buffer_cancel(void);

// --- Hi-res mic stream ---
typedef enum { AACP_MIC_NONE, AACP_MIC_USB, AACP_MIC_CALL } aacp_mic_owner_t;
bool aacp_mic_requested(void);
// Main-loop ownership transition; a fresh USB revision also invalidates old PCM.
bool aacp_mic_update_owner(bool fresh_usb_request);
void aacp_mic_call_changed(void);
uint32_t aacp_mic_usb_read(int16_t *pcm, uint32_t frames);
// Main-loop call consumer; USB reads may interrupt and are serialized here.
uint32_t aacp_mic_call_read(int16_t *pcm, uint32_t frames);

// Send the mic START / STOP byte sequences. If the channel is not open yet,
// START is remembered and fired once it is. START also arms the per-second
// RX statistics report; STOP stops it.
void aacp_mic_start(void);
void aacp_mic_stop(void);

// True through uplink drain and confirmed playback repair after an attempted START.
bool aacp_mic_active(void);
// Physical uplink/drain only; the broader active guard also owns playback cleanup.
bool aacp_mic_uplink_active(void);
// Playback repair failure requires a fresh USB capture request, never an automatic loop.
void aacp_mic_recovery_failed(void);

// Bounded host controls; readback is separate from command acceptance.
uint32_t aacp_control_session(void);
bool aacp_control_ready(void);
const dongle_readback_t *aacp_control_readback(void);
bool aacp_settings_model_known(void);
bool aacp_control_anc(uint8_t mode);
bool aacp_control_address(bd_addr_t address);

// Typed model/value-checked controls. Acceptance is local, never a device report.
uint8_t aacp_setting_write(uint8_t id, uint32_t value);
// The RPC owner retains the operation until sent/failed, then consumes its receipt.
int aacp_setting_take(void); // 0 pending/idle, 1 sent locally, 2 failed
void aacp_setting_cancel(void); // Remove only its unsent queue entry and clear receipt.
// Version 1, count, then {id:u8, age_ms:u32, revision:u32, value:u32} records.
uint16_t aacp_settings_snapshot(uint8_t *out, uint16_t capacity);
uint16_t aacp_settings_snapshot_extended(uint8_t *out, uint16_t capacity);
bool aacp_model_game_supported(void);
bool aacp_model_known(void);
// Only the stream-owned 60/150 ms policy is allowed; not a general buffer knob.
uint8_t aacp_receive_buffer(uint16_t milliseconds);
uint32_t aacp_receive_buffer_serial(void);
uint16_t aacp_receive_buffer_last(void);
// Stream-owner callback at the actual send, before any new delay report can arrive.
void dongle_playback_receive_buffer_sent(uint32_t now);
// Pair-bound key retained through same-peer radio loss, never a host-supplied key.
bool aacp_pair_irk(uint8_t address[6], uint8_t key[16]);
// Pro 3 only; private advertising key, retained across same-peer AACP loss.
bool aacp_pair_case_key(uint8_t address[6], uint8_t key[16]);

// Bounded settings transport; completion means sent locally, not device readback.
bool aacp_research_send(const uint8_t *packet, uint16_t length);
int aacp_research_take(void); // 0 waiting/idle, 1 sent, 2 failed
void aacp_research_cancel(void);
// Slots: model, hearing gate, assist gate, PME configuration, ear status, settings, EQ.
// Returns little-endian u32 age (ms), u32 receipt revision, then the raw report.
// Revision changes even for identical reports and resets with the AACP session.
uint16_t aacp_research_cached(uint8_t slot, uint8_t *out, uint16_t capacity);

#endif // BTSTACK_AACP_H
