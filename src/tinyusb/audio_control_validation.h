// SPDX-License-Identifier: GPL-3.0-only
#ifndef USB_AUDIO_CONTROL_VALIDATION_H
#define USB_AUDIO_CONTROL_VALIDATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool usb_audio_fixed_rate(const uint8_t *data, size_t length, uint32_t rate) {
    if (data == NULL || length != 4) return false;
    uint32_t requested = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    return requested == rate;
}

static inline bool usb_audio_volume_value(const uint8_t *data, size_t length, int16_t *volume) {
    if (data == NULL || length != 2 || volume == NULL) return false;
    int32_t requested = (uint32_t)data[0] | ((uint32_t)data[1] << 8);
    if (requested >= 32768) requested -= 65536;
    if (requested < -50 * 256 || requested > 0) return false;
    *volume = (int16_t)requested;
    return true;
}

#endif
