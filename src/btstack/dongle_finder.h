// SPDX-License-Identifier: GPL-3.0-only
#ifndef DONGLE_FINDER_H
#define DONGLE_FINDER_H
#include <stdbool.h>
#include <stdint.h>
uint8_t dongle_finder_start(uint8_t client);
uint8_t dongle_finder_stop(uint8_t client);
uint16_t dongle_finder_snapshot(uint8_t client,uint8_t *out,uint16_t capacity);
void dongle_finder_disconnect(uint8_t client);
void dongle_finder_poll(void);
bool dongle_finder_active(void);
#endif
