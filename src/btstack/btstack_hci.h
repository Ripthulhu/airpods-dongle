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
void bt_hci_phone_pairing_start(void);
bool bt_hci_phone_pairing_active(void);
bool bt_hci_phone_peer_allowed(const uint8_t address[6]);
bool bt_hci_phone_peer_connected(const uint8_t address[6]);
void bt_hci_phone_poll(void);


#endif //PICOW_USB_BT_AUDIO_BTSTACK_HCI_H
