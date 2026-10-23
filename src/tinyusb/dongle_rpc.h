// SPDX-License-Identifier: GPL-3.0-only
#ifndef DONGLE_RPC_H
#define DONGLE_RPC_H
#include <stdbool.h>
#include <stdint.h>
#include "dongle_client.h"
bool dongle_rpc_client_get(uint8_t client, uint8_t report[64]);
bool dongle_rpc_client_set(uint8_t client, const uint8_t report[64]);
void dongle_rpc_client_available(uint8_t client, bool available);
uint16_t dongle_rpc_get(uint8_t *buffer, uint16_t capacity);
void dongle_rpc_set(const uint8_t *buffer, uint16_t length);
void dongle_rpc_available(bool available);
void dongle_rpc_poll(void);
bool dongle_rpc_busy(void);
// Recheck client/session ownership at the radio send boundary, after IRQ revocation.
bool dongle_rpc_transport_current(void);
#endif
