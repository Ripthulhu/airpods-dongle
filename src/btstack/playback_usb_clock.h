// SPDX-License-Identifier: GPL-3.0-only
#ifndef AIRPODS_PLAYBACK_USB_CLOCK_H
#define AIRPODS_PLAYBACK_USB_CLOCK_H
#include <stdbool.h>
#include <stdint.h>

#define PLAYBACK_USB_RESERVE 960u
// Retain 40 ms of send stalls plus encoding and one scheduler-phase unit.
#define PLAYBACK_USB_ACTIVE_LIMIT 3840u
#define PLAYBACK_USB_DEBT_LIMIT 2880u
typedef struct { int32_t fill_q8; uint32_t rate_q16; } playback_usb_clock_t;
static inline void playback_usb_clock_reset(playback_usb_clock_t *s) {
    s->fill_q8=PLAYBACK_USB_RESERVE*256; s->rate_q16=65536;
}
static inline uint32_t playback_usb_clock_update(playback_usb_clock_t *s,
    uint32_t remaining, uint32_t frames, bool frozen) {
    if (frozen) return s->rate_q16;
    s->fill_q8+=((int32_t)remaining*256-s->fill_q8)*(int32_t)frames/(480*32);
    int32_t error=s->fill_q8-(int32_t)PLAYBACK_USB_RESERVE*256;
    // A 2.5 ms dead band ignores packet phase; +/-64 Q16 covers +/-500 ppm clocks.
    s->rate_q16=error>120*256?65600u:error< -120*256?65472u:65536u;
    return s->rate_q16;
}
#endif
