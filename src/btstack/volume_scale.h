// SPDX-License-Identifier: GPL-3.0-only
#ifndef VOLUME_SCALE_H
#define VOLUME_SCALE_H

#include <stdint.h>

// Compatibility scale between USB [-50, 0] dB and AVRCP [0, 127], not calibrated gain.
static inline uint8_t usb_volume_to_bt(int16_t db256) {
    if (db256 <= -12800) return 0;
    if (db256 >= 0) return 127;
    return (uint8_t)((((int32_t)db256 + 12800) * 127 + 6400) / 12800);
}

static inline int16_t bt_volume_to_usb(uint8_t level) {
    if (level >= 127) return 0;
    return -(int16_t)(((127u - level) * 12800u + 63u) / 127u);
}

#endif
