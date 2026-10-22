// SPDX-License-Identifier: GPL-3.0-only
#ifndef DONGLE_CONTROL_H
#define DONGLE_CONTROL_H
#include <stdbool.h>
#include <stdint.h>
// USB IRQ context: reports exclude the ID already handled by TinyUSB.
uint16_t dongle_control_get(uint8_t *buffer, uint16_t requested);
void dongle_control_set(const uint8_t *buffer, uint16_t size);
void dongle_control_available(bool available);
// Core 0 with Bluetooth async lock, never from core 1 or USB IRQ.
void dongle_control_poll(void);
uint32_t dongle_control_generation(void);
#endif
