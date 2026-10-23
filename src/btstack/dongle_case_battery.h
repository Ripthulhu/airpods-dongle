// SPDX-License-Identifier: GPL-3.0-only
#ifndef DONGLE_CASE_BATTERY_H
#define DONGLE_CASE_BATTERY_H

#include "../tinyusb/dongle_protocol.h"

typedef struct {
    uint8_t battery[3], known, present, live, charging;
    uint8_t lid, lid_counter;
    uint16_t variant;
    uint32_t counter;
    uint64_t updated[3], report_at;
} dongle_case_battery_snapshot_t;

// Core 0 under the Bluetooth lock. Association is a keyed layout check, not
// authenticated identity. Never use it for finder, pairing or audio ownership.
void dongle_case_battery_poll(void);
bool dongle_case_battery_snapshot(dongle_case_battery_snapshot_t *out);
// Merge into a caller-owned copy; AACP readback and its session remain untouched.
void dongle_case_battery_merge(dongle_readback_t *out, bool aacp_healthy,
                              uint8_t *present, uint8_t *case_source);

#endif
