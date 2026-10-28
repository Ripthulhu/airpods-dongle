// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_MEDIA_CONTROL_H
#define AIRPODS_MEDIA_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

enum usb_media_command {
    USB_MEDIA_PLAY,
    USB_MEDIA_PAUSE,
    USB_MEDIA_NEXT,
    USB_MEDIA_PREVIOUS,
    USB_MEDIA_STOP,
    USB_MEDIA_VOLUME_UP,
    USB_MEDIA_VOLUME_DOWN,
};

// Called on the Bluetooth press event. The USB task supplies the release.
// False means the USB host is unavailable or the eight-command queue is full.
bool usb_media_enqueue(enum usb_media_command command);
uint32_t usb_media_generation(void);
// Completed current-session volume presses, not queued reports or OS application.
uint32_t usb_media_volume_sent(void);
void usb_media_cancel_volume(void);

// USB callbacks/task only; never call TinyUSB from the Bluetooth run loop.
void usb_media_set_available(bool available);
void usb_media_task(void);

#endif
