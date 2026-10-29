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
} bt_hci_credit_diagnostics_t;
// Read under the Bluetooth lock; false means Read Buffer Size has not succeeded.
bool bt_hci_credit_diagnostics(bt_hci_credit_diagnostics_t *out);


#endif //PICOW_USB_BT_AUDIO_BTSTACK_HCI_H
