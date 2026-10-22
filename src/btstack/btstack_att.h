// SPDX-License-Identifier: GPL-3.0-only
#ifndef DONGLE_BTSTACK_ATT_H
#define DONGLE_BTSTACK_ATT_H
#include <stdbool.h>
#include <stdint.h>

enum { DONGLE_ATT_MAX = 512 };
typedef enum { DONGLE_ATT_WAIT, DONGLE_ATT_DONE, DONGLE_ATT_FAILED } dongle_att_result_t;
// Core 0 only, under the Bluetooth async lock. Requests never establish a new ACL.
bool dongle_att_request(const uint8_t *packet, uint16_t length);
void dongle_att_poll(void);
dongle_att_result_t dongle_att_take(uint8_t *out, uint16_t capacity, uint16_t *length);
void dongle_att_cancel(void);
#endif
