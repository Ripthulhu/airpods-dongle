// SPDX-License-Identifier: GPL-3.0-only
#ifndef TEST_CDC_SYNC_H
#define TEST_CDC_SYNC_H
#include <stdint.h>
uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t previous);
#endif
