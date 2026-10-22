// SPDX-License-Identifier: GPL-3.0-only
#ifndef TEST_MEDIA_TUSB_H
#define TEST_MEDIA_TUSB_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { HID_REPORT_TYPE_INPUT = 1, HID_REPORT_TYPE_OUTPUT = 2 } hid_report_type_t;
bool tud_hid_ready(void);
bool tud_hid_report(uint8_t report_id, const void *report, uint16_t length);
#endif
