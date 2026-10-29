#ifndef AIRPODS_USB_AUDIO_H
#define AIRPODS_USB_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

void tinyusb_main(void);
void tinyusb_task(void);
void tinyusb_control_task(void);
// Main publishes Bluetooth ownership; USB service re-enumerates asynchronously.
void usb_audio_set_bluetooth(bool requested);
bool usb_audio_available(void);

// Latest USB capture intent; consumed only by the main loop.
bool usb_mic_take_request(bool *wanted);
bool usb_mic_requested(void);
bool usb_host_active(void);
uint32_t usb_volume_set_revision(void);
uint32_t usb_volume_set_generation(void);
uint32_t usb_volume_set_receipt(void);

#endif
