// SPDX-License-Identifier: GPL-3.0-only
#ifndef TEST_CDC_TUSB_H
#define TEST_CDC_TUSB_H
#include <stdbool.h>
#include <stdint.h>
bool tud_cdc_connected(void);
uint32_t tud_cdc_write_available(void);
uint32_t tud_cdc_write(const void *buffer, uint32_t count);
uint32_t tud_cdc_write_flush(void);
#endif
