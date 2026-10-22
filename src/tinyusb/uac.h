#ifndef AIRPODS_USB_AUDIO_H
#define AIRPODS_USB_AUDIO_H

#include <stdbool.h>

void tinyusb_main(void);
void tinyusb_task(void);
void tinyusb_control_task(void);

// Latest USB capture intent; consumed only by the main loop.
bool usb_mic_take_request(bool *wanted);
bool usb_mic_requested(void);

#endif
