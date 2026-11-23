// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define USB_MOTION_USAGE 0x03
#define USB_MOTION_INSTANCE 1
#define USB_MOTION_INPUT_ID 1
#define USB_MOTION_RATE_ID 2
#define USB_MOTION_SIZE 50

void usb_motion_reset(void);
void usb_motion_set_rate(uint8_t rate);
uint8_t usb_motion_rate(void);
uint32_t usb_motion_lease(uint8_t *rate);
void usb_motion_invalidate(void);
void usb_motion_publish(uint32_t lease, uint32_t now, const uint8_t report[USB_MOTION_SIZE]);
void usb_motion_task(void);
