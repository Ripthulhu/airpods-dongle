// SPDX-License-Identifier: GPL-3.0-only
#include <string.h>

#include "btstack.h"
#include "btstack_ble_scan.h"

static btstack_packet_callback_registration_t scan_registration;
static ble_scan_snapshot_t scan_state;
static bool scan_initialized;

static void scan_update(void) {
    bool wanted = scan_state.stats.controller_ready && scan_state.stats.le_supported && scan_state.stats.pause_reasons == 0;
    if (wanted == scan_state.stats.scan_requested) return;
    scan_state.stats.scan_requested = wanted;
    if (wanted) {
        // Provisional 10% passive duty cycle; no scan requests or audio ownership.
        gap_set_scan_params(0, 480, 48, 0);
        gap_set_scan_duplicate_filter(false);
        gap_start_scan();
    } else {
        gap_stop_scan();
    }
}

void ble_scan_controller_ready(bool ready) {
    if (!scan_initialized) return;
    if (!ready && scan_state.stats.controller_ready) {
        scan_state.stats.cache_count = 0;
        memset(scan_state.reports, 0, sizeof(scan_state.reports));
    }
    scan_state.stats.controller_ready = ready;
    scan_state.stats.le_supported = ready && hci_le_supported();
    scan_update();
}

void ble_scan_set_paused(uint32_t reason, bool paused) {
    const uint32_t known = BLE_SCAN_PAUSE_MIC | BLE_SCAN_PAUSE_INQUIRY | BLE_SCAN_PAUSE_USB_SUSPEND;
    reason &= known;
    if (!scan_initialized || reason == 0) return;
    if (paused) scan_state.stats.pause_reasons |= reason;
    else scan_state.stats.pause_reasons &= ~reason;
    scan_update();
}

void ble_scan_snapshot(ble_scan_snapshot_t *snapshot) {
    if (snapshot != NULL) *snapshot = scan_state;
}
void ble_scan_visit(void (*visit)(const ble_scan_report_t *,unsigned,void *),void *context) {
    if (!visit) return;
    for (unsigned i=0;i<scan_state.stats.cache_count;i++) visit(&scan_state.reports[i],i,context);
}

void ble_scan_get_stats(ble_scan_stats_t *stats) {
    if (stats != NULL) *stats = scan_state.stats;
}

#define SCAN_DATA_VALID 1u
#define SCAN_DATA_APPLE 2u
#define SCAN_DATA_HAS_TYPE 4u

// Validate every AD structure and Apple type/length record before caching any prefix.
static unsigned scan_parse_data(const uint8_t *data, unsigned size, unsigned wanted_type) {
    unsigned result = SCAN_DATA_VALID;
    for (unsigned at = 0; at < size;) {
        unsigned length = data[at++];
        if (length == 0) {
            while (at < size) if (data[at++] != 0) return 0;
            return result;
        }
        if (length > size - at) return 0;
        unsigned end = at + length;
        if (data[at] == 0xff) {
            if (length < 3) return 0;
            if (data[at + 1] == 0x4c && data[at + 2] == 0x00) {
                result |= SCAN_DATA_APPLE;
                for (unsigned field = at + 3; field < end;) {
                    if (end - field < 2) return 0;
                    uint8_t type = data[field++];
                    unsigned bytes = data[field++];
                    if (bytes > end - field) return 0;
                    if (type == wanted_type) result |= SCAN_DATA_HAS_TYPE;
                    field += bytes;
                }
            }
        }
        at = end;
    }
    return result;
}

bool ble_scan_report_has_type(const ble_scan_report_t *report, uint8_t type) {
    if (report == NULL || report->data_length > BLE_SCAN_DATA_MAX) return false;
    return (scan_parse_data(report->data, report->data_length, type) & SCAN_DATA_HAS_TYPE) != 0;
}

static void scan_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET || packet == NULL || size == 0 ||
        packet[0] != GAP_EVENT_ADVERTISING_REPORT) return;
    if (!scan_state.stats.scan_requested) return;
    if (size < 12 || (unsigned)packet[1] + 2 != size) {
        scan_state.stats.malformed++;
        return;
    }
    unsigned length = gap_event_advertising_report_get_data_length(packet);
    if (length > BLE_SCAN_DATA_MAX || length != (unsigned)size - 12 ||
        gap_event_advertising_report_get_address_type(packet) > 3 ||
        gap_event_advertising_report_get_advertising_event_type(packet) > 4) {
        scan_state.stats.malformed++;
        return;
    }
    const uint8_t *data = gap_event_advertising_report_get_data(packet);
    unsigned parsed = scan_parse_data(data, length, 256);
    if (!(parsed & SCAN_DATA_VALID)) {
        scan_state.stats.malformed++;
        return;
    }
    if (!(parsed & SCAN_DATA_APPLE)) {
        scan_state.stats.ignored++;
        return;
    }
    ble_scan_report_t report = {0};
    gap_event_advertising_report_get_address(packet, report.address);
    report.address_type = gap_event_advertising_report_get_address_type(packet);
    report.event_type = gap_event_advertising_report_get_advertising_event_type(packet);
    report.rssi = (int8_t)gap_event_advertising_report_get_rssi(packet);
    report.rssi_valid = report.rssi != 127;
    report.received_ms = btstack_run_loop_get_time_ms();
    report.data_length = (uint8_t)length;
    memcpy(report.data, data, length);

    unsigned slot;
    for (slot = 0; slot < scan_state.stats.cache_count; ++slot) {
        ble_scan_report_t *saved = &scan_state.reports[slot];
        if (saved->address_type == report.address_type &&
            memcmp(saved->address, report.address, sizeof(report.address)) == 0) break;
    }
    if (slot < scan_state.stats.cache_count) {
        report.reports = scan_state.reports[slot].reports + 1u;
    } else {
        report.reports = 1;
        if (slot == BLE_SCAN_CACHE_SIZE) {
            slot = 0;
            for (unsigned i = 1; i < BLE_SCAN_CACHE_SIZE; ++i) {
                if ((uint32_t)(report.received_ms - scan_state.reports[i].received_ms) >
                    (uint32_t)(report.received_ms - scan_state.reports[slot].received_ms)) slot = i;
            }
            scan_state.stats.evicted++;
        } else {
            scan_state.stats.cache_count++;
        }
    }
    scan_state.reports[slot] = report;
    scan_state.stats.accepted++;
}

void ble_scan_init(void) {
    if (scan_initialized) return;
    scan_initialized = true;
    scan_state.stats.pause_reasons = BLE_SCAN_PAUSE_USB_SUSPEND;
    scan_registration.callback = scan_packet_handler;
    hci_add_event_handler(&scan_registration);
}
