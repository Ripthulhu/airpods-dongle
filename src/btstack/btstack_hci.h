//
// Created by  on 8/10/23.
//

#include <stdint.h>
#include "btstack.h"


#ifndef PICOW_USB_BT_AUDIO_BTSTACK_HCI_H
#define PICOW_USB_BT_AUDIO_BTSTACK_HCI_H

void bt_hci_init(void);

const char * get_device_addr_string();
bd_addr_t * get_device_addr();
bd_addr_t * get_device_addr_from_list(uint8_t i);
void get_link_keys();
// Call after the selected pairing slot and key have been cleared successfully.
void bt_hci_retire_selected_peer(void);

// Phone identity is separate from the two selected AirPods slots.
bool bt_hci_phone_pairing_start(void);
bool bt_hci_phone_pairing_busy(void);
bool bt_hci_phone_pairing_active(void);
bool bt_hci_phone_peer_allowed(const uint8_t address[6]);
bool bt_hci_phone_peer_connected(const uint8_t address[6]);
// Nonzero token for this authenticated phone ACL; changes across reconnects.
uint32_t bt_hci_phone_session(const uint8_t address[6]);
bool bt_hci_phone_address(uint8_t address[6]);
void bt_hci_phone_poll(void);
// Selected AirPods Game Mode link policy; retiring live links restore their confirmed packet baseline.
// Media alone is flushable; signaling/AACP and the phone ACL keep their existing policy.
void bt_hci_airpods_flush_timeout(bool low_latency);
// Temporarily compare 200-ms controller flush with unchanged Game Mode; expires after 120 seconds.
bool bt_hci_airpods_flush_comparison(bool enabled);

typedef struct {
    uint32_t events, packets, max_active_gap_ms;
} bt_hci_credit_role_t;
typedef struct {
    uint32_t raw_acl_count, raw_acl_length, effective_acl_limit;
    uint32_t free_acl_slots, outstanding_acl_packets, packet_buffer_reserved;
    bt_hci_credit_role_t airpods, phone;
    uint16_t airpods_handle, phone_handle;
    // Actual local HCI role: 0 master, 1 slave, 255 unavailable.
    uint8_t airpods_local_role, phone_local_role;
    uint16_t media_flush_desired_ms, media_flush_confirmed_ms;
    uint8_t media_flush_status;
    bool media_flush_pending, media_flush_failed;
    // FFFF unknown. Requested is SDK-derived; normal/confirmed require actual packet-type events.
    uint16_t media_packet_requested, media_packet_normal, media_packet_desired, media_packet_confirmed;
    uint8_t media_packet_status;
    bool media_packet_pending, media_packet_failed;
} bt_hci_credit_diagnostics_t;
// Read under the Bluetooth lock; false means Read Buffer Size has not succeeded.
bool bt_hci_credit_diagnostics(bt_hci_credit_diagnostics_t *out);

enum { BT_RADIO_RSSI=1, BT_RADIO_QUALITY=2, BT_RADIO_TX_POWER=4,
       BT_RADIO_MAX_TX_POWER=8, BT_RADIO_AFH_MAP=16 };
typedef struct {
    uint32_t epoch, flush_events, completed_packets;
    // RSSI, quality, current power, maximum power, AFH map; FFFFFFFF means unknown.
    uint32_t age_ms[5];
    uint16_t handle;
    uint8_t valid, failed, unavailable, quality, afh_mode, afh_map[10];
    int8_t rssi, tx_power, max_tx_power;
    bool active, pending;
} bt_hci_radio_diagnostics_t;
// RSSI is BR/EDR-relative dB. Completed packets include control and discarded data, not proven delivery.
bool bt_hci_radio_diagnostics(bt_hci_radio_diagnostics_t *out);
void bt_hci_radio_active(bool active);


#endif //PICOW_USB_BT_AUDIO_BTSTACK_HCI_H
