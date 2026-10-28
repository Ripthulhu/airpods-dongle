// SPDX-License-Identifier: GPL-3.0-only
#ifndef BTSTACK_BLE_SCAN_H
#define BTSTACK_BLE_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#define BLE_SCAN_CACHE_SIZE 8u
#define BLE_SCAN_DATA_MAX 31u
#define BLE_SCAN_PAUSE_MIC (1u << 0)
#define BLE_SCAN_PAUSE_INQUIRY (1u << 1)
#define BLE_SCAN_PAUSE_USB_SUSPEND (1u << 2)
#define BLE_SCAN_PAUSE_DIAGNOSTIC (1u << 3)

typedef struct {
    uint64_t received_ms;  // Extended Bluetooth uptime, shared with battery readback.
    uint32_t reports;
    uint8_t address[6];
    uint8_t address_type;
    uint8_t event_type;
    int8_t rssi;
    bool rssi_valid;
    uint8_t data_length;
    uint8_t data[BLE_SCAN_DATA_MAX];
} ble_scan_report_t;

typedef struct {
    bool controller_ready;
    bool le_supported;
    bool scan_requested;  // gap_start_scan queues a command; this is not confirmation.
    uint32_t pause_reasons;
    uint32_t accepted;
    uint32_t malformed;
    uint32_t ignored;
    uint32_t evicted;
    uint8_t cache_count;
} ble_scan_stats_t;

typedef struct {
    ble_scan_stats_t stats;
    ble_scan_report_t reports[BLE_SCAN_CACHE_SIZE];
} ble_scan_snapshot_t;

// Call these from the BTstack context or while holding its async-context lock.
// The unconditional main-loop case poll observes every 32-bit clock wrap.
uint64_t dongle_uptime_ms(void);
void ble_scan_init(void);
void ble_scan_controller_ready(bool ready);
void ble_scan_set_paused(uint32_t reason, bool paused);
// Temporary A/B test only; automatically resumes within two minutes.
void ble_scan_diagnostic_pause(bool paused);
void ble_scan_get_stats(ble_scan_stats_t *stats);
void ble_scan_snapshot(ble_scan_snapshot_t *snapshot);
// Borrow each cache entry under the Bluetooth lock; avoid copying the whole cache.
void ble_scan_visit(void (*visit)(const ble_scan_report_t *,unsigned,void *),void *context);
bool ble_scan_report_has_type(const ble_scan_report_t *report, uint8_t type);

#endif
