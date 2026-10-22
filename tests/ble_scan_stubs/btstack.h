#ifndef TEST_BLE_SCAN_BTSTACK_H
#define TEST_BLE_SCAN_BTSTACK_H
#include <stdbool.h>
#include <stdint.h>

#define HCI_EVENT_PACKET 4
#define GAP_EVENT_ADVERTISING_REPORT 0xdau
typedef uint8_t bd_addr_t[6];
typedef struct {
    void (*callback)(uint8_t, uint16_t, uint8_t *, uint16_t);
} btstack_packet_callback_registration_t;

void hci_add_event_handler(btstack_packet_callback_registration_t *registration);
bool hci_le_supported(void);
void gap_set_scan_params(uint8_t type, uint16_t interval, uint16_t window, uint8_t policy);
void gap_set_scan_duplicate_filter(bool enabled);
void gap_start_scan(void);
void gap_stop_scan(void);
uint32_t btstack_run_loop_get_time_ms(void);

// Offsets and reversed addresses match the pinned SDK's btstack_event.h.
static inline uint8_t gap_event_advertising_report_get_advertising_event_type(const uint8_t *p) { return p[2]; }
static inline uint8_t gap_event_advertising_report_get_address_type(const uint8_t *p) { return p[3]; }
static inline void gap_event_advertising_report_get_address(const uint8_t *p, bd_addr_t address) {
    for (unsigned i = 0; i < 6; ++i) address[i] = p[9 - i];
}
static inline uint8_t gap_event_advertising_report_get_rssi(const uint8_t *p) { return p[10]; }
static inline uint8_t gap_event_advertising_report_get_data_length(const uint8_t *p) { return p[11]; }
static inline const uint8_t *gap_event_advertising_report_get_data(const uint8_t *p) { return p + 12; }
#endif
