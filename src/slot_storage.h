// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_SLOT_STORAGE_H
#define AIRPODS_SLOT_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#define MAC_LEN 6

// Call after cyw43_arch_init(), under the Bluetooth lock once its run loop is active.
// Existing API names are retained; new writes use BTstack TLV rather than a raw page.
uint8_t read_uint8_last_flash(void);
bool write_uint8_last_flash(uint8_t value);
void read_slot1_mac(uint8_t mac[MAC_LEN]);
bool write_slot1_mac(const uint8_t mac[MAC_LEN]);
void read_slot2_mac(uint8_t mac[MAC_LEN]);
bool write_slot2_mac(const uint8_t mac[MAC_LEN]);

#endif
