// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/btstack/btstack_ble_scan.c"

_Static_assert(sizeof(ble_scan_report_t) <= 56, "Keep advertisements without redundant type storage");
_Static_assert(sizeof(ble_scan_stats_t) <= 28, "Aggregate diagnostics must not copy report payloads");

static unsigned registrations, starts, stops, params, duplicate_calls;
static uint32_t now;
static bool supports_le;
static btstack_packet_callback_registration_t *callback;

void hci_add_event_handler(btstack_packet_callback_registration_t *registration) {
    registrations++;
    callback = registration;
}
void gap_set_scan_params(uint8_t type, uint16_t interval, uint16_t window, uint8_t policy) {
    assert(type == 0 && interval == 480 && window == 48 && policy == 0);
    params++;
}
void gap_set_scan_duplicate_filter(bool enabled) { assert(!enabled); duplicate_calls++; }
void gap_start_scan(void) { starts++; }
void gap_stop_scan(void) { stops++; }
bool hci_le_supported(void) { return supports_le; }
uint32_t btstack_run_loop_get_time_ms(void) { return now; }

static void reset(void) {
    memset(&scan_state, 0, sizeof(scan_state));
    scan_initialized = false;
    uptime_ms = 0;
    registrations = starts = stops = params = duplicate_calls = now = 0;
    supports_le = true;
    callback = NULL;
    ble_scan_init();
    ble_scan_controller_ready(true);
    assert(starts == 0 && !scan_state.stats.scan_requested); // Mount policy must explicitly allow scanning.
    ble_scan_set_paused(BLE_SCAN_PAUSE_USB_SUSPEND, false);
}

static uint16_t event(uint8_t packet[64], uint8_t address, uint8_t address_type,
                      const uint8_t *data, unsigned size) {
    assert(size <= 52);
    memset(packet, 0, 64);
    packet[0] = GAP_EVENT_ADVERTISING_REPORT;
    packet[1] = (uint8_t)(10 + size);
    packet[2] = 3;
    packet[3] = address_type;
    packet[4] = address;
    packet[9] = 0xc0;
    packet[10] = (uint8_t)-42;
    packet[11] = (uint8_t)size;
    if (size) memcpy(packet + 12, data, size);
    return (uint16_t)(12 + size);
}

static void receive(uint8_t address, uint8_t address_type, const uint8_t *data, unsigned size) {
    uint8_t packet[64];
    uint16_t length = event(packet, address, address_type, data, size);
    callback->callback(HCI_EVENT_PACKET, 0, packet, length);
}

static void lifecycle(void) {
    reset();
    ble_scan_init();
    ble_scan_controller_ready(true);
    assert(registrations == 1 && starts == 1 && params == 1 && duplicate_calls == 1);
    ble_scan_set_paused(BLE_SCAN_PAUSE_MIC, true);
    ble_scan_set_paused(BLE_SCAN_PAUSE_USB_SUSPEND, true);
    ble_scan_set_paused(BLE_SCAN_PAUSE_MIC, false);
    assert(stops == 1 && starts == 1);
    ble_scan_set_paused(1u << 31, false);
    assert(starts == 1);
    ble_scan_set_paused(BLE_SCAN_PAUSE_USB_SUSPEND, false);
    assert(starts == 2);
    const uint8_t apple[] = {5, 0xff, 0x4c, 0, 0x12, 0};
    receive(1, 1, apple, sizeof(apple));
    assert(scan_state.stats.cache_count == 1);
    ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, true);
    receive(2, 1, apple, sizeof(apple));
    assert(scan_state.stats.cache_count == 1 && scan_state.stats.accepted == 1);
    ble_scan_controller_ready(false);
    assert(scan_state.stats.cache_count == 0 && stops == 2 && !scan_state.stats.scan_requested);
    ble_scan_set_paused(BLE_SCAN_PAUSE_INQUIRY, false);
    assert(starts == 2);
    ble_scan_controller_ready(true);
    assert(starts == 3);
    ble_scan_controller_ready(false);
    ble_scan_controller_ready(false);
    assert(stops == 3 && !scan_state.stats.controller_ready);
    supports_le = false;
    ble_scan_controller_ready(true);
    assert(starts == 3 && !scan_state.stats.scan_requested && !scan_state.stats.le_supported);
    supports_le = true;
    ble_scan_controller_ready(true);
    assert(starts == 4 && scan_state.stats.scan_requested && scan_state.stats.le_supported);
}

static void retain_exact_reports(void) {
    reset();
    uint8_t proximity[31] = {30, 0xff, 0x4c, 0, 7, 25};
    for (unsigned i = 6; i < sizeof(proximity); ++i) proximity[i] = (uint8_t)i;
    now = 1234;
    receive(1, 1, proximity, sizeof(proximity));
    ble_scan_snapshot_t snapshot;
    ble_scan_snapshot(&snapshot);
    ble_scan_report_t *report = &snapshot.reports[0];
    assert(snapshot.stats.cache_count == 1 && snapshot.stats.accepted == 1 && snapshot.stats.scan_requested);
    assert(report->address[0] == 0xc0 && report->address[5] == 1 && report->address_type == 1);
    assert(report->event_type == 3 && report->rssi == -42 && report->rssi_valid);
    assert(report->received_ms == now && report->reports == 1 && report->data_length == 31);
    assert(memcmp(report->data, proximity, 31) == 0 && ble_scan_report_has_type(report, 7));
    assert(!ble_scan_report_has_type(report, 0x12));

    const uint8_t multi[] = {10, 0xff, 0x4c, 0, 0x12, 1, 0x55, 0xff, 2, 0xaa, 0xbb};
    now++;
    receive(1, 1, multi, sizeof(multi));
    receive(1, 0, multi, sizeof(multi));
    assert(scan_state.stats.cache_count == 2 && scan_state.reports[0].reports == 2);
    assert(!ble_scan_report_has_type(&scan_state.reports[0], 7));
    assert(ble_scan_report_has_type(&scan_state.reports[0], 0x12));
    assert(ble_scan_report_has_type(&scan_state.reports[0], 0xff));
    uint8_t packet[64];
    uint16_t length = event(packet, 1, 0, multi, sizeof(multi));
    packet[10] = 127;
    callback->callback(HCI_EVENT_PACKET, 0, packet, length);
    assert(!scan_state.reports[1].rssi_valid && scan_state.reports[1].rssi == 127);
    ble_scan_stats_t stats;
    ble_scan_get_stats(&stats);
    ble_scan_snapshot(&snapshot);
    assert(memcmp(&stats, &snapshot.stats, sizeof(stats)) == 0);
    stats.accepted = 0;
    assert(scan_state.stats.accepted == 4);
    ble_scan_get_stats(NULL);
    ble_scan_snapshot(NULL);
}

static void reject_malformed(void) {
    reset();
    const uint8_t good[] = {5, 0xff, 0x4c, 0, 7, 0};
    uint8_t packet[64];
    uint16_t length = event(packet, 1, 1, good, sizeof(good));
    for (unsigned size = 1; size < 12; ++size) {
        callback->callback(HCI_EVENT_PACKET, 0, packet, (uint16_t)size);
    }
    callback->callback(HCI_EVENT_PACKET, 0, NULL, 20);
    callback->callback(HCI_EVENT_PACKET, 0, packet, 0);
    assert(scan_state.stats.malformed == 11);
    packet[1]--;
    callback->callback(HCI_EVENT_PACKET, 0, packet, length);
    packet[1]++;
    packet[11]++;
    callback->callback(HCI_EVENT_PACKET, 0, packet, length);
    packet[11]--;
    packet[3] = 0xff;
    callback->callback(HCI_EVENT_PACKET, 0, packet, length);
    packet[3] = 1;
    packet[2] = 5;
    callback->callback(HCI_EVENT_PACKET, 0, packet, length);
    assert(scan_state.stats.malformed == 15);
    uint8_t oversized[32] = {0};
    receive(1, 1, oversized, sizeof(oversized));
    const uint8_t bad[][12] = {
        {12, 0xff},                       // AD runs past complete packet.
        {1, 0xff},                        // Manufacturer ID missing.
        {2, 0xff, 0x4c},                  // Manufacturer ID truncated.
        {4, 0xff, 0x4c, 0, 7},            // Apple TLV length missing.
        {6, 0xff, 0x4c, 0, 7, 3, 1},      // Apple payload truncated.
        {5, 0xff, 0x4c, 0, 7, 0, 4, 1},   // Good Apple prefix, bad AD tail.
        {5, 0xff, 0x4c, 0, 7, 0, 0, 1},   // Nonzero bytes after AD terminator.
    };
    const unsigned sizes[] = {2, 2, 3, 5, 7, 8, 8};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i)
        receive(1, 1, bad[i], sizes[i]);
    assert(scan_state.stats.malformed == 23 && scan_state.stats.cache_count == 0 && scan_state.stats.accepted == 0);
    const uint8_t other[] = {3, 0xff, 0x34, 0x12, 0, 0};
    receive(1, 1, other, sizeof(other));
    receive(1, 1, NULL, 0);
    assert(scan_state.stats.ignored == 2 && scan_state.stats.cache_count == 0);
    const uint8_t padded[] = {5, 0xff, 0x4c, 0, 7, 0, 0, 0};
    receive(1, 1, padded, sizeof(padded));
    assert(scan_state.stats.accepted == 1);
    ble_scan_report_t report = {0};
    memcpy(report.data, bad[6], sizes[6]);
    report.data_length = (uint8_t)sizes[6];
    assert(!ble_scan_report_has_type(&report, 7)); // A matching prefix cannot hide a malformed tail.
    report.data_length = BLE_SCAN_DATA_MAX + 1;
    assert(!ble_scan_report_has_type(&report, 7));
    assert(!ble_scan_report_has_type(NULL, 7));
}

static void bounded_cache_and_wrap(void) {
    reset();
    const uint8_t offline[] = {5, 0xff, 0x4c, 0, 0x12, 0};
    now = UINT32_MAX - 5u;
    for (unsigned i = 1; i <= BLE_SCAN_CACHE_SIZE; ++i) {
        receive((uint8_t)i, 1, offline, sizeof(offline));
        now++;
    }
    assert(scan_state.stats.cache_count == BLE_SCAN_CACHE_SIZE && scan_state.stats.evicted == 0);
    receive(2, 1, offline, sizeof(offline)); // Refresh this exact address, even after clock wrap.
    now++;
    receive(9, 1, offline, sizeof(offline));
    assert(scan_state.stats.cache_count == BLE_SCAN_CACHE_SIZE && scan_state.stats.evicted == 1);
    assert(scan_state.reports[0].address[5] == 9 && scan_state.reports[1].reports == 2);
    now++;
    receive(10, 1, offline, sizeof(offline));
    assert(scan_state.stats.evicted == 2 && scan_state.reports[2].address[5] == 10);
    ble_scan_snapshot_t snapshot;
    ble_scan_snapshot(&snapshot);
    memset(snapshot.reports, 0, sizeof(snapshot.reports));
    assert(scan_state.reports[0].address[5] == 9); // Snapshot does not expose mutable cache storage.
}

int main(void) {
    lifecycle();
    retain_exact_reports();
    reject_malformed();
    bounded_cache_and_wrap();
    reset();now=UINT32_MAX-60000;
    ble_scan_diagnostic_pause(true);assert(!scan_state.stats.scan_requested);
    now+=119999;ble_scan_set_paused(BLE_SCAN_PAUSE_MIC,false);
    assert(!scan_state.stats.scan_requested);
    ++now;ble_scan_set_paused(BLE_SCAN_PAUSE_MIC,false);
    assert(scan_state.stats.scan_requested);
    puts("BLE passive lifecycle, complete TLV validation and bounded identity-free cache: passed");
    return 0;
}
