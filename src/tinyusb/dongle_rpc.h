// SPDX-License-Identifier: GPL-3.0-only
#ifndef DONGLE_RPC_H
#define DONGLE_RPC_H
#include <stdbool.h>
#include <stdint.h>
uint16_t dongle_rpc_get(uint8_t *buffer, uint16_t capacity);
void dongle_rpc_set(const uint8_t *buffer, uint16_t length);
void dongle_rpc_available(bool available);
void dongle_rpc_poll(void);
bool dongle_rpc_busy(void);
#endif
