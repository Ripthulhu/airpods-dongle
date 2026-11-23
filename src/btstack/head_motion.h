// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../realtime_metrics.h"

void head_motion_reset(void);
void head_motion_role(const uint8_t role[4]);
bool head_motion_receive(const uint8_t *packet, uint16_t length, uint32_t now);
// Read under the Bluetooth context lock. Counts consumed RX callbacks, including fragments.
realtime_metric_t head_motion_receive_work(void);
void head_motion_poll(uint32_t now, uint8_t max_rate, bool (*send)(uint8_t *, uint16_t));
