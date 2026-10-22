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
#include "btstack.h"
#include "../tinyusb/dongle_protocol.h"

// One-time setup. Call once from btstack_main() after l2cap_init().
void aacp_init(void);

// Initiate the AACP L2CAP connection (PSM 0x1001) to the given AirPods address.
// Call once the A2DP signaling connection to the same device is established.
// A short settle delay is applied internally before the outgoing connect.
void aacp_connect(bd_addr_t addr);

// Tear down the AACP channel if open.
void aacp_disconnect(void);

// Service deferred capture and bounded channel deadlines from the main loop,
// with the same async-context lock used for START/STOP.
void aacp_poll(void);

// True once the channel is open (handshake may still be in flight).
bool aacp_is_connected(void);

// --- Hi-res mic stream ---

// Send the mic START / STOP byte sequences. If the channel is not open yet,
// START is remembered and fired once it is. START also arms the per-second
// RX statistics report; STOP stops it.
void aacp_mic_start(void);
void aacp_mic_stop(void);

// True after START is sent or unsolicited audio arrives, until quiet after STOP/close.
bool aacp_mic_active(void);

// Bounded host controls; readback is separate from command acceptance.
uint32_t aacp_control_session(void);
bool aacp_control_ready(void);
const dongle_readback_t *aacp_control_readback(void);
bool aacp_control_anc(uint8_t mode);
bool aacp_control_address(bd_addr_t address);

// Bounded settings transport; completion means sent locally, not device readback.
bool aacp_research_send(const uint8_t *packet, uint16_t length);
int aacp_research_take(void); // 0 waiting/idle, 1 sent, 2 failed
void aacp_research_cancel(void);
// Slots: model, hearing gate, assist gate, PME configuration, ear status.
// Returns little-endian u32 age (ms), u32 receipt revision, then the raw report.
// Revision changes even for identical reports and resets with the AACP session.
uint16_t aacp_research_cached(uint8_t slot, uint8_t *out, uint16_t capacity);

#endif // BTSTACK_AACP_H
