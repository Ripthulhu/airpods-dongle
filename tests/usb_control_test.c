// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/tinyusb/mic_request.h"
#include "../src/tinyusb/audio_control_validation.h"

int main(void) {
    usb_mic_request_t request = {0};
    bool wanted = true;
    assert(!usb_mic_request_take(&request, &wanted));
    usb_mic_request_set(&request, true);
    usb_mic_request_set(&request, false);
    assert(usb_mic_request_take(&request, &wanted) && !wanted);
    usb_mic_request_set(&request, true);
    usb_mic_request_set(&request, false);
    usb_mic_request_set(&request, true);
    assert(usb_mic_request_take(&request, &wanted) && wanted);
    assert(usb_mic_request_wanted(&request));
    assert(!usb_mic_request_take(&request, &wanted));
    usb_mic_request_set(&request, false);
    assert(!usb_mic_request_wanted(&request));
    assert(usb_mic_request_take(&request, &wanted) && !wanted);

    const uint8_t rate[] = {0x00, 0xfa, 0x00, 0x00};
    assert(usb_audio_fixed_rate(rate, 4, 64000));
    assert(!usb_audio_fixed_rate(rate, 3, 64000));
    assert(!usb_audio_fixed_rate(rate, 4, 48000));
    assert(!usb_audio_fixed_rate(NULL, 4, 64000));
    int16_t volume;
    const uint8_t min_volume[] = {0, 0xce}, max_volume[] = {0, 0};
    const uint8_t too_quiet[] = {0xff, 0xcd}, too_loud[] = {1, 0};
    assert(usb_audio_volume_value(min_volume, 2, &volume) && volume == -12800);
    assert(usb_audio_volume_value(max_volume, 2, &volume) && volume == 0);
    assert(!usb_audio_volume_value(too_quiet, 2, &volume));
    assert(!usb_audio_volume_value(too_loud, 2, &volume));
    assert(!usb_audio_volume_value(max_volume, 1, &volume));
    puts("USB request ordering and control validation: passed");
}
