#ifndef AIRPODS_USB_AUDIO_H
#define AIRPODS_USB_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

void tinyusb_main(void);
void tinyusb_task(void);
// USB service context only, after processing this tick's interface changes.
uint32_t tinyusb_service_interval_us(void);
void tinyusb_control_task(void);
// Main publishes headset presence and input ownership; USB service re-enumerates.
void usb_audio_set_route(bool airpods_connected, bool bluetooth_requested);
bool usb_audio_available(void);

// Latest USB capture intent; consumed only by the main loop.
bool usb_mic_take_request(bool *wanted);
bool usb_mic_requested(void);
bool usb_host_active(void);
uint32_t usb_volume_set_revision(void);
uint32_t usb_volume_set_generation(void);
uint32_t usb_volume_set_receipt(void);

#endif
